// Environment lighting: the DFG lookup table, the physical atmosphere
// (transmittance / multiple-scattering / sky-view LUTs), HDRI loading and
// caching, filtering the current sky into layer 0 of the probe cubemap
// array (GGX prefiltered specular + L2 spherical harmonics for diffuse),
// and the procedural terrain layer textures.
#include "Render/RendererImpl.hpp"

#include "Hollow/Assets/HdrLoader.hpp"
#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace Hollow {

namespace {

std::uint64_t hash_floats(std::initializer_list<float> values) {
    std::uint64_t h = 1469598103934665603ull;
    for (float v : values) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        h = (h ^ bits) * 1099511628211ull;
    }
    return h;
}

void dispatch_2d(int width, int height) {
    glDispatchCompute(gfx::groups(width, 8), gfx::groups(height, 8), 1);
}

} // namespace

void Renderer::Impl::init_environment() {
    dfg_lut = gfx::Texture(gfx::tex2d(GL_RGBA16F, 128, 128), "DFG LUT");
    bake_dfg_lut();
    probe_array = gfx::Texture(gfx::cubemap_array(GL_RGBA16F, kEnvironmentSize, kMaxProbes + 1, gfx::mip_count(kEnvironmentSize, kEnvironmentSize)), "ProbeArray");
    for (gfx::Texture& t : volume_sh) {
        t = gfx::Texture(gfx::tex3d(GL_RGBA16F, 1, 1, 1), "IrradianceVolumeSH");
        const float zero[4] = {0, 0, 0, 0};
        glClearTexImage(t.id(), 0, GL_RGBA, GL_FLOAT, zero);
    }
    decal_array = gfx::Texture(gfx::tex2d_array(GL_RGBA8, kDecalLayerSize, kDecalLayerSize, kMaxDecalLayers, gfx::mip_count(kDecalLayerSize, kDecalLayerSize)), "DecalArray");
    for (gfx::Texture& t : fog_history) {
        t = gfx::Texture(gfx::tex3d(GL_RGBA16F, 1, 1, 1), "FogVolume");
        const float clear[4] = {0, 0, 0, 1};
        glClearTexImage(t.id(), 0, GL_RGBA, GL_FLOAT, clear);
    }
    // Noise: per-channel white noise (dithering, rotations).
    {
        noise = gfx::Texture(gfx::tex2d(GL_RGBA8, 64, 64), "Noise");
        std::vector<std::uint8_t> data(64 * 64 * 4);
        std::uint32_t state = 0x9E3779B9u;
        for (auto& v : data) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            v = static_cast<std::uint8_t>(state & 0xFF);
        }
        glTextureSubImage2D(noise.id(), 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    }
    transmittance_lut = gfx::Texture(gfx::tex2d(GL_RGBA16F, 256, 64), "TransmittanceLUT");
    multiscatter_lut = gfx::Texture(gfx::tex2d(GL_RGBA16F, 32, 32), "MultiScatterLUT");
    sky_view_lut = gfx::Texture(gfx::tex2d(GL_RGBA16F, 192, 108), "SkyViewLUT");
    sky_capture = gfx::Texture(gfx::cubemap(GL_RGBA16F, 64, gfx::mip_count(64, 64)), "SkyCapture");
    capture_color = gfx::Texture(gfx::cubemap(GL_RGBA16F, 128, gfx::mip_count(128, 128)), "ProbeCapture");
    capture_depth = gfx::Texture(gfx::tex2d(GL_DEPTH_COMPONENT32F, 128, 128), "ProbeCaptureDepth");
    exposure_buffer = gfx::Buffer(1024 * sizeof(std::uint32_t), GL_DYNAMIC_STORAGE_BIT, nullptr, "Exposure");
    for (gfx::Buffer& b : exposure_readback) {
        b = gfx::Buffer(4 * sizeof(float), GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT, nullptr, "ExposureReadback");
    }
    ensure_terrain_textures(nullptr);
    // Layer 0 starts as a neutral gray sky until the first update.
    const float gray[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int level = 0; level < probe_array.desc().mips; ++level) {
        glClearTexImage(probe_array.id(), level, GL_RGBA, GL_FLOAT, gray);
    }
    sky_sh = {};
}

void Renderer::Impl::destroy_environment() {
    environments.clear();
    active_environment = -1;
    dfg_lut.reset();
    probe_array.reset();
    for (gfx::Texture& t : volume_sh) t.reset();
    for (gfx::Texture& t : fog_history) t.reset();
    for (gfx::Texture& t : taa_history) t.reset();
    decal_array.reset();
    noise.reset();
    transmittance_lut.reset();
    multiscatter_lut.reset();
    sky_view_lut.reset();
    sky_capture.reset();
    capture_color.reset();
    capture_depth.reset();
    terrain_layers.reset();
    terrain_normals.reset();
    grading_lut.reset();
    lens_dirt.reset();
    output.reset();
    cascades.reset();
    atlas.reset();
    exposure_buffer.reset();
    for (gfx::Buffer& b : exposure_readback) b.reset();
}

void Renderer::Impl::bake_dfg_lut() {
    const ProgramId program = shaders.compute("brdf_lut.comp");
    shaders.use(program);
    glBindImageTexture(0, dfg_lut.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    dispatch_2d(dfg_lut.width(), dfg_lut.height());
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

// --- Filtering ----------------------------------------------------------------------------

void Renderer::Impl::filter_cubemap_into(GLuint source, int source_size, int layer, float rotation, float scale) {
    const ProgramId program = shaders.compute("prefilter.comp");
    shaders.use(program);
    gfx::bind_texture(0, source, gfx::SamplerKind::LinearMipClamp);
    glUniform1f(shaders.uniform(program, "u_source_size"), static_cast<float>(source_size));
    glUniform1i(shaders.uniform(program, "u_layer"), layer);
    glUniform1f(shaders.uniform(program, "u_rotation"), rotation);
    glUniform1f(shaders.uniform(program, "u_scale"), scale);
    const int mips = probe_array.desc().mips;
    for (int level = 0; level < mips; ++level) {
        const int size = std::max(1, kEnvironmentSize >> level);
        const float roughness = mips > 1 ? static_cast<float>(level) / static_cast<float>(mips - 1) : 0.0f;
        glUniform1f(shaders.uniform(program, "u_roughness"), roughness);
        glBindImageTexture(0, probe_array.id(), level, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gfx::groups(size, 8), gfx::groups(size, 8), 6);
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

std::array<Vec4, 9> Renderer::Impl::project_sh(GLuint cubemap, int size, float rotation) {
    // Read back a small mip and project on the CPU (rare: on sky changes).
    int level = 0;
    while ((size >> level) > 16) {
        ++level;
    }
    const int n = std::max(1, size >> level);
    std::vector<float> pixels(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * 6 * 4);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glGetTextureImage(cubemap, level, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(pixels.size() * sizeof(float)), pixels.data());
    std::array<Vec3, 9> sh{};
    float total_weight = 0.0f;
    const float c = std::cos(-rotation);
    const float s = std::sin(-rotation);
    for (int face = 0; face < 6; ++face) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
                Vec3 d;
                switch (face) {
                case 0: d = {1.0f, -v, -u}; break;
                case 1: d = {-1.0f, -v, u}; break;
                case 2: d = {u, 1.0f, v}; break;
                case 3: d = {u, -1.0f, -v}; break;
                case 4: d = {u, -v, 1.0f}; break;
                default: d = {-u, -v, -1.0f}; break;
                }
                const float weight = 4.0f / (std::pow(1.0f + u * u + v * v, 1.5f) * static_cast<float>(n * n));
                d = normalize(d);
                // The environment is rotated by `rotation`; the radiance seen
                // along world direction w comes from texel R(-rotation) w, so
                // texel direction d contributes to world direction R(+) d.
                const Vec3 w{c * d.x - s * d.z, d.y, s * d.x + c * d.z};
                const std::size_t index = ((static_cast<std::size_t>(face) * static_cast<std::size_t>(n) + static_cast<std::size_t>(y)) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)) * 4;
                const Vec3 radiance{pixels[index], pixels[index + 1], pixels[index + 2]};
                const float basis[9] = {
                    0.282095f,
                    0.488603f * w.y,
                    0.488603f * w.z,
                    0.488603f * w.x,
                    1.092548f * w.x * w.y,
                    1.092548f * w.y * w.z,
                    0.315392f * (3.0f * w.z * w.z - 1.0f),
                    1.092548f * w.x * w.z,
                    0.546274f * (w.x * w.x - w.y * w.y),
                };
                for (int i = 0; i < 9; ++i) {
                    sh[static_cast<std::size_t>(i)] += radiance * (basis[i] * weight);
                }
                total_weight += weight;
            }
        }
    }
    // Normalize to exactly 4pi of solid angle, then convolve with the
    // clamped cosine and divide by pi: bands 0/1/2 scale by 1, 2/3, 1/4.
    const float norm = 4.0f * kPi / std::max(total_weight, 1e-6f);
    const float band[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    std::array<Vec4, 9> out{};
    for (int i = 0; i < 9; ++i) {
        const Vec3 value = sh[static_cast<std::size_t>(i)] * (norm * band[i]);
        out[static_cast<std::size_t>(i)] = {value.x, value.y, value.z, 0.0f};
    }
    return out;
}

// --- Atmosphere ------------------------------------------------------------------------------

void Renderer::Impl::update_atmosphere_luts() {
    const SkySettings& sky = frame.settings->sky;
    const std::uint64_t hash = hash_floats({sky.haze, sky.air_density, sky.ozone, sky.ground_albedo.x, sky.ground_albedo.y, sky.ground_albedo.z});
    if (hash == atmosphere_lut_hash) {
        return;
    }
    atmosphere_lut_hash = hash;
    bind_scene_buffers();
    {
        const ProgramId program = shaders.compute("sky_transmittance.comp");
        shaders.use(program);
        glBindImageTexture(0, transmittance_lut.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        dispatch_2d(transmittance_lut.width(), transmittance_lut.height());
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    {
        const ProgramId program = shaders.compute("sky_multiscatter.comp");
        shaders.use(program);
        gfx::bind_texture(gpu::binding::kTransmittance, transmittance_lut, gfx::SamplerKind::LinearClamp);
        glBindImageTexture(0, multiscatter_lut.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        dispatch_2d(multiscatter_lut.width(), multiscatter_lut.height());
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    sky_dirty = true;
}

void Renderer::Impl::capture_sky(GLuint cubemap, int size) {
    const ProgramId program = shaders.compute("sky_capture.comp");
    shaders.use(program);
    gfx::bind_texture(gpu::binding::kTransmittance, transmittance_lut, gfx::SamplerKind::LinearClamp);
    gfx::bind_texture(gpu::binding::kSkyView, sky_view_lut, gfx::SamplerKind::LinearClamp);
    glBindImageTexture(0, cubemap, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(gfx::groups(size, 8), gfx::groups(size, 8), 6);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glGenerateTextureMipmap(cubemap);
}

void Renderer::Impl::update_environment() {
    FrameState& f = frame;
    const SkySettings& sky = f.settings->sky;
    // The frame UBO isn't uploaded yet: the atmosphere shaders need the
    // sun/atmosphere fields, so push a provisional copy.
    upload_frame_uniforms();
    update_atmosphere_luts();

    // Sky-view LUT every frame (cheap), used by the background and capture.
    {
        const ProgramId program = shaders.compute("sky_view.comp");
        shaders.use(program);
        gfx::bind_texture(gpu::binding::kTransmittance, transmittance_lut, gfx::SamplerKind::LinearClamp);
        gfx::bind_texture(0, multiscatter_lut, gfx::SamplerKind::LinearClamp);
        glBindImageTexture(0, sky_view_lut.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        dispatch_2d(sky_view_lut.width(), sky_view_lut.height());
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    SkyMode mode = sky.mode;
    if (mode == SkyMode::Hdri && active_environment < 0) {
        mode = SkyMode::Atmosphere; // nothing loaded yet
    }
    std::uint64_t hash = 0;
    switch (mode) {
    case SkyMode::Atmosphere: {
        // Quantize the sun so tiny movements don't refilter every frame.
        const Vec3 d = f.sun_direction;
        hash = hash_floats({1.0f, std::round(d.x * 400.0f), std::round(d.y * 400.0f), std::round(d.z * 400.0f), f.sun_illuminance.x, f.sun_illuminance.y,
                   f.sun_illuminance.z, static_cast<float>(atmosphere_lut_hash & 0xFFFF)});
        break;
    }
    case SkyMode::Hdri:
        hash = hash_floats({2.0f, static_cast<float>(active_environment), sky.hdri_rotation, static_cast<float>(reinterpret_cast<std::uintptr_t>(environments[static_cast<std::size_t>(active_environment)].get()) & 0xFFFFFF)});
        break;
    case SkyMode::Color:
        hash = hash_floats({3.0f, sky.color.x, sky.color.y, sky.color.z});
        break;
    }
    if (hash == env_layer_hash) {
        return;
    }
    // Limit atmosphere refilters to every few frames while the sun moves.
    if (mode == SkyMode::Atmosphere && env_layer_hash != 0 && frame_index - last_sky_capture_frame < 4) {
        return;
    }
    env_layer_hash = hash;
    last_sky_capture_frame = frame_index;
    switch (mode) {
    case SkyMode::Atmosphere:
        capture_sky(sky_capture.id(), sky_capture.width());
        filter_cubemap_into(sky_capture.id(), sky_capture.width(), 0);
        sky_sh = project_sh(sky_capture.id(), sky_capture.width(), 0.0f);
        break;
    case SkyMode::Hdri: {
        EnvironmentEntry& entry = *environments[static_cast<std::size_t>(active_environment)];
        entry.last_used = frame_index;
        filter_cubemap_into(entry.background.id(), entry.background.width(), 0, sky.hdri_rotation);
        sky_sh = project_sh(entry.background.id(), entry.background.width(), sky.hdri_rotation);
        break;
    }
    case SkyMode::Color: {
        const ProgramId program = shaders.compute("fill_cube.comp");
        shaders.use(program);
        glUniform1i(shaders.uniform(program, "u_layer"), 0);
        glUniform3f(shaders.uniform(program, "u_color"), sky.color.x, sky.color.y, sky.color.z);
        for (int level = 0; level < probe_array.desc().mips; ++level) {
            const int size = std::max(1, kEnvironmentSize >> level);
            glBindImageTexture(0, probe_array.id(), level, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(gfx::groups(size, 8), gfx::groups(size, 8), 6);
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        sky_sh = {};
        sky_sh[0] = {sky.color.x / 0.282095f, sky.color.y / 0.282095f, sky.color.z / 0.282095f, 0.0f};
        break;
    }
    }
}

// --- HDRI ------------------------------------------------------------------------------------

std::unique_ptr<EnvironmentEntry> Renderer::Impl::bake_hdri(const HdrImage& hdr, const std::string& path) {
    if (hdr.width <= 0 || hdr.height <= 0 || hdr.pixels.empty()) {
        throw std::runtime_error("empty HDR image");
    }
    gfx::Texture equirect(gfx::tex2d(GL_RGB32F, hdr.width, hdr.height), "HDRI.Equirect");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTextureSubImage2D(equirect.id(), 0, 0, 0, hdr.width, hdr.height, GL_RGB, GL_FLOAT, hdr.pixels.data());
    int face = 256;
    while (face < 1024 && face * 4 < hdr.width) {
        face *= 2;
    }
    auto entry = std::make_unique<EnvironmentEntry>();
    entry->path = path;
    entry->background = gfx::Texture(gfx::cubemap(GL_R11F_G11F_B10F, face, gfx::mip_count(face, face)), "HDRI.Background");
    const ProgramId program = shaders.compute("equirect_to_cube.comp");
    shaders.use(program);
    gfx::bind_texture(0, equirect, gfx::SamplerKind::LinearClamp);
    glUniform1f(shaders.uniform(program, "u_rotation"), 0.0f);
    glBindImageTexture(0, entry->background.id(), 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R11F_G11F_B10F);
    glDispatchCompute(gfx::groups(face, 8), gfx::groups(face, 8), 6);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glGenerateTextureMipmap(entry->background.id());
    entry->last_used = frame_index;
    return entry;
}

std::size_t Renderer::Impl::ensure_environment_cached(const std::string& path) {
    for (std::size_t i = 0; i < environments.size(); ++i) {
        if (environments[i]->path == path) {
            return i;
        }
    }
    const HdrImage hdr = load_radiance_hdr(path); // throws
    auto entry = bake_hdri(hdr, path);
    // Bounded cache: evict the least recently used inactive entry.
    constexpr std::size_t kMaxEnvironments = 8;
    if (environments.size() >= kMaxEnvironments) {
        std::size_t victim = environments.size();
        for (std::size_t i = 0; i < environments.size(); ++i) {
            if (static_cast<int>(i) == active_environment) {
                continue;
            }
            if (victim == environments.size() || environments[i]->last_used < environments[victim]->last_used) {
                victim = i;
            }
        }
        if (victim < environments.size()) {
            environments.erase(environments.begin() + static_cast<std::ptrdiff_t>(victim));
            if (active_environment > static_cast<int>(victim)) {
                --active_environment;
            }
        }
    }
    environments.push_back(std::move(entry));
    log_info("Environment baked: " + path);
    return environments.size() - 1;
}

// --- Terrain layer textures ------------------------------------------------------------------

void Renderer::Impl::ensure_terrain_textures(const TerrainComponent* terrain) {
    TerrainComponent defaults;
    const TerrainComponent& t = terrain != nullptr ? *terrain : defaults;
    const std::uint64_t hash = hash_floats({t.grass_color.x, t.grass_color.y, t.grass_color.z, t.rock_color.x, t.rock_color.y, t.rock_color.z, t.high_color.x,
        t.high_color.y, t.high_color.z, t.low_color.x, t.low_color.y, t.low_color.z});
    if (hash == terrain_texture_hash && terrain_layers) {
        return;
    }
    terrain_texture_hash = hash;
    constexpr int kSize = 256;
    const int mips = gfx::mip_count(kSize, kSize);
    terrain_layers = gfx::Texture(gfx::tex2d_array(GL_SRGB8_ALPHA8, kSize, kSize, 4, mips), "TerrainLayers");
    terrain_normals = gfx::Texture(gfx::tex2d_array(GL_RGBA8, kSize, kSize, 4, mips), "TerrainNormals");
    const Vec3 colors[4] = {t.grass_color, t.rock_color, t.high_color, t.low_color};
    const float roughness[4] = {0.92f, 0.78f, 0.85f, 0.95f};
    const float frequencies[4] = {24.0f, 9.0f, 14.0f, 30.0f};
    const float bump[4] = {0.6f, 2.2f, 1.2f, 0.5f};
    // Tileable value noise (fBm) per layer.
    auto lattice = [](int x, int y, int period, std::uint32_t seed) {
        x = ((x % period) + period) % period;
        y = ((y % period) + period) % period;
        std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    auto noise2 = [&](float x, float y, int period, std::uint32_t seed) {
        const int xi = static_cast<int>(std::floor(x));
        const int yi = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(xi);
        const float fy = y - static_cast<float>(yi);
        const float u = fx * fx * (3.0f - 2.0f * fx);
        const float v = fy * fy * (3.0f - 2.0f * fy);
        const float a = lattice(xi, yi, period, seed);
        const float b = lattice(xi + 1, yi, period, seed);
        const float c = lattice(xi, yi + 1, period, seed);
        const float d = lattice(xi + 1, yi + 1, period, seed);
        return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
    };
    std::vector<float> height(kSize * kSize);
    std::vector<std::uint8_t> albedo(kSize * kSize * 4);
    std::vector<std::uint8_t> normals(kSize * kSize * 4);
    for (int layer = 0; layer < 4; ++layer) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                float value = 0.0f;
                float amplitude = 0.5f;
                int period = 8;
                for (int octave = 0; octave < 5; ++octave) {
                    const float scale = static_cast<float>(period) / static_cast<float>(kSize);
                    value += amplitude * noise2(static_cast<float>(x) * scale, static_cast<float>(y) * scale, period, static_cast<std::uint32_t>(layer * 17 + octave));
                    amplitude *= 0.5f;
                    period *= 2;
                }
                (void)frequencies;
                height[static_cast<std::size_t>(y * kSize + x)] = value;
            }
        }
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float h = height[static_cast<std::size_t>(y * kSize + x)];
                const float hx = height[static_cast<std::size_t>(y * kSize + (x + 1) % kSize)] - height[static_cast<std::size_t>(y * kSize + (x + kSize - 1) % kSize)];
                const float hy = height[static_cast<std::size_t>(((y + 1) % kSize) * kSize + x)] - height[static_cast<std::size_t>(((y + kSize - 1) % kSize) * kSize + x)];
                const Vec3 n = normalize(Vec3{-hx * bump[layer] * 4.0f, -hy * bump[layer] * 4.0f, 1.0f});
                const float variation = 0.65f + 0.7f * h;
                const Vec3 c = colors[layer] * variation;
                auto srgb = [](float v) {
                    v = std::clamp(v, 0.0f, 1.0f);
                    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
                };
                const std::size_t i = static_cast<std::size_t>(y * kSize + x) * 4;
                albedo[i + 0] = srgb(c.x);
                albedo[i + 1] = srgb(c.y);
                albedo[i + 2] = srgb(c.z);
                albedo[i + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(roughness[layer] - h * 0.1f, 0.0f, 1.0f) * 255.0f));
                normals[i + 0] = static_cast<std::uint8_t>(std::lround((n.x * 0.5f + 0.5f) * 255.0f));
                normals[i + 1] = static_cast<std::uint8_t>(std::lround((n.y * 0.5f + 0.5f) * 255.0f));
                normals[i + 2] = static_cast<std::uint8_t>(std::lround((n.z * 0.5f + 0.5f) * 255.0f));
                normals[i + 3] = 255;
            }
        }
        glTextureSubImage3D(terrain_layers.id(), 0, 0, 0, layer, kSize, kSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, albedo.data());
        glTextureSubImage3D(terrain_normals.id(), 0, 0, 0, layer, kSize, kSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, normals.data());
    }
    glGenerateTextureMipmap(terrain_layers.id());
    glGenerateTextureMipmap(terrain_normals.id());
}

// --- Public environment API -----------------------------------------------------------------

void Renderer::load_environment(const std::string& hdr_path) {
    const std::size_t index = impl_->ensure_environment_cached(hdr_path);
    impl_->active_environment = static_cast<int>(index);
    impl_->environment_path = hdr_path;
    impl_->env_layer_hash = 0;
}

void Renderer::preload_environment(const std::string& hdr_path) {
    impl_->ensure_environment_cached(hdr_path);
}

bool Renderer::activate_environment(const std::string& hdr_path) {
    for (std::size_t i = 0; i < impl_->environments.size(); ++i) {
        if (impl_->environments[i]->path == hdr_path) {
            impl_->active_environment = static_cast<int>(i);
            impl_->environment_path = hdr_path;
            impl_->env_layer_hash = 0;
            return true;
        }
    }
    return false;
}

bool Renderer::is_environment_cached(const std::string& hdr_path) const {
    return std::any_of(impl_->environments.begin(), impl_->environments.end(), [&](const auto& e) { return e->path == hdr_path; });
}

void Renderer::clear_environment() {
    impl_->active_environment = -1;
    impl_->environment_path.clear();
    impl_->env_layer_hash = 0;
}

bool Renderer::has_environment() const {
    return impl_->active_environment >= 0;
}

const std::string& Renderer::environment_path() const {
    return impl_->environment_path;
}

std::size_t Renderer::cached_environment_count() const {
    return impl_->environments.size();
}

} // namespace Hollow

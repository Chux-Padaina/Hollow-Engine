// Renderer core: initialization, per-frame setup (camera, jitter, frame
// uniforms), the render graph that ties every stage together, and the
// public API. The stages themselves live in the other Renderer*.cpp files
// (see RendererImpl.hpp).
#include "Render/RendererImpl.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace Hollow {

namespace {

float halton(int index, int base) {
    float f = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

Vec4 v4(Vec3 v, float w) {
    return {v.x, v.y, v.z, w};
}

} // namespace

// --- Initialization ---------------------------------------------------------------

bool Renderer::Impl::initialize() {
    log_info(std::format("OpenGL {} | {} | GLSL {}", gfx::gl_string(GL_VERSION), gfx::gl_string(GL_RENDERER), gfx::gl_string(GL_SHADING_LANGUAGE_VERSION)));
    if (!GLAD_GL_VERSION_4_5) {
        error = "OpenGL 4.5 is required (the driver reports " + gfx::gl_string(GL_VERSION) + "). Update the GPU driver.";
        log_error(error);
        return false;
    }
#ifndef NDEBUG
    gfx::install_debug_output();
#else
    if (gfx::has_extension("GL_KHR_debug")) {
        gfx::install_debug_output();
    }
#endif
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_anisotropy);
    glGetError();
    max_anisotropy = std::clamp(max_anisotropy, 1.0f, 16.0f);
    has_s3tc = gfx::has_extension("GL_EXT_texture_compression_s3tc");
    gfx::create_samplers(max_anisotropy);

    if (!shaders.initialize()) {
        error = "Shader folder not found (expected engine/shaders or <exe>/shaders).";
        log_error(error);
        return false;
    }

    graph = std::make_unique<rg::RenderGraph>(framebuffers, &profiler);
    frame_ubo = gfx::Buffer(sizeof(gpu::FrameUniforms), GL_DYNAMIC_STORAGE_BIT, nullptr, "FrameUBO");
    shadow_ubo = gfx::Buffer(sizeof(gpu::ShadowUniforms), GL_DYNAMIC_STORAGE_BIT, nullptr, "ShadowUBO");
    pass_ubo = gfx::Buffer(256 * 1024, GL_DYNAMIC_STORAGE_BIT, nullptr, "PassUBO");

    try {
        init_mesh_pool();
        textures.initialize();
        textures.set_compression(has_s3tc);
        init_environment();
        init_overlay();
        // Compile the core programs now so a broken shader is reported at
        // startup rather than on the first frame.
        mesh_program("PASS_PREPASS", false);
        mesh_program("PASS_FORWARD", false);
        mesh_program("PASS_SHADOW", false);
        shaders.compute("cull.comp");
        shaders.compute("clusters.comp");
    } catch (const std::exception& e) {
        error = e.what();
        log_error(error);
        return false;
    }

    ready = true;
    return true;
}

void Renderer::Impl::shutdown() {
    if (graph) {
        graph->clear_pool();
    }
    destroy_overlay();
    destroy_environment();
    textures.shutdown();
    destroy_mesh_pool();
    if (capture_fbo != 0) {
        glDeleteFramebuffers(1, &capture_fbo);
        capture_fbo = 0;
    }
    capture_ubo.reset();
    capture_commands.reset();
    cull_stats.reset();
    for (gfx::Buffer& b : cull_readback) {
        b.reset();
    }
    volume_capture.reset();
    volume_capture_depth.reset();
    particle_systems.clear();
    grass_visible.reset();
    grass_args.reset();
    if (empty_vao != 0) {
        glDeleteVertexArrays(1, &empty_vao);
        empty_vao = 0;
    }
    terrains.clear();
    shaders.shutdown();
    framebuffers.clear();
    gfx::destroy_shared_objects();
    ready = false;
}

// --- Frame setup -------------------------------------------------------------------

void Renderer::Impl::setup_frame(const Scene& scene, const Camera& camera, const RenderPipelineSettings& settings, const RenderView& view, float delta) {
    FrameState& f = frame;
    const FrameState previous = f;
    f = FrameState{};
    f.scene = &scene;
    f.camera = &camera;
    f.settings = &settings;
    f.view = &view;
    f.delta = delta;
    f.output_width = output_width;
    f.output_height = output_height;
    f.taa = settings.aa.method == AntiAliasing::Taa;
    f.render_scale = std::clamp(settings.aa.render_scale, 0.5f, 1.0f);
    f.render_width = std::max(1, static_cast<int>(std::lround(static_cast<float>(output_width) * f.render_scale)));
    f.render_height = std::max(1, static_cast<int>(std::lround(static_cast<float>(output_height) * f.render_scale)));
    f.gpu_driven = settings.performance.gpu_culling;
    f.occlusion = f.gpu_driven && settings.performance.occlusion_culling;
    f.debug_view = static_cast<int>(view.debug_view);
    fog_wind_offset += settings.fog.wind * delta;
    f.editor = view.editor;

    // Camera
    f.near_plane = camera.near_plane;
    f.fov_y = camera.fov_y_radians;
    f.view_matrix = camera.view_matrix();
    f.camera_position = camera.position();
    f.camera_forward = camera.forward();
    const float aspect = static_cast<float>(f.render_width) / static_cast<float>(f.render_height);
    f.unjittered_projection = Mat4::perspective_reversed_z(camera.fov_y_radians, aspect, camera.near_plane);
    f.projection = f.unjittered_projection;
    if (f.taa) {
        const int index = static_cast<int>(frame_index % 8) + 1;
        f.jitter = {(halton(index, 2) - 0.5f) * 2.0f / static_cast<float>(f.render_width), (halton(index, 3) - 0.5f) * 2.0f / static_cast<float>(f.render_height)};
        f.prev_jitter = previous.jitter;
        f.projection.m[8] += f.jitter.x;
        f.projection.m[9] += f.jitter.y;
    }
    f.view_projection = f.projection * f.view_matrix;
    f.unjittered_view_projection = f.unjittered_projection * f.view_matrix;
    // Conventional projection with a far plane for frustum planes/LODs.
    f.cull_view_projection = Mat4::perspective(camera.fov_y_radians, aspect, camera.near_plane, std::max(camera.far_plane, 2000.0f)) * f.view_matrix;
    f.frustum = Frustum::from_matrix(f.cull_view_projection);
}

void Renderer::Impl::upload_frame_uniforms() {
    const FrameState& f = frame;
    const RenderPipelineSettings& s = *f.settings;
    gpu::FrameUniforms& u = frame_uniforms;
    const Mat4 previous_vp = has_previous ? previous_view_projection : f.unjittered_view_projection;
    const Vec4 previous_camera_position = has_previous ? previous_camera : v4(f.camera_position, f.near_plane);
    const float exposure_before = has_previous ? previous_exposure : f.exposure;
    u = {};
    u.view = f.view_matrix;
    u.projection = f.projection;
    u.view_projection = f.view_projection;
    u.inv_view = inverse(f.view_matrix);
    u.inv_projection = inverse(f.projection);
    u.inv_view_projection = inverse(f.view_projection);
    u.unjittered_view_projection = f.unjittered_view_projection;
    u.prev_view_projection = previous_vp;
    u.prev_inv_view_projection = inverse(previous_vp);
    u.camera_position = v4(f.camera_position, f.near_plane);
    u.camera_forward = v4(f.camera_forward, static_cast<float>(time));
    u.prev_camera_position = previous_camera_position;
    u.render_size = {static_cast<float>(f.render_width), static_cast<float>(f.render_height), 1.0f / static_cast<float>(f.render_width), 1.0f / static_cast<float>(f.render_height)};
    u.output_size = {static_cast<float>(f.output_width), static_cast<float>(f.output_height), 1.0f / static_cast<float>(f.output_width), 1.0f / static_cast<float>(f.output_height)};
    u.jitter = {f.jitter.x, f.jitter.y, f.prev_jitter.x, f.prev_jitter.y};
    u.exposure = {f.exposure, f.ev100, exposure_before, 1.0f / std::max(f.exposure, 1e-12f)};
    u.flags[0] = f.debug_view;
    u.flags[1] = f.feature_bits;
    u.flags[2] = f.light_count;
    u.flags[3] = static_cast<int>(frame_index & 0x7FFFFFFF);
    u.time = {f.delta, static_cast<float>(time), std::sin(static_cast<float>(time)), f.render_scale};

    // Clusters: square tiles, 16 across the wider axis.
    const float tile = std::ceil(static_cast<float>(std::max(f.render_width, f.render_height)) / 16.0f);
    const float tiles_x = std::ceil(static_cast<float>(f.render_width) / tile);
    const float tiles_y = std::ceil(static_cast<float>(f.render_height) / tile);
    const float cluster_near = std::max(f.near_plane, 0.05f);
    const float cluster_far = 400.0f;
    u.cluster = {tiles_x, tiles_y, static_cast<float>(kClusterSlices), tile};
    const float log_ratio = std::log2(cluster_far / cluster_near);
    u.cluster_z = {static_cast<float>(kClusterSlices) / log_ratio, -static_cast<float>(kClusterSlices) * std::log2(cluster_near) / log_ratio, cluster_near, cluster_far};

    u.sun_direction = v4(f.sun_direction, static_cast<float>(f.sun_light_index));
    u.sun_illuminance = v4(f.sun_illuminance, f.sun_angular_radius);
    float env_scale = 1.0f;
    switch (s.sky.mode) {
    case SkyMode::Hdri: env_scale = s.sky.hdri_intensity; break;
    case SkyMode::Color: env_scale = s.sky.color_intensity; break;
    case SkyMode::Atmosphere: env_scale = 1.0f; break;
    }
    if (!s.gi.ibl) {
        env_scale = 0.0f;
    }
    u.ambient = {env_scale, static_cast<float>(gfx::mip_count(kEnvironmentSize, kEnvironmentSize)), static_cast<float>(static_cast<int>(s.sky.mode)), s.sky.hdri_rotation};
    u.sky_color = v4(s.sky.color, s.sky.color_intensity);
    u.counts = {static_cast<float>(probes.size()), static_cast<float>(volumes.size()), static_cast<float>(decals.size()), static_cast<float>(f.directional_count)};
    const FogSettings& fog = s.fog;
    u.fog0 = {fog.density, fog.height_falloff, fog.base_height, fog.anisotropy};
    u.fog1 = v4(fog.albedo, fog.max_distance);
    u.fog2 = {fog.ambient, fog.noise, 2.0f, 0.0f};
    u.fog_wind = v4(fog_wind_offset, 0.0f);
    u.gi = {s.gi.ao_intensity, s.gi.specular_occlusion, s.gi.ssr_max_roughness, s.gi.ao_radius};
    u.atmosphere0 = {6360.0f, 6460.0f, s.sky.haze, s.sky.air_density};
    u.atmosphere1 = {s.sky.ozone, s.sky.sun_disk ? 1.0f : 0.0f, s.sky.background_intensity, s.sky.hdri_intensity};
    u.ground_albedo = v4(s.sky.ground_albedo, 0.0f);
    u.selection = {static_cast<float>(f.view->selected), 0.0f, 2.0f, 0.0f};
    for (std::size_t i = 0; i < 9; ++i) {
        u.sky_sh[i] = sky_sh[i];
    }
    frame_ubo.upload(&u, sizeof(u));
    glBindBufferBase(GL_UNIFORM_BUFFER, gpu::binding::kFrame, frame_ubo.id());
    shadow_ubo.upload(&shadow_uniforms, sizeof(shadow_uniforms));
    glBindBufferBase(GL_UNIFORM_BUFFER, gpu::binding::kShadow, shadow_ubo.id());
}

void Renderer::Impl::bind_pass_uniforms(const void* data, std::size_t size) {
    GLint alignment = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    const std::size_t aligned = (size + static_cast<std::size_t>(alignment) - 1) / static_cast<std::size_t>(alignment) * static_cast<std::size_t>(alignment);
    if (pass_ubo_offset + aligned > pass_ubo.size()) {
        pass_ubo_offset = 0;
    }
    pass_ubo.upload(data, size, pass_ubo_offset);
    glBindBufferRange(GL_UNIFORM_BUFFER, gpu::binding::kPass, pass_ubo.id(), static_cast<GLintptr>(pass_ubo_offset), static_cast<GLsizeiptr>(size));
    pass_ubo_offset += aligned;
}

void Renderer::Impl::bind_scene_buffers() {
    using namespace gpu::binding;
    auto bind = [](unsigned binding, const gfx::Buffer& buffer) {
        if (buffer) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer.id());
        }
    };
    bind(kInstances, instance_buffer);
    bind(kMaterials, material_buffer);
    bind(kMeshes, pool.infos);
    bind(kLights, light_buffer);
    bind(kShadowViews, shadow_view_buffer);
    bind(kProbes, probe_buffer);
    bind(kVolumes, volume_buffer);
    bind(kDecals, decal_buffer);
    bind(kClusterGrid, cluster_grid);
    bind(kClusterItems, cluster_items);
    bind(kVertices, pool.vertices);
    bind(kScratch1, instance_lods);
    glBindBufferBase(GL_UNIFORM_BUFFER, kFrame, frame_ubo.id());
    glBindBufferBase(GL_UNIFORM_BUFFER, kShadow, shadow_ubo.id());
}

void Renderer::Impl::bind_global_textures(GLuint scene_color, GLuint scene_depth, GLuint ao, GLuint ssr, GLuint contact, GLuint fog) {
    using namespace gpu::binding;
    using gfx::SamplerKind;
    const GLuint white = textures.white();
    gfx::bind_texture(kBrdfLut, dfg_lut, SamplerKind::LinearClamp);
    gfx::bind_texture(kProbesTex, probe_array, SamplerKind::LinearMipClamp);
    gfx::bind_texture(kVolumeSh0, volume_sh[0], SamplerKind::LinearClamp);
    gfx::bind_texture(kVolumeSh1, volume_sh[1], SamplerKind::LinearClamp);
    gfx::bind_texture(kVolumeSh2, volume_sh[2], SamplerKind::LinearClamp);
    gfx::bind_texture(kCascades, cascades, SamplerKind::ShadowCompare);
    gfx::bind_texture(kCascadesRaw, cascades, SamplerKind::NearestClamp);
    gfx::bind_texture(kAtlas, atlas, SamplerKind::ShadowCompare);
    gfx::bind_texture(kAtlasRaw, atlas, SamplerKind::NearestClamp);
    gfx::bind_texture(kAo, ao != 0 ? ao : white, SamplerKind::LinearClamp);
    gfx::bind_texture(kSsr, ssr != 0 ? ssr : textures.black(), SamplerKind::LinearClamp);
    gfx::bind_texture(kContact, contact != 0 ? contact : white, SamplerKind::LinearClamp);
    gfx::bind_texture(kFog, fog != 0 ? fog : fog_history[0].id(), SamplerKind::LinearClamp);
    gfx::bind_texture(kDecalsTex, decal_array, SamplerKind::LinearMipClamp);
    gfx::bind_texture(kTransmittance, transmittance_lut, SamplerKind::LinearClamp);
    gfx::bind_texture(kSceneColor, scene_color != 0 ? scene_color : textures.black(), SamplerKind::LinearMipClamp);
    gfx::bind_texture(kSceneDepth, scene_depth != 0 ? scene_depth : textures.black(), SamplerKind::NearestClamp);
    gfx::bind_texture(kTerrainLayers, terrain_layers, SamplerKind::LinearMipRepeatAniso);
    gfx::bind_texture(kTerrainNormals, terrain_normals, SamplerKind::LinearMipRepeatAniso);
    gfx::bind_texture(kNoise, noise, SamplerKind::LinearRepeat);
}

// --- Frame ------------------------------------------------------------------------

void Renderer::Impl::render(const Scene& scene, const Camera& camera, const RenderPipelineSettings& settings, const RenderView& view, float delta) {
    ++frame_index;
    time += static_cast<double>(delta);
    stats = {};
    profiler.begin_frame();
    shaders.hot_reload();
    textures.set_async(settings.performance.async_textures);
    if (has_s3tc) {
        textures.set_compression(settings.performance.texture_compression);
    }
    textures.update(24u << 20);
    pass_ubo_offset = 0;

    // Global state for the whole frame: reversed-Z with [0,1] depth.
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glClearDepth(0.0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glMemoryBarrier(GL_ALL_BARRIER_BITS); // last frame's image/SSBO writes

    output.ensure(gfx::tex2d(GL_RGBA8, output_width, output_height), "Output");

    try {
        setup_frame(scene, camera, settings, view, delta);
        update_exposure();
        extract_scene();
        setup_shadows();
        compute_features();
        update_environment();
        upload_scene_buffers();
        upload_frame_uniforms();
        bind_scene_buffers();
        prepare_skinning();

        graph->reset();
        build_graph();
        graph->compile();
        graph->execute();
        last_pass_info = graph->pass_info();
        if (frame.gpu_driven) {
            stats.visible_instances = last_visible_instances; // GPU count, two frames old
        }
        last_resource_info = graph->resource_info();

        process_bakes();
    } catch (const std::exception& e) {
        static std::string last;
        if (last != e.what()) {
            last = e.what();
            log_error(std::string("Render error: ") + e.what());
        }
    }

    // Hand the GL state back to the UI in its expected defaults.
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, output_width, output_height);
    glUseProgram(0);
    glBindVertexArray(0);
    for (GLuint unit = 0; unit < 32; ++unit) {
        glBindSampler(unit, 0);
    }

    // Bookkeeping for the next frame.
    previous_view_projection = frame.unjittered_view_projection;
    previous_camera = v4(frame.camera_position, frame.near_plane);
    previous_exposure = frame.exposure;
    has_previous = true;
    visibility_index ^= 1;
    history_index ^= 1;
    fog_index ^= 1;
    stats.gpu_ms = profiler.frame_milliseconds();
    stats.gpu_meshes = pooled_meshes.size();
    stats.mesh_pool_bytes = pool.vertices.size() + pool.indices.size();
    stats.textures = textures.texture_count();
    stats.texture_bytes = textures.gpu_bytes();
    stats.pending_textures = textures.pending();
    stats.transient_textures = graph->pooled_texture_count();
    stats.transient_bytes = graph->pooled_texture_bytes();
    stats.render_width = frame.render_width;
    stats.render_height = frame.render_height;
    stats.gpu_driven = frame.gpu_driven;
    stats.exposure_ev100 = frame.ev100;
    if (frame_index % 60 == 0) {
        graph->trim(120);
        evict_unused_meshes();
    }
}

// --- Public API -------------------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() = default;

bool Renderer::initialize() {
    return impl_->initialize();
}

bool Renderer::ready() const {
    return impl_->ready;
}

const std::string& Renderer::error() const {
    return impl_->error;
}

void Renderer::shutdown() {
    impl_->shutdown();
}

void Renderer::resize(int width, int height) {
    impl_->output_width = std::max(1, width);
    impl_->output_height = std::max(1, height);
}

int Renderer::width() const {
    return impl_->output_width;
}

int Renderer::height() const {
    return impl_->output_height;
}

void Renderer::render(const Scene& scene, const Camera& camera, const RenderPipelineSettings& settings, const RenderView& view, float delta_seconds) {
    if (!impl_->ready) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.15f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    impl_->render(scene, camera, settings, view, std::clamp(delta_seconds, 0.0f, 0.25f));
}

unsigned int Renderer::output_texture() const {
    return impl_->output.id();
}

void Renderer::present(int x, int y, int width, int height) const {
    if (!impl_->output) {
        return;
    }
    const GLuint fbo = impl_->framebuffers.get(std::array<gfx::Attachment, 1>{gfx::Attachment{impl_->output.id(), 0, -1}}, {});
    glBlitNamedFramebuffer(fbo, 0, 0, 0, impl_->output.width(), impl_->output.height(), x, y, x + width, y + height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

Vec2 Renderer::project(Vec3 world, const Camera& camera) const {
    const float w_px = static_cast<float>(impl_->output_width);
    const float h_px = static_cast<float>(impl_->output_height);
    const Mat4 vp = camera.projection_matrix(w_px / std::max(h_px, 1.0f)) * camera.view_matrix();
    const Vec4 clip = vp * Vec4{world.x, world.y, world.z, 1.0f};
    if (clip.w < 0.0001f) {
        return {-10000.0f, -10000.0f};
    }
    return {(clip.x / clip.w * 0.5f + 0.5f) * w_px, (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * h_px};
}

const RenderStats& Renderer::stats() const {
    return impl_->stats;
}

std::vector<RenderPassStat> Renderer::pass_stats() const {
    std::vector<RenderPassStat> out;
    for (const rg::PassInfo& pass : impl_->last_pass_info) {
        out.push_back({pass.name, pass.gpu_ms, pass.culled, pass.reads, pass.writes});
    }
    return out;
}

namespace {

std::string format_name(GLenum format) {
    switch (format) {
    case GL_RGBA8: return "RGBA8";
    case GL_SRGB8_ALPHA8: return "SRGB8_A8";
    case GL_RGBA16F: return "RGBA16F";
    case GL_RG16F: return "RG16F";
    case GL_R16F: return "R16F";
    case GL_R32F: return "R32F";
    case GL_R8: return "R8";
    case GL_RG8: return "RG8";
    case GL_RGB10_A2: return "RGB10A2";
    case GL_R11F_G11F_B10F: return "R11G11B10F";
    case GL_DEPTH_COMPONENT32F: return "D32F";
    case GL_RGBA32F: return "RGBA32F";
    case GL_R32UI: return "R32UI";
    default: return std::format("0x{:X}", format);
    }
}

} // namespace

std::vector<RenderResourceStat> Renderer::resource_stats() const {
    std::vector<RenderResourceStat> out;
    for (const rg::ResourceInfo& r : impl_->last_resource_info) {
        RenderResourceStat stat;
        stat.name = r.name;
        stat.imported = r.imported;
        stat.buffer = r.buffer;
        stat.pool_slot = r.physical;
        if (r.buffer) {
            stat.width = static_cast<int>(r.buffer_size);
            stat.format = "buffer";
        } else {
            stat.width = r.desc.width;
            stat.height = r.desc.height;
            stat.depth = r.desc.depth;
            stat.format = format_name(r.desc.format);
        }
        out.push_back(std::move(stat));
    }
    return out;
}

void Renderer::request_bake() {
    impl_->bake_requested = true;
}

float Renderer::bake_progress() const {
    if (impl_->bake_total <= 0) {
        return 1.0f;
    }
    return std::clamp(static_cast<float>(impl_->bake_done) / static_cast<float>(impl_->bake_total), 0.0f, 1.0f);
}

bool Renderer::baking() const {
    return !impl_->bake_queue.empty();
}

void Renderer::reload_shaders() {
    impl_->shaders.hot_reload(true);
}

} // namespace Hollow

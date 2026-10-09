// Post processing: exposure (manual / automatic histogram with read-back
// a few frames late), TAA + temporal upscaling, contrast-adaptive
// sharpening, depth of field, motion blur, bloom, the grading LUT (bakes
// the tonemapper + grading + an optional .cube LUT), the final composite
// and FXAA.
#include "Render/RendererImpl.hpp"

#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Hollow {

namespace {

std::uint64_t hash_mix(std::uint64_t h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ bytes[i]) * 1099511628211ull;
    }
    return h;
}

// Adobe .cube 3D LUT. Returns size (0 on failure).
int load_cube_lut(const std::string& path, std::vector<float>& data) {
    std::ifstream file(path_from_utf8(path));
    if (!file) {
        return 0;
    }
    int size = 0;
    data.clear();
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream in(line);
        std::string head;
        in >> head;
        if (head == "LUT_3D_SIZE") {
            in >> size;
            continue;
        }
        if (head == "TITLE" || head == "DOMAIN_MIN" || head == "DOMAIN_MAX" || head == "LUT_1D_SIZE") {
            continue;
        }
        std::istringstream values(line);
        float r, g, b;
        if (values >> r >> g >> b) {
            data.push_back(r);
            data.push_back(g);
            data.push_back(b);
        }
    }
    if (size <= 1 || data.size() != static_cast<std::size_t>(size) * size * size * 3) {
        data.clear();
        return 0;
    }
    return size;
}

} // namespace

void Renderer::Impl::update_exposure() {
    FrameState& f = frame;
    const ExposureSettings& e = f.settings->exposure;
    if (e.mode == ExposureMode::Automatic && exposure_initialized) {
        // Result written two frames ago (no stall).
        const gfx::Buffer& readback = exposure_readback[static_cast<std::size_t>((exposure_frame + 1) % 3)];
        float result[4] = {};
        glGetNamedBufferSubData(readback.id(), 0, sizeof(result), result);
        const float measured = result[0];
        if (measured > -50.0f) {
            const float target = std::clamp(measured, e.min_ev100, e.max_ev100);
            const float speed = target > auto_ev100 ? e.speed_up : e.speed_down;
            const float t = 1.0f - std::exp(-f.delta * speed);
            if (!std::isfinite(auto_ev100) || std::abs(target - auto_ev100) > 12.0f) {
                auto_ev100 = target;
            } else {
                auto_ev100 += (target - auto_ev100) * t;
            }
        }
    }
    float ev100 = e.mode == ExposureMode::Automatic ? auto_ev100 : e.ev100;
    ev100 -= e.compensation;
    f.ev100 = ev100;
    // Saturation-based exposure: maximum luminance 1.2 * 2^EV100.
    f.exposure = 1.0f / (1.2f * std::pow(2.0f, ev100));
}

void Renderer::Impl::update_grading_lut() {
    const PostSettings& p = frame.settings->post;
    if (p.lut_path != loaded_lut_path) {
        loaded_lut_path = p.lut_path;
        user_lut.clear();
        user_lut_size = 0;
        if (!p.lut_path.empty()) {
            user_lut_size = load_cube_lut(p.lut_path, user_lut);
            if (user_lut_size == 0) {
                log_warning("Couldn't read the LUT " + p.lut_path + " (expected an Adobe .cube 3D LUT)");
            } else {
                user_lut_texture = gfx::Texture(gfx::tex3d(GL_RGBA16F, user_lut_size, user_lut_size, user_lut_size), "UserLUT");
                std::vector<float> rgba(static_cast<std::size_t>(user_lut_size) * user_lut_size * user_lut_size * 4);
                for (std::size_t i = 0; i < rgba.size() / 4; ++i) {
                    rgba[i * 4 + 0] = user_lut[i * 3 + 0];
                    rgba[i * 4 + 1] = user_lut[i * 3 + 1];
                    rgba[i * 4 + 2] = user_lut[i * 3 + 2];
                    rgba[i * 4 + 3] = 1.0f;
                }
                glTextureSubImage3D(user_lut_texture.id(), 0, 0, 0, 0, user_lut_size, user_lut_size, user_lut_size, GL_RGBA, GL_FLOAT, rgba.data());
            }
        }
        grading_hash = 0;
    }
    std::uint64_t h = 1469598103934665603ull;
    const int tonemapper = static_cast<int>(p.tonemapper);
    h = hash_mix(h, &tonemapper, sizeof(int));
    for (float v : {p.temperature, p.tint, p.contrast, p.saturation, p.lift.x, p.lift.y, p.lift.z, p.gamma.x, p.gamma.y, p.gamma.z, p.gain.x, p.gain.y, p.gain.z,
             p.lut_contribution, static_cast<float>(user_lut_size)}) {
        h = hash_mix(h, &v, sizeof(float));
    }
    if (h == grading_hash && grading_lut) {
        return;
    }
    grading_hash = h;
    grading_lut.ensure(gfx::tex3d(GL_RGBA16F, 32, 32, 32), "GradingLUT");
    if (!user_lut_texture) {
        user_lut_texture = gfx::Texture(gfx::tex3d(GL_RGBA16F, 2, 2, 2), "UserLUT");
    }
    const ProgramId program = shaders.compute("lut_bake.comp");
    shaders.use(program);
    glUniform1i(shaders.uniform(program, "u_tonemapper"), tonemapper);
    glUniform1f(shaders.uniform(program, "u_temperature"), p.temperature);
    glUniform1f(shaders.uniform(program, "u_tint"), p.tint);
    glUniform1f(shaders.uniform(program, "u_contrast"), p.contrast);
    glUniform1f(shaders.uniform(program, "u_saturation"), p.saturation);
    glUniform3f(shaders.uniform(program, "u_lift"), p.lift.x, p.lift.y, p.lift.z);
    glUniform3f(shaders.uniform(program, "u_gamma"), p.gamma.x, p.gamma.y, p.gamma.z);
    glUniform3f(shaders.uniform(program, "u_gain"), p.gain.x, p.gain.y, p.gain.z);
    glUniform1f(shaders.uniform(program, "u_user_lut"), user_lut_size > 0 ? p.lut_contribution : 0.0f);
    gfx::bind_texture(0, user_lut_texture, gfx::SamplerKind::LinearClamp);
    glBindImageTexture(0, grading_lut.id(), 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(8, 8, 8);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Renderer::Impl::ensure_lens_dirt() {
    if (lens_dirt) {
        return;
    }
    constexpr int kSize = 256;
    std::vector<float> pixels(kSize * kSize * 4, 0.0f);
    std::uint32_t state = 12345u;
    auto rnd = [&state] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    for (int blob = 0; blob < 90; ++blob) {
        const float cx = rnd() * kSize;
        const float cy = rnd() * kSize;
        const float radius = 3.0f + rnd() * rnd() * 26.0f;
        const float strength = 0.15f + rnd() * 0.6f;
        const float tint = rnd();
        for (int y = static_cast<int>(cy - radius); y <= static_cast<int>(cy + radius); ++y) {
            for (int x = static_cast<int>(cx - radius); x <= static_cast<int>(cx + radius); ++x) {
                if (x < 0 || y < 0 || x >= kSize || y >= kSize) continue;
                const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
                if (d > 1.0f) continue;
                const float v = strength * (1.0f - d * d) * (0.6f + 0.4f * std::sin(d * 9.0f));
                float* p = &pixels[(static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4];
                p[0] += v * (0.9f + 0.1f * tint);
                p[1] += v * 0.95f;
                p[2] += v * (1.0f - 0.15f * tint);
                p[3] = 1.0f;
            }
        }
    }
    lens_dirt = gfx::Texture(gfx::tex2d(GL_RGBA16F, kSize, kSize), "LensDirt");
    glTextureSubImage2D(lens_dirt.id(), 0, 0, 0, kSize, kSize, GL_RGBA, GL_FLOAT, pixels.data());
}

rg::TextureHandle Renderer::Impl::add_post_passes(rg::TextureHandle color, rg::TextureHandle depth, rg::TextureHandle velocity, rg::TextureHandle output_handle) {
    FrameState& f = frame;
    const RenderPipelineSettings& s = *f.settings;
    const int ow = f.output_width;
    const int oh = f.output_height;
    update_grading_lut();
    ensure_lens_dirt();

    // --- Auto exposure histogram ------------------------------------------------
    if (s.exposure.mode == ExposureMode::Automatic && f.debug_view == 0) {
        const rg::BufferHandle histogram = graph->import("ExposureHistogram", exposure_buffer);
        graph->add_pass(
            "Exposure histogram",
            [&](rg::PassBuilder& b) {
                b.read(color);
                b.write(histogram, rg::Access::Storage);
                b.side_effect();
            },
            [this, color, histogram](const rg::PassContext& ctx) {
                glClearNamedBufferSubData(ctx.buf(histogram), GL_R32UI, 0, 256 * sizeof(std::uint32_t), GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, ctx.buf(histogram));
                const ProgramId program = shaders.compute("exposure_histogram.comp");
                shaders.use(program);
                gfx::bind_texture(0, ctx.tex(color), gfx::SamplerKind::NearestClamp);
                const gfx::Texture& c = ctx.texture(color);
                glDispatchCompute(gfx::groups((c.width() + 1) / 2, 16), gfx::groups((c.height() + 1) / 2, 16), 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                shaders.use(shaders.compute("exposure_average.comp"));
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                exposure_frame = (exposure_frame + 1) % 3;
                glCopyNamedBufferSubData(ctx.buf(histogram), exposure_readback[static_cast<std::size_t>(exposure_frame)].id(), 256 * sizeof(std::uint32_t), 0,
                    4 * sizeof(float));
                if (!exposure_initialized) {
                    // Seed every slot so the first reads are valid.
                    for (const gfx::Buffer& b : exposure_readback) {
                        glCopyNamedBufferSubData(ctx.buf(histogram), b.id(), 256 * sizeof(std::uint32_t), 0, 4 * sizeof(float));
                    }
                    exposure_initialized = true;
                }
            });
    }

    // --- TAA / upscale ------------------------------------------------------------------
    rg::TextureHandle current = color;
    if (f.taa) {
        const gfx::TextureDesc history_desc = gfx::tex2d(GL_RGBA16F, ow, oh);
        for (gfx::Texture& t : taa_history) {
            if (t.ensure(history_desc, "TAAHistory")) {
                history_valid = false;
            }
        }
        const rg::TextureHandle history_out = graph->import("TAAHistory", taa_history[static_cast<std::size_t>(history_index)]);
        const rg::TextureHandle history_in = graph->import("TAAHistoryPrevious", taa_history[static_cast<std::size_t>(history_index ^ 1)]);
        const bool reset = !history_valid;
        history_valid = true;
        graph->add_pass(
            "TAA + upscale",
            [&](rg::PassBuilder& b) {
                b.read(color);
                b.read(depth);
                b.read(velocity);
                b.read(history_in);
                b.write(history_out, rg::Access::Image);
            },
            [this, color, depth, velocity, history_in, history_out, reset](const rg::PassContext& ctx) {
                const ProgramId program = shaders.compute("taa.comp");
                shaders.use(program);
                glUniform1f(shaders.uniform(program, "u_history_weight"), frame.settings->aa.taa_history);
                glUniform1i(shaders.uniform(program, "u_reset"), reset ? 1 : 0);
                gfx::bind_texture(0, ctx.tex(color), gfx::SamplerKind::NearestClamp);
                gfx::bind_texture(1, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
                gfx::bind_texture(2, ctx.tex(velocity), gfx::SamplerKind::NearestClamp);
                gfx::bind_texture(3, ctx.tex(history_in), gfx::SamplerKind::LinearClamp);
                const gfx::Texture& out = ctx.texture(history_out);
                glBindImageTexture(0, out.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                glDispatchCompute(gfx::groups(out.width(), 8), gfx::groups(out.height(), 8), 1);
            });
        current = history_out;
        if (s.aa.sharpness > 0.001f) {
            struct Data {
                rg::TextureHandle sharpened;
            };
            const Data& sharp = graph->add_pass<Data>(
                "Sharpen (CAS)",
                [&](rg::PassBuilder& b, Data& d) {
                    d.sharpened = b.create("Sharpened", gfx::tex2d(GL_RGBA16F, ow, oh));
                    b.write(d.sharpened, rg::Access::Image);
                    b.read(current);
                },
                [this, current](const Data& d, const rg::PassContext& ctx) {
                    const ProgramId program = shaders.compute("sharpen.comp");
                    shaders.use(program);
                    glUniform1f(shaders.uniform(program, "u_sharpness"), frame.settings->aa.sharpness);
                    gfx::bind_texture(0, ctx.tex(current), gfx::SamplerKind::NearestClamp);
                    const gfx::Texture& out = ctx.texture(d.sharpened);
                    glBindImageTexture(0, out.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                    glDispatchCompute(gfx::groups(out.width(), 8), gfx::groups(out.height(), 8), 1);
                });
            current = sharp.sharpened;
        }
    } else {
        history_valid = false;
    }

    // --- Depth of field -------------------------------------------------------------
    if (s.post.depth_of_field && f.debug_view == 0) {
        struct Data {
            rg::TextureHandle out;
        };
        const Data& dof = graph->add_pass<Data>(
            "Depth of field",
            [&](rg::PassBuilder& b, Data& d) {
                d.out = b.create("DepthOfField", gfx::tex2d(GL_RGBA16F, ow, oh));
                b.write(d.out, rg::Access::Image);
                b.read(current);
                b.read(depth);
            },
            [this, current, depth](const Data& d, const rg::PassContext& ctx) {
                const PostSettings& p = frame.settings->post;
                const float sensor_height = 0.024f;
                const float focal = sensor_height / (2.0f * std::tan(frame.fov_y * 0.5f));
                const float aperture = focal / std::max(p.f_stop, 0.5f);
                const gfx::Texture& out = ctx.texture(d.out);
                const ProgramId program = shaders.compute("dof.comp");
                shaders.use(program);
                glUniform1f(shaders.uniform(program, "u_focus"), std::max(p.focus_distance, focal * 2.0f));
                glUniform1f(shaders.uniform(program, "u_aperture"), aperture);
                glUniform1f(shaders.uniform(program, "u_focal"), focal);
                glUniform1f(shaders.uniform(program, "u_sensor"), static_cast<float>(out.height()) / sensor_height);
                glUniform1f(shaders.uniform(program, "u_max_coc"), static_cast<float>(out.height()) * 0.02f);
                glUniform1i(shaders.uniform(program, "u_autofocus"), p.autofocus ? 1 : 0);
                gfx::bind_texture(0, ctx.tex(current), gfx::SamplerKind::LinearClamp);
                gfx::bind_texture(1, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
                glBindImageTexture(0, out.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                glDispatchCompute(gfx::groups(out.width(), 8), gfx::groups(out.height(), 8), 1);
            });
        current = dof.out;
    }

    // --- Motion blur -----------------------------------------------------------------
    if (s.post.motion_blur && f.debug_view == 0) {
        const int tw = std::max(1, (ow + 15) / 16);
        const int th = std::max(1, (oh + 15) / 16);
        const float scale = std::clamp(s.post.shutter_angle / 360.0f, 0.0f, 1.0f);
        struct Data {
            rg::TextureHandle tiles;
            rg::TextureHandle neighbor;
            rg::TextureHandle out;
        };
        const Data& mb = graph->add_pass<Data>(
            "Motion blur",
            [&](rg::PassBuilder& b, Data& d) {
                d.tiles = b.create("VelocityTiles", gfx::tex2d(GL_RG16F, tw, th));
                d.neighbor = b.create("VelocityNeighborMax", gfx::tex2d(GL_RG16F, tw, th));
                d.out = b.create("MotionBlur", gfx::tex2d(GL_RGBA16F, ow, oh));
                b.write(d.tiles, rg::Access::Image);
                b.write(d.neighbor, rg::Access::Image);
                b.write(d.out, rg::Access::Image);
                b.read(current);
                b.read(velocity);
                b.read(depth);
            },
            [this, current, velocity, depth, scale, tw, th](const Data& d, const rg::PassContext& ctx) {
                const ProgramId tiles = shaders.compute("motion_tiles.comp");
                shaders.use(tiles);
                glUniform1f(shaders.uniform(tiles, "u_scale"), scale);
                glUniform1i(shaders.uniform(tiles, "u_pass"), 0);
                gfx::bind_texture(0, ctx.tex(velocity), gfx::SamplerKind::NearestClamp);
                glBindImageTexture(0, ctx.tex(d.tiles), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
                glDispatchCompute(gfx::groups(tw, 8), gfx::groups(th, 8), 1);
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
                glUniform1i(shaders.uniform(tiles, "u_pass"), 1);
                gfx::bind_texture(0, ctx.tex(d.tiles), gfx::SamplerKind::NearestClamp);
                glBindImageTexture(0, ctx.tex(d.neighbor), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
                glDispatchCompute(gfx::groups(tw, 8), gfx::groups(th, 8), 1);
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
                const ProgramId blur = shaders.compute("motion_blur.comp");
                shaders.use(blur);
                glUniform1f(shaders.uniform(blur, "u_scale"), scale);
                glUniform1i(shaders.uniform(blur, "u_samples"), frame.settings->post.motion_blur_samples);
                gfx::bind_texture(0, ctx.tex(current), gfx::SamplerKind::LinearClamp);
                gfx::bind_texture(1, ctx.tex(velocity), gfx::SamplerKind::NearestClamp);
                gfx::bind_texture(2, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
                gfx::bind_texture(3, ctx.tex(d.neighbor), gfx::SamplerKind::NearestClamp);
                const gfx::Texture& out = ctx.texture(d.out);
                glBindImageTexture(0, out.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                glDispatchCompute(gfx::groups(out.width(), 8), gfx::groups(out.height(), 8), 1);
            });
        current = mb.out;
    }

    // --- Bloom ---------------------------------------------------------------------------
    rg::TextureHandle bloom{};
    constexpr int kBloomLevels = 6;
    if (s.post.bloom && f.debug_view == 0) {
        const int bw = std::max(1, ow / 2);
        const int bh = std::max(1, oh / 2);
        const int levels = std::min(kBloomLevels, gfx::mip_count(bw, bh));
        struct Data {
            rg::TextureHandle down;
            rg::TextureHandle up;
        };
        const Data& data = graph->add_pass<Data>(
            "Bloom",
            [&](rg::PassBuilder& b, Data& d) {
                d.down = b.create("BloomDown", gfx::tex2d(GL_RGBA16F, bw, bh, levels));
                d.up = b.create("BloomUp", gfx::tex2d(GL_RGBA16F, bw, bh, levels));
                b.write(d.down, rg::Access::Image);
                b.write(d.up, rg::Access::Image);
                b.read(current);
            },
            [this, current, levels, bw, bh](const Data& d, const rg::PassContext& ctx) {
                const ProgramId down = shaders.compute("bloom_down.comp");
                shaders.use(down);
                for (int level = 0; level < levels; ++level) {
                    const int w = std::max(1, bw >> level);
                    const int h = std::max(1, bh >> level);
                    if (level == 0) {
                        gfx::bind_texture(0, ctx.tex(current), gfx::SamplerKind::LinearClamp);
                        glUniform1f(shaders.uniform(down, "u_source_lod"), 0.0f);
                    } else {
                        gfx::bind_texture(0, ctx.tex(d.down), gfx::SamplerKind::LinearMipClamp);
                        glUniform1f(shaders.uniform(down, "u_source_lod"), static_cast<float>(level - 1));
                    }
                    glUniform1i(shaders.uniform(down, "u_first"), level == 0 ? 1 : 0);
                    glBindImageTexture(0, ctx.tex(d.down), level, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                    glDispatchCompute(gfx::groups(w, 8), gfx::groups(h, 8), 1);
                    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
                }
                // Smallest level: copy down -> up.
                glCopyImageSubData(ctx.tex(d.down), GL_TEXTURE_2D, levels - 1, 0, 0, 0, ctx.tex(d.up), GL_TEXTURE_2D, levels - 1, 0, 0, 0, std::max(1, bw >> (levels - 1)),
                    std::max(1, bh >> (levels - 1)), 1);
                const ProgramId up = shaders.compute("bloom_up.comp");
                shaders.use(up);
                glUniform1f(shaders.uniform(up, "u_radius"), 0.5f + frame.settings->post.bloom_radius);
                for (int level = levels - 2; level >= 0; --level) {
                    const int w = std::max(1, bw >> level);
                    const int h = std::max(1, bh >> level);
                    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                    gfx::bind_texture(0, ctx.tex(d.up), gfx::SamplerKind::LinearMipClamp);
                    gfx::bind_texture(1, ctx.tex(d.down), gfx::SamplerKind::LinearMipClamp);
                    glUniform1f(shaders.uniform(up, "u_small_lod"), static_cast<float>(level + 1));
                    glUniform1f(shaders.uniform(up, "u_large_lod"), static_cast<float>(level));
                    glBindImageTexture(0, ctx.tex(d.up), level, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
                    glDispatchCompute(gfx::groups(w, 8), gfx::groups(h, 8), 1);
                }
            });
        bloom = data.up;
    }

    // --- Final composite (+ FXAA) ------------------------------------------------------
    const bool fxaa = s.aa.method == AntiAliasing::Fxaa;
    const float bloom_scale = 1.0f / static_cast<float>(kBloomLevels);
    struct Composite {
        rg::TextureHandle ldr;
    };
    const Composite& composite = graph->add_pass<Composite>(
        "Tonemap + grade",
        [&](rg::PassBuilder& b, Composite& d) {
            d.ldr = fxaa ? b.create("LDR", gfx::tex2d(GL_RGBA8, ow, oh)) : output_handle;
            b.write(d.ldr);
            b.read(current);
            if (bloom.valid()) {
                b.read(bloom);
            }
        },
        [this, current, bloom, bloom_scale](const Composite& d, const rg::PassContext& ctx) {
            const PostSettings& p = frame.settings->post;
            ctx.bind_targets({{d.ldr}});
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            const ProgramId program = shaders.graphics("fullscreen.vert", "post.frag");
            shaders.use(program);
            glUniform1f(shaders.uniform(program, "u_bloom"), p.bloom_intensity);
            glUniform1f(shaders.uniform(program, "u_dirt"), p.lens_dirt);
            glUniform1f(shaders.uniform(program, "u_vignette"), p.vignette);
            glUniform1f(shaders.uniform(program, "u_chromatic"), p.chromatic_aberration);
            glUniform1f(shaders.uniform(program, "u_grain"), p.film_grain);
            glUniform1i(shaders.uniform(program, "u_has_bloom"), bloom.valid() ? 1 : 0);
            gfx::bind_texture(0, ctx.tex(current), gfx::SamplerKind::LinearClamp);
            gfx::bind_texture(1, bloom.valid() ? ctx.tex(bloom) : textures.black(), gfx::SamplerKind::LinearClamp);
            gfx::bind_texture(2, grading_lut, gfx::SamplerKind::LinearClamp);
            gfx::bind_texture(3, lens_dirt, gfx::SamplerKind::LinearClamp);
            glUniform1f(shaders.uniform(program, "u_bloom_scale"), bloom_scale);
            gfx::draw_fullscreen_triangle();
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
        });
    if (fxaa) {
        const rg::TextureHandle ldr = composite.ldr;
        graph->add_pass(
            "FXAA",
            [&](rg::PassBuilder& b) {
                b.read(ldr);
                b.write(output_handle);
            },
            [this, ldr, output_handle](const rg::PassContext& ctx) {
                ctx.bind_targets({{output_handle}});
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                shaders.use(shaders.graphics("fullscreen.vert", "fxaa.frag"));
                gfx::bind_texture(0, ctx.tex(ldr), gfx::SamplerKind::LinearClamp);
                gfx::draw_fullscreen_triangle();
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_CULL_FACE);
            });
    }
    return output_handle;
}

} // namespace Hollow

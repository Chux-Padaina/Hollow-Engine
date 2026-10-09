// The frame render graph: declares every pass in order with the resources
// it reads and writes. The graph culls unused passes, aliases transient
// textures and inserts barriers (see RenderGraph.hpp).
#include "Render/RendererImpl.hpp"

#include <algorithm>
#include <cmath>

namespace Hollow {

void Renderer::Impl::compute_features() {
    FrameState& f = frame;
    const RenderPipelineSettings& s = *f.settings;
    using namespace gpu::feature;
    int bits = 0;
    if (s.gi.ibl) bits |= kIbl;
    if (s.gi.ibl && s.gi.reflection_probes && !probes.empty()) bits |= kProbes;
    if (s.gi.ibl && s.gi.irradiance_volumes && !volumes.empty()) bits |= kVolumes;
    if (s.shadows.enabled) bits |= kShadows;
    if (f.cascades) bits |= kCascades;
    if (s.shadows.filter == ShadowFilter::Pcss) bits |= kPcss;
    if (s.aa.specular_aa) bits |= kSpecularAa;
    if (!decals.empty()) bits |= kDecals;
    if (s.transparency.refraction) bits |= kRefraction;
    if (s.gi.ambient_occlusion) bits |= kAo;
    if (s.gi.ibl && s.gi.ssr) bits |= kSsr;
    if (s.shadows.enabled && s.shadows.contact_shadows && s.shadows.contact_length > 0.0f && f.cascades) bits |= kContactShadows;
    if (s.fog.enabled) bits |= kFog;
    if (s.fog.enabled && s.fog.volumetric) bits |= kVolumetricFog;
    if (f.editor) bits |= kEditor;
    f.feature_bits = bits;
}

void Renderer::Impl::build_graph() {
    FrameState& f = frame;
    const float projection_scale = 1.0f / std::tan(f.fov_y * 0.5f);
    const rg::BufferHandle commands = graph->import("DrawCommands", command_buffer);
    const rg::BufferHandle visibility_now = graph->import("Visibility", visibility[static_cast<std::size_t>(visibility_index)]);
    const rg::BufferHandle visibility_prev = graph->import("VisibilityPrevious", visibility[static_cast<std::size_t>(visibility_index ^ 1)]);
    const rg::TextureHandle output_handle = graph->import("Output", output);

    add_skinning_pass();
    add_shadow_passes(commands);

    // --- Main view culling (phase 1) ------------------------------------------
    if (f.gpu_driven) {
        if (f.occlusion) {
            add_cull_pass("Cull: main (phase 1)", f.main_phase1_offset, 1, gpu::cull_flag::kMainView, f.view_projection, f.frustum, f.camera_position,
                projection_scale, {}, commands, {}, visibility_prev);
        } else {
            add_cull_pass("Cull: main", f.main_union_offset, 0, gpu::cull_flag::kMainView, f.view_projection, f.frustum, f.camera_position, projection_scale, {},
                commands, {}, {});
        }
    }

    const auto [grass_visible_handle, grass_args_handle] = add_grass_cull_pass();
    auto read_grass = [grass_visible_handle, grass_args_handle](rg::PassBuilder& b) {
        if (grass_args_handle.valid()) {
            b.read(grass_visible_handle, rg::Access::Storage);
            b.read(grass_args_handle, rg::Access::Indirect);
        }
    };

    // --- Depth / normal / velocity prepass ---------------------------------------
    struct Prepass {
        rg::TextureHandle depth;
        rg::TextureHandle normals;
        rg::TextureHandle velocity;
    };
    const std::uint32_t first_offset = f.occlusion ? f.main_phase1_offset : f.main_union_offset;
    auto draw_prepass = [this](std::uint32_t offset) {
        const ProgramId opaque = mesh_program("PASS_PREPASS", false);
        const ProgramId masked = mesh_program("PASS_PREPASS", true);
        draw_batches(offset, {BatchCategory::Opaque, BatchCategory::Masked},
            [opaque, masked](const Batch& b) { return b.category == BatchCategory::Masked ? masked : opaque; }, true);
    };
    const Prepass& prepass = graph->add_pass<Prepass>(
        "Depth prepass",
        [&](rg::PassBuilder& b, Prepass& d) {
            d.depth = b.create("SceneDepth", gfx::tex2d(GL_DEPTH_COMPONENT32F, f.render_width, f.render_height));
            d.normals = b.create("SceneNormals", gfx::tex2d(GL_RGB10_A2, f.render_width, f.render_height));
            d.velocity = b.create("Velocity", gfx::tex2d(GL_RG16F, f.render_width, f.render_height));
            b.write(d.depth, rg::Access::Depth);
            b.write(d.normals);
            b.write(d.velocity);
            b.read(commands, rg::Access::Indirect);
            read_grass(b);
        },
        [this, first_offset, draw_prepass](const Prepass& d, const rg::PassContext& ctx) {
            ctx.bind_targets({{d.normals}, {d.velocity}}, {d.depth});
            const float zero[4] = {0, 0, 0, 0};
            glClearBufferfv(GL_COLOR, 0, zero);
            glClearBufferfv(GL_COLOR, 1, zero);
            const float depth_clear = 0.0f;
            glClearBufferfv(GL_DEPTH, 0, &depth_clear);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_TRUE);
            bind_scene_buffers();
            draw_prepass(first_offset);
            draw_grass(true);
        });

    rg::TextureHandle depth = prepass.depth;
    rg::TextureHandle normals = prepass.normals;
    rg::TextureHandle velocity = prepass.velocity;

    if (f.occlusion) {
        const rg::TextureHandle hiz = add_hiz_pass("Hi-Z (phase 1)", depth);
        add_cull_pass("Cull: main (phase 2, Hi-Z)", f.main_phase2_offset, 2, gpu::cull_flag::kMainView, f.view_projection, f.frustum, f.camera_position,
            projection_scale, hiz, commands, visibility_now, visibility_prev);
        graph->add_pass(
            "Depth prepass (phase 2)",
            [&](rg::PassBuilder& b) {
                b.write(depth, rg::Access::Depth);
                b.write(normals);
                b.write(velocity);
                b.read(commands, rg::Access::Indirect);
            },
            [this, depth, normals, velocity, draw_prepass](const rg::PassContext& ctx) {
                ctx.bind_targets({{normals}, {velocity}}, {depth});
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_GREATER);
                glDepthMask(GL_TRUE);
                bind_scene_buffers();
                draw_prepass(frame.main_phase2_offset);
            });
    }

    const auto [cluster_grid_handle, cluster_items_handle] = add_cluster_pass();

    // --- Screen-space lighting inputs ---------------------------------------------------
    const int features = f.feature_bits;
    using namespace gpu::feature;
    const rg::TextureHandle contact = (features & kContactShadows) ? add_contact_shadow_pass(depth) : rg::TextureHandle{};
    const rg::TextureHandle ao = (features & kAo) ? add_gtao_passes(depth, normals) : rg::TextureHandle{};
    const rg::TextureHandle ssr = (features & kSsr) ? add_ssr_passes(depth, normals) : rg::TextureHandle{};
    const rg::TextureHandle fog = (features & kVolumetricFog) ? add_fog_passes(cluster_grid_handle, cluster_items_handle) : rg::TextureHandle{};
    auto read_optional = [](rg::PassBuilder& b, rg::TextureHandle h) {
        if (h.valid()) {
            b.read(h);
        }
    };
    auto tex_or_zero = [](const rg::PassContext& ctx, rg::TextureHandle h) -> GLuint { return h.valid() ? ctx.tex(h) : 0; };

    // --- Forward opaque ------------------------------------------------------------
    struct Forward {
        rg::TextureHandle color;
    };
    const Forward& forward = graph->add_pass<Forward>(
        "Forward opaque",
        [&](rg::PassBuilder& b, Forward& d) {
            d.color = b.create("SceneColor", gfx::tex2d(GL_RGBA16F, f.render_width, f.render_height));
            b.write(d.color);
            b.read(depth, rg::Access::Depth);
            b.read(commands, rg::Access::Indirect);
            b.read(cluster_grid_handle, rg::Access::Storage);
            b.read(cluster_items_handle, rg::Access::Storage);
            read_optional(b, contact);
            read_optional(b, ao);
            read_optional(b, ssr);
            read_optional(b, fog);
            read_grass(b);
        },
        [this, depth, contact, ao, ssr, fog, tex_or_zero](const Forward& d, const rg::PassContext& ctx) {
            ctx.bind_targets({{d.color}}, {depth});
            const float zero[4] = {0, 0, 0, 1};
            glClearBufferfv(GL_COLOR, 0, zero);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_EQUAL);
            glDepthMask(GL_FALSE);
            bind_scene_buffers();
            bind_global_textures(0, 0, tex_or_zero(ctx, ao), tex_or_zero(ctx, ssr), tex_or_zero(ctx, contact), tex_or_zero(ctx, fog));
            const ProgramId opaque = mesh_program("PASS_FORWARD", false);
            const ProgramId masked = mesh_program("PASS_FORWARD", true);
            draw_batches(frame.main_union_offset, {BatchCategory::Opaque, BatchCategory::Masked},
                [opaque, masked](const Batch& b) { return b.category == BatchCategory::Masked ? masked : opaque; }, true);
            draw_grass(false);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_GREATER);
        });
    rg::TextureHandle color = forward.color;

    // --- Sky -----------------------------------------------------------------------------
    graph->add_pass(
        "Sky",
        [&](rg::PassBuilder& b) {
            b.write(color);
            b.read(depth, rg::Access::Depth);
            read_optional(b, fog);
        },
        [this, color, depth, fog, tex_or_zero](const rg::PassContext& ctx) {
            ctx.bind_targets({{color}}, {depth});
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GEQUAL); // only where the depth buffer is still clear (0)
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            const ProgramId program = shaders.graphics("fullscreen.vert", "sky.frag");
            shaders.use(program);
            bind_global_textures(0, 0, 0, 0, 0, tex_or_zero(ctx, fog));
            gfx::bind_texture(gpu::binding::kSkyView, sky_view_lut, gfx::SamplerKind::LinearClamp);
            GLuint background = 0;
            if (active_environment >= 0) {
                background = environments[static_cast<std::size_t>(active_environment)]->background.id();
            }
            glBindTextureUnit(gpu::binding::kExtra, background != 0 ? background : sky_capture.id());
            glBindSampler(gpu::binding::kExtra, gfx::sampler(gfx::SamplerKind::LinearMipClamp));
            gfx::draw_fullscreen_triangle();
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_GREATER);
            glEnable(GL_CULL_FACE);
        });

    // --- Water, glass, transparency, particles ------------------------------------------
    add_transparency_passes(color, depth, fog, cluster_grid_handle, cluster_items_handle);

    // --- Keep the lit scene for next frame's SSR -------------------------------------
    if (features & kSsr) {
        const rg::TextureHandle history = graph->import("SceneColorHistory", scene_color_history);
        graph->add_pass(
            "Copy scene color",
            [&](rg::PassBuilder& b) {
                b.read(color);
                b.write(history, rg::Access::Image);
            },
            [color, history](const rg::PassContext& ctx) {
                const gfx::Texture& src = ctx.texture(color);
                glCopyImageSubData(src.id(), GL_TEXTURE_2D, 0, 0, 0, 0, ctx.tex(history), GL_TEXTURE_2D, 0, 0, 0, 0, src.width(), src.height(), 1);
            });
    }

    // --- Post processing -> output ------------------------------------------------------
    add_post_passes(color, depth, velocity, output_handle);

    // --- Editor overlay --------------------------------------------------------------
    if (f.editor) {
        add_overlay_passes(depth, output_handle);
    }
}

} // namespace Hollow

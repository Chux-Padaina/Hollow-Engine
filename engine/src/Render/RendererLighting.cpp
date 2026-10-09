// Lighting support passes: clustered light/decal/probe culling.
// (GTAO, contact shadows, SSR and volumetric fog are added to the graph in
// RendererGraph.cpp with their helpers below.)
#include "Render/RendererImpl.hpp"

#include <algorithm>

namespace Hollow {

std::pair<rg::BufferHandle, rg::BufferHandle> Renderer::Impl::add_cluster_pass() {
    const FrameState& f = frame;
    const rg::BufferHandle grid = graph->import("ClusterGrid", cluster_grid);
    const rg::BufferHandle items = graph->import("ClusterItems", cluster_items);
    graph->add_pass(
        "Light clusters",
        [&](rg::PassBuilder& b) {
            b.write(grid, rg::Access::Storage);
            b.write(items, rg::Access::Storage);
        },
        [this, grid, items](const rg::PassContext& ctx) {
            const gpu::FrameUniforms& u = frame_uniforms;
            const auto cluster_count = static_cast<int>(u.cluster.x * u.cluster.y * u.cluster.z);
            const std::uint32_t zero = 0;
            glNamedBufferSubData(ctx.buf(items), 0, sizeof(std::uint32_t), &zero);
            const ProgramId program = shaders.compute("clusters.comp");
            shaders.use(program);
            const auto capacity = static_cast<GLuint>(ctx.buffer(items).size() / sizeof(std::uint32_t) - 1);
            glUniform1ui(shaders.uniform(program, "u_capacity"), capacity);
            bind_scene_buffers();
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kClusterGrid, ctx.buf(grid));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kClusterItems, ctx.buf(items));
            glDispatchCompute(gfx::groups(cluster_count, 64), 1, 1);
        });
    (void)f;
    return {grid, items};
}

} // namespace Hollow

namespace Hollow {

rg::TextureHandle Renderer::Impl::add_contact_shadow_pass(rg::TextureHandle depth) {
    const FrameState& f = frame;
    struct Data {
        rg::TextureHandle out;
    };
    const Data& data = graph->add_pass<Data>(
        "Contact shadows",
        [&](rg::PassBuilder& b, Data& d) {
            d.out = b.create("ContactShadows", gfx::tex2d(GL_R8, f.render_width, f.render_height));
            b.write(d.out, rg::Access::Image);
            b.read(depth);
        },
        [this, depth](const Data& d, const rg::PassContext& ctx) {
            const ProgramId program = shaders.compute("contact_shadows.comp");
            shaders.use(program);
            glUniform1f(shaders.uniform(program, "u_length"), frame.settings->shadows.contact_length);
            glUniform1i(shaders.uniform(program, "u_steps"), 16);
            gfx::bind_texture(0, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
            const gfx::Texture& out = ctx.texture(d.out);
            glBindImageTexture(0, out.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
            glDispatchCompute(gfx::groups(out.width(), 8), gfx::groups(out.height(), 8), 1);
        });
    return data.out;
}

rg::TextureHandle Renderer::Impl::add_gtao_passes(rg::TextureHandle depth, rg::TextureHandle normals) {
    const FrameState& f = frame;
    struct Data {
        rg::TextureHandle raw;
        rg::TextureHandle out;
    };
    const Data& data = graph->add_pass<Data>(
        "GTAO",
        [&](rg::PassBuilder& b, Data& d) {
            d.raw = b.create("AORaw", gfx::tex2d(GL_R16F, f.render_width, f.render_height));
            d.out = b.create("AmbientOcclusion", gfx::tex2d(GL_R16F, f.render_width, f.render_height));
            b.write(d.raw, rg::Access::Image);
            b.write(d.out, rg::Access::Image);
            b.read(depth);
            b.read(normals);
        },
        [this, depth, normals](const Data& d, const rg::PassContext& ctx) {
            const GiSettings& gi = frame.settings->gi;
            const ProgramId program = shaders.compute("gtao.comp");
            shaders.use(program);
            glUniform1i(shaders.uniform(program, "u_slices"), std::clamp(gi.ao_slices, 1, 4));
            glUniform1i(shaders.uniform(program, "u_steps"), 6);
            glUniform1f(shaders.uniform(program, "u_radius"), gi.ao_radius);
            glUniform1f(shaders.uniform(program, "u_proj_scale"), static_cast<float>(frame.render_height) * 0.5f * frame.projection.m[5]);
            gfx::bind_texture(0, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
            gfx::bind_texture(1, ctx.tex(normals), gfx::SamplerKind::NearestClamp);
            const gfx::Texture& raw = ctx.texture(d.raw);
            glBindImageTexture(0, raw.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
            glDispatchCompute(gfx::groups(raw.width(), 8), gfx::groups(raw.height(), 8), 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
            shaders.use(shaders.compute("ao_blur.comp"));
            gfx::bind_texture(0, raw.id(), gfx::SamplerKind::NearestClamp);
            gfx::bind_texture(1, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
            glBindImageTexture(0, ctx.tex(d.out), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
            glDispatchCompute(gfx::groups(raw.width(), 8), gfx::groups(raw.height(), 8), 1);
        });
    return data.out;
}

rg::TextureHandle Renderer::Impl::add_ssr_passes(rg::TextureHandle depth, rg::TextureHandle normals) {
    const FrameState& f = frame;
    if (scene_color_history.ensure(gfx::tex2d(GL_RGBA16F, f.render_width, f.render_height), "SceneColorHistory")) {
        const float black[4] = {0, 0, 0, 0};
        glClearTexImage(scene_color_history.id(), 0, GL_RGBA, GL_FLOAT, black);
    }
    const rg::TextureHandle previous = graph->import("SceneColorHistory", scene_color_history);
    struct Data {
        rg::TextureHandle traced;
        rg::TextureHandle out;
    };
    const Data& data = graph->add_pass<Data>(
        "SSR",
        [&](rg::PassBuilder& b, Data& d) {
            d.traced = b.create("SSRTrace", gfx::tex2d(GL_RGBA16F, f.render_width, f.render_height));
            d.out = b.create("SSR", gfx::tex2d(GL_RGBA16F, f.render_width, f.render_height));
            b.write(d.traced, rg::Access::Image);
            b.write(d.out, rg::Access::Image);
            b.read(depth);
            b.read(normals);
            b.read(previous);
        },
        [this, depth, normals, previous](const Data& d, const rg::PassContext& ctx) {
            const GiSettings& gi = frame.settings->gi;
            const ProgramId program = shaders.compute("ssr.comp");
            shaders.use(program);
            glUniform1i(shaders.uniform(program, "u_steps"), std::clamp(gi.ssr_steps, 8, 128));
            glUniform1f(shaders.uniform(program, "u_thickness"), gi.ssr_thickness);
            glUniform1f(shaders.uniform(program, "u_max_roughness"), gi.ssr_max_roughness);
            glUniform1f(shaders.uniform(program, "u_exposure_ratio"), frame_uniforms.exposure.x / std::max(frame_uniforms.exposure.z, 1e-12f));
            gfx::bind_texture(0, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
            gfx::bind_texture(1, ctx.tex(normals), gfx::SamplerKind::NearestClamp);
            gfx::bind_texture(2, ctx.tex(previous), gfx::SamplerKind::LinearClamp);
            const gfx::Texture& traced = ctx.texture(d.traced);
            glBindImageTexture(0, traced.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(gfx::groups(traced.width(), 8), gfx::groups(traced.height(), 8), 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
            shaders.use(shaders.compute("ssr_resolve.comp"));
            gfx::bind_texture(0, traced.id(), gfx::SamplerKind::NearestClamp);
            gfx::bind_texture(1, ctx.tex(normals), gfx::SamplerKind::NearestClamp);
            gfx::bind_texture(2, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
            glBindImageTexture(0, ctx.tex(d.out), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(gfx::groups(traced.width(), 8), gfx::groups(traced.height(), 8), 1);
        });
    return data.out;
}

rg::TextureHandle Renderer::Impl::add_fog_passes(rg::BufferHandle cluster_grid_handle, rg::BufferHandle cluster_items_handle) {
    const FrameState& f = frame;
    const int fx = std::max(1, (f.render_width + 7) / 8);
    const int fy = std::max(1, (f.render_height + 7) / 8);
    const int fz = 64;
    const gfx::TextureDesc desc = gfx::tex3d(GL_RGBA16F, fx, fy, fz);
    bool reset = false;
    for (gfx::Texture& t : fog_history) {
        if (t.ensure(desc, "FogScatter")) {
            reset = true;
        }
    }
    if (reset) {
        fog_history_valid = false;
    }
    const rg::TextureHandle scatter = graph->import("FogScatter", fog_history[static_cast<std::size_t>(fog_index)]);
    const rg::TextureHandle history = graph->import("FogScatterPrevious", fog_history[static_cast<std::size_t>(fog_index ^ 1)]);
    const bool history_ok = fog_history_valid;
    fog_history_valid = true;
    struct Data {
        rg::TextureHandle integrated;
    };
    const Data& data = graph->add_pass<Data>(
        "Volumetric fog",
        [&](rg::PassBuilder& b, Data& d) {
            d.integrated = b.create("FogVolume", desc);
            b.write(scatter, rg::Access::Image);
            b.read(history);
            b.write(d.integrated, rg::Access::Image);
            b.read(cluster_grid_handle, rg::Access::Storage);
            b.read(cluster_items_handle, rg::Access::Storage);
        },
        [this, scatter, history, history_ok, fx, fy, fz](const Data& d, const rg::PassContext& ctx) {
            bind_scene_buffers();
            bind_global_textures(0, 0, 0, 0, 0, 0);
            const ProgramId inject = shaders.compute("fog_inject.comp");
            shaders.use(inject);
            glUniform1i(shaders.uniform(inject, "u_history_valid"), history_ok ? 1 : 0);
            gfx::bind_texture(0, ctx.tex(history), gfx::SamplerKind::LinearClamp);
            glBindImageTexture(0, ctx.tex(scatter), 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(gfx::groups(fx, 4), gfx::groups(fy, 4), gfx::groups(fz, 4));
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
            shaders.use(shaders.compute("fog_integrate.comp"));
            gfx::bind_texture(0, ctx.tex(scatter), gfx::SamplerKind::LinearClamp);
            glBindImageTexture(0, ctx.tex(d.integrated), 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(gfx::groups(fx, 8), gfx::groups(fy, 8), 1);
        });
    return data.integrated;
}

} // namespace Hollow

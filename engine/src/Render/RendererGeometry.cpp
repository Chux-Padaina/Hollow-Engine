// Geometry: shader permutations for mesh passes, batch drawing (multi-draw
// indirect on the GPU-driven path, one draw per visible instance on the
// CPU path), GPU and CPU culling, and the Hi-Z depth pyramid.
#include "Render/RendererImpl.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace Hollow {

namespace {

Vec4 plane_v4(const Plane& p) {
    return {p.normal.x, p.normal.y, p.normal.z, p.distance};
}

} // namespace

ProgramId Renderer::Impl::mesh_program(const char* pass_define, bool masked, std::initializer_list<const char*> extra) {
    std::vector<std::string> defines{pass_define};
    if (masked) {
        defines.emplace_back("ALPHA_MASK");
    }
    for (const char* e : extra) {
        defines.emplace_back(e);
    }
    const std::string pass = pass_define;
    std::string fragment = "forward.frag";
    if (pass == "PASS_PREPASS") {
        fragment = "prepass.frag";
    } else if (pass == "PASS_SHADOW") {
        fragment = "shadow.frag";
    } else if (pass == "PASS_SELECTION") {
        fragment = "selection.frag";
    }
    return shaders.graphics("mesh.vert", fragment, defines);
}

void Renderer::Impl::bind_texture_set(std::uint32_t set) {
    if (set >= texture_sets.size()) {
        return;
    }
    const TextureSet& ts = texture_sets[set];
    glBindTextures(0, static_cast<GLsizei>(ts.ids.size()), ts.ids.data());
    const GLuint material_sampler = gfx::sampler(gfx::SamplerKind::LinearMipRepeatAniso);
    std::array<GLuint, gpu::binding::kMaterialTextureCount> samplers;
    samplers.fill(material_sampler);
    glBindSamplers(0, static_cast<GLsizei>(samplers.size()), samplers.data());
}

void Renderer::Impl::draw_batches(std::uint32_t command_offset, std::initializer_list<BatchCategory> categories, const std::function<ProgramId(const Batch&)>& program_for,
    bool bind_textures, GLuint commands_override) {
    const bool indirect = commands_override != 0 || frame.gpu_driven;
    if (instances.empty()) {
        return;
    }
    glBindVertexArray(pool.vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, commands_override != 0 ? commands_override : command_buffer.id());
    ProgramId current = kNoProgram;
    int cull_state = -1;
    for (const Batch& batch : batches) {
        if (std::find(categories.begin(), categories.end(), batch.category) == categories.end()) {
            continue;
        }
        const ProgramId program = program_for(batch);
        if (program == kNoProgram) {
            continue;
        }
        if (program != current) {
            shaders.use(program);
            current = program;
        }
        const int wanted_cull = batch.double_sided ? 0 : 1;
        if (wanted_cull != cull_state) {
            if (wanted_cull) {
                glEnable(GL_CULL_FACE);
            } else {
                glDisable(GL_CULL_FACE);
            }
            cull_state = wanted_cull;
        }
        if (bind_textures) {
            bind_texture_set(batch.texture_set);
        }
        if (indirect) {
            const auto offset = static_cast<std::uintptr_t>(command_offset + batch.first) * sizeof(gpu::DrawCommand);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(offset), static_cast<GLsizei>(batch.count), 0);
            ++stats.draw_calls;
        } else {
            for (std::uint32_t i = 0; i < batch.count; ++i) {
                const gpu::DrawCommand& cmd = cpu_commands[command_offset + batch.first + i];
                if (cmd.instance_count == 0) {
                    continue;
                }
                glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, static_cast<GLsizei>(cmd.count), GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<std::uintptr_t>(cmd.first_index) * sizeof(std::uint32_t)), 1, cmd.base_vertex, cmd.base_instance);
                ++stats.draw_calls;
            }
        }
    }
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
}

void Renderer::Impl::cpu_cull(std::uint32_t command_offset, const Frustum& frustum, bool shadow_casters) {
    const FrameState& f = frame;
    const bool use_frustum = f.settings->performance.frustum_culling;
    const bool lods = f.settings->performance.lods;
    const float projection_scale = 1.0f / std::tan(f.fov_y * 0.5f);
    const float bias = f.settings->performance.lod_bias;
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const gpu::Instance& inst = instances[i];
        gpu::DrawCommand& cmd = cpu_commands[command_offset + i];
        const gpu::MeshInfo& mesh = pool.info_shadow[inst.mesh];
        const Vec3 center{inst.bounds.x, inst.bounds.y, inst.bounds.z};
        bool visible = !shadow_casters || (inst.flags & gpu::instance_flag::kCastShadows) != 0;
        if (visible && use_frustum) {
            visible = frustum.intersects_sphere(center, inst.bounds.w);
        }
        std::uint32_t lod = 0;
        if (lods && mesh.lod_levels > 1) {
            const float distance = std::max(length(center - f.camera_position) - inst.bounds.w, 0.01f);
            const float screen = inst.bounds.w / distance * projection_scale * bias;
            for (std::uint32_t l = 1; l < mesh.lod_levels; ++l) {
                if (screen < mesh.lod_error[l]) {
                    lod = l;
                }
            }
        }
        cmd.count = mesh.lod_count[lod];
        cmd.instance_count = visible ? 1u : 0u;
        cmd.first_index = mesh.lod_first[lod];
        cmd.base_vertex = inst.base_vertex;
        cmd.base_instance = i;
        if (!shadow_casters && command_offset == f.main_union_offset && visible) {
            ++stats.visible_instances;
            stats.triangles += cmd.count / 3;
        }
    }
}

void Renderer::Impl::add_cull_pass(const char* name, std::uint32_t command_offset, std::uint32_t mode, std::uint32_t flags, const Mat4& cull_view_projection,
    const Frustum& frustum, Vec3 lod_reference, float projection_scale, rg::TextureHandle hiz, rg::BufferHandle commands, rg::BufferHandle visibility_now,
    rg::BufferHandle visibility_prev) {
    const FrameState& f = frame;
    if (f.instance_count == 0) {
        return;
    }
    const RenderPipelineSettings& s = *f.settings;
    gpu::CullUniforms u{};
    for (int i = 0; i < 6; ++i) {
        u.planes[static_cast<std::size_t>(i)] = plane_v4(frustum.planes[static_cast<std::size_t>(i)]);
    }
    u.view_projection = cull_view_projection;
    u.camera = {lod_reference.x, lod_reference.y, lod_reference.z, projection_scale};
    if (s.performance.frustum_culling) {
        flags |= gpu::cull_flag::kFrustum;
    }
    if (s.performance.lods) {
        flags |= gpu::cull_flag::kLod;
    }
    u.params[0] = f.instance_count;
    u.params[1] = command_offset;
    u.params[2] = mode;
    u.params[3] = flags;
    u.hiz_size = {static_cast<float>(f.render_width), static_cast<float>(f.render_height), static_cast<float>(gfx::mip_count(f.render_width, f.render_height)),
        s.performance.lod_bias};
    u.params2[0] = f.main_union_offset;
    graph->add_pass(
        name,
        [&](rg::PassBuilder& b) {
            b.write(commands, rg::Access::Storage);
            if (visibility_now.valid()) {
                b.write(visibility_now, rg::Access::Storage);
            }
            if (visibility_prev.valid()) {
                b.read(visibility_prev, rg::Access::Storage);
            }
            if (hiz.valid()) {
                b.read(hiz, rg::Access::Sampled);
            }
        },
        [this, u, hiz, commands, visibility_now, visibility_prev, mode, flags](const rg::PassContext& ctx) {
            const ProgramId program = shaders.compute("cull.comp");
            shaders.use(program);
            bind_pass_uniforms(u);
            const bool main_view = (flags & gpu::cull_flag::kMainView) != 0;
            if (!cull_stats) {
                cull_stats = gfx::Buffer(sizeof(std::uint32_t), GL_DYNAMIC_STORAGE_BIT, nullptr, "CullStats");
                for (gfx::Buffer& b : cull_readback) {
                    b = gfx::Buffer(sizeof(std::uint32_t), GL_DYNAMIC_STORAGE_BIT, nullptr, "CullStatsReadback");
                    b.clear_u32(0);
                }
            }
            if (main_view && mode != 2) {
                cull_stats.clear_u32(0); // first main-view cull of the frame
            }
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, cull_stats.id());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kCommands, ctx.buf(commands));
            if (visibility_now.valid()) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kVisibility, ctx.buf(visibility_now));
            }
            if (visibility_prev.valid()) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kPrevVisibility, ctx.buf(visibility_prev));
            }
            if (hiz.valid()) {
                gfx::bind_texture(gpu::binding::kExtra, ctx.tex(hiz), gfx::SamplerKind::NearestMipClamp);
            }
            glDispatchCompute(gfx::groups(static_cast<int>(frame.instance_count), 64), 1, 1);
            // The final main-view result: queue a copy, read the one from two frames ago.
            if (main_view && mode != 1) {
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
                const std::size_t slot = static_cast<std::size_t>(frame_index % 3);
                glCopyNamedBufferSubData(cull_stats.id(), cull_readback[slot].id(), 0, 0, sizeof(std::uint32_t));
                std::uint32_t count = 0;
                glGetNamedBufferSubData(cull_readback[(slot + 1) % 3].id(), 0, sizeof(count), &count);
                last_visible_instances = static_cast<int>(count);
            }
        });
}

rg::TextureHandle Renderer::Impl::add_hiz_pass(const char* name, rg::TextureHandle depth) {
    const FrameState& f = frame;
    const int mips = gfx::mip_count(f.render_width, f.render_height);
    struct Data {
        rg::TextureHandle hiz;
    };
    const Data& data = graph->add_pass<Data>(
        name,
        [&](rg::PassBuilder& b, Data& d) {
            d.hiz = b.create(name, gfx::tex2d(GL_R32F, f.render_width, f.render_height, mips));
            b.write(d.hiz, rg::Access::Image);
            b.read(depth, rg::Access::Sampled);
        },
        [this, depth, mips](const Data& d, const rg::PassContext& ctx) {
            const ProgramId program = shaders.compute("hiz.comp");
            const GLuint gl = shaders.use(program);
            const gfx::Texture& hiz = ctx.texture(d.hiz);
            // Level 0: copy depth.
            glUniform1i(shaders.uniform(program, "u_first"), 1);
            glUniform1i(shaders.uniform(program, "u_source_level"), 0);
            gfx::bind_texture(0, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
            glBindImageTexture(0, hiz.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            glDispatchCompute(gfx::groups(hiz.width(), 8), gfx::groups(hiz.height(), 8), 1);
            glUniform1i(shaders.uniform(program, "u_first"), 0);
            gfx::bind_texture(0, hiz.id(), gfx::SamplerKind::NearestMipClamp);
            for (int level = 1; level < mips; ++level) {
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                glUniform1i(shaders.uniform(program, "u_source_level"), level - 1);
                glBindImageTexture(0, hiz.id(), level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
                glDispatchCompute(gfx::groups(std::max(1, hiz.width() >> level), 8), gfx::groups(std::max(1, hiz.height() >> level), 8), 1);
            }
            (void)gl;
        });
    return data.hiz;
}

} // namespace Hollow

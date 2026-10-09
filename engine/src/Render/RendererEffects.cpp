// Grass, GPU particles and the transparent stage (refraction, water,
// sorted / order-independent transparency); skinning.
#include "Render/RendererImpl.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace Hollow {

namespace {

constexpr std::uint32_t kGrassVertices = 21; // 3 quads + tip
constexpr std::size_t kParticleStride = 48;  // particle_common.glsl

void set_mat4(GLint location, const Mat4& m) {
    glUniformMatrix4fv(location, 1, GL_FALSE, m.m.data());
}

} // namespace

// --- Skinning -------------------------------------------------------------------------

void Renderer::Impl::setup_skinned_instance(std::uint64_t key, const MeshAsset& mesh, const PooledMesh& pooled, const AnimatorComponent& animator,
    gpu::Instance& instance) {
    const auto vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    auto [it, created] = skinned_regions.try_emplace(key);
    SkinnedRegion& region = it->second;
    bool fresh = created;
    if (!created && region.mesh_id != mesh.id) {
        pool.vertex_alloc.free(region.regions[0]);
        pool.vertex_alloc.free(region.regions[1]);
        fresh = true;
    }
    if (fresh) {
        allocate_vertices(vertex_count, region.regions[0]);
        allocate_vertices(vertex_count, region.regions[1]);
        region.mesh_id = mesh.id;
        region.current = 0;
    } else {
        region.current ^= 1;
    }
    region.last_used = frame_index;

    // Per-vertex influences, uploaded once per mesh.
    auto skin_it = skin_offsets.find(mesh.id);
    if (skin_it == skin_offsets.end()) {
        std::vector<std::uint32_t> packed;
        packed.reserve(mesh.skin.size() * 4);
        auto unorm = [](float w) { return static_cast<std::uint32_t>(std::lround(std::clamp(w, 0.0f, 1.0f) * 65535.0f)); };
        for (const SkinInfluence& s : mesh.skin) {
            packed.push_back(static_cast<std::uint32_t>(s.joints[0]) | (static_cast<std::uint32_t>(s.joints[1]) << 16));
            packed.push_back(static_cast<std::uint32_t>(s.joints[2]) | (static_cast<std::uint32_t>(s.joints[3]) << 16));
            packed.push_back(unorm(s.weights[0]) | (unorm(s.weights[1]) << 16));
            packed.push_back(unorm(s.weights[2]) | (unorm(s.weights[3]) << 16));
        }
        const std::size_t needed = (static_cast<std::size_t>(skin_buffer_used) + mesh.skin.size()) * 16;
        if (needed > skin_buffer.size()) {
            skin_buffer.grow_preserve(std::max(needed, skin_buffer.size() * 2), "SkinData");
        }
        skin_buffer.upload(packed.data(), packed.size() * sizeof(std::uint32_t), static_cast<std::size_t>(skin_buffer_used) * 16);
        skin_it = skin_offsets.emplace(mesh.id, skin_buffer_used).first;
        skin_buffer_used += static_cast<std::uint32_t>(mesh.skin.size());
    }

    // Skinning palette: joint (model space) * inverse bind.
    const auto joint_offset = static_cast<std::uint32_t>(joint_palette.size());
    for (std::size_t i = 0; i < mesh.skin_joints.size(); ++i) {
        const int joint = mesh.skin_joints[i];
        const Mat4 world = joint >= 0 && static_cast<std::size_t>(joint) < animator.joint_matrices.size() ? animator.joint_matrices[static_cast<std::size_t>(joint)] : Mat4::identity();
        joint_palette.push_back(world * (i < mesh.inverse_bind.size() ? mesh.inverse_bind[i] : Mat4::identity()));
    }

    const RangeAllocator::Range& current = region.regions[static_cast<std::size_t>(region.current)];
    const RangeAllocator::Range& previous = region.regions[static_cast<std::size_t>(region.current ^ 1)];
    SkinDispatch dispatch;
    dispatch.source_vertex = pooled.vertex_range.offset;
    dispatch.dest_vertex = current.offset;
    dispatch.vertex_count = vertex_count;
    dispatch.skin_offset = skin_it->second;
    dispatch.joint_offset = joint_offset;
    skin_dispatches.push_back(dispatch);
    instance.base_vertex = static_cast<std::int32_t>(current.offset);
    instance.prev_base_vertex = static_cast<std::int32_t>(fresh ? current.offset : previous.offset);
}

void Renderer::Impl::prepare_skinning() {
    if (joint_palette.empty()) {
        return;
    }
    joint_buffer.reserve(joint_palette.size() * sizeof(Mat4), GL_DYNAMIC_STORAGE_BIT, "JointPalette");
    joint_buffer.upload(joint_palette.data(), joint_palette.size() * sizeof(Mat4));
}

void Renderer::Impl::add_skinning_pass() {
    if (skin_dispatches.empty()) {
        return;
    }
    graph->add_pass(
        "Skinning",
        [&](rg::PassBuilder& b) { b.side_effect(); },
        [this](const rg::PassContext&) {
            const ProgramId program = shaders.compute("skin.comp");
            if (program == kNoProgram) {
                return;
            }
            shaders.use(program);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kVertices, pool.vertices.id());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, skin_buffer.id());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch1, joint_buffer.id());
            for (const SkinDispatch& d : skin_dispatches) {
                glUniform1ui(shaders.uniform(program, "u_source"), d.source_vertex);
                glUniform1ui(shaders.uniform(program, "u_dest"), d.dest_vertex);
                glUniform1ui(shaders.uniform(program, "u_count"), d.vertex_count);
                glUniform1ui(shaders.uniform(program, "u_skin_offset"), d.skin_offset);
                glUniform1ui(shaders.uniform(program, "u_joint_offset"), d.joint_offset);
                glDispatchCompute(gfx::groups(static_cast<int>(d.vertex_count), 64), 1, 1);
            }
            glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
            bind_scene_buffers();
        });
}

// --- Extraction -------------------------------------------------------------------

void Renderer::Impl::extract_effects(const std::vector<const Actor*>& actors) {
    FrameState& f = frame;
    grass_draws.clear();
    particle_draws.clear();
    std::uint32_t grass_total = 0;
    for (const Actor* actor : actors) {
        if (const auto* terrain = actor->find_component<TerrainComponent>()) {
            const auto it = terrains.find(actor->id());
            if (it != terrains.end() && it->second.grass_count > 0 && terrain->grass_distance > 0.0f) {
                GrassDraw draw;
                draw.cache = &it->second;
                draw.model = actor->world_matrix();
                draw.color = terrain->grass_color;
                draw.distance = terrain->grass_distance;
                draw.wind = terrain->wind;
                draw.offset = grass_total;
                grass_total += it->second.grass_count;
                grass_draws.push_back(draw);
            }
        }
        if (const auto* emitter = actor->find_component<ParticleEmitterComponent>()) {
            ParticleSystemState& state = particle_systems[actor->id()];
            state.last_used = frame_index;
            const int capacity = std::clamp(emitter->max_particles, 16, 1 << 18);
            if (state.capacity != capacity || !state.particles) {
                state.particles = gfx::Buffer(static_cast<std::size_t>(capacity) * kParticleStride, GL_DYNAMIC_STORAGE_BIT, nullptr, "Particles");
                state.particles.clear_u32(0); // age 0 >= lifetime 0: dead
                state.capacity = capacity;
                state.emit_cursor = 0;
                state.emit_accumulator = 0.0f;
            }
            ParticleDraw draw;
            draw.state = &state;
            draw.emitter = emitter;
            const Mat4& world = actor->world_matrix();
            draw.spawn = emitter->world_space ? world : Mat4::identity();
            draw.model = emitter->world_space ? Mat4::identity() : world;
            if (emitter->emitting && f.delta > 0.0f) {
                state.emit_accumulator += std::max(emitter->rate, 0.0f) * std::min(f.delta, 0.1f);
            }
            const auto count = static_cast<std::uint32_t>(std::min(std::floor(state.emit_accumulator), static_cast<float>(capacity)));
            state.emit_accumulator -= static_cast<float>(count);
            draw.emit_start = state.emit_cursor;
            draw.emit_count = count;
            draw.seed = static_cast<std::uint32_t>(frame_index * 2654435761ull) ^ static_cast<std::uint32_t>(actor->id());
            state.emit_cursor = (state.emit_cursor + count) % static_cast<std::uint32_t>(capacity);
            particle_draws.push_back(draw);
        }
    }
    f.has_particles = !particle_draws.empty();

    // Drop systems and terrains that are gone.
    std::erase_if(particle_systems, [this](const auto& entry) { return entry.second.last_used + 2 < frame_index; });
    std::erase_if(terrains, [this](const auto& entry) { return entry.second.last_used + 120 < frame_index; });

    if (grass_total > 0) {
        grass_visible.reserve(grass_total * sizeof(std::uint32_t), GL_DYNAMIC_STORAGE_BIT, "Grass.Visible");
        grass_args.reserve(std::max<std::size_t>(grass_draws.size(), 1) * 4 * sizeof(std::uint32_t), GL_DYNAMIC_STORAGE_BIT, "Grass.Args");
    }
    if (empty_vao == 0) {
        glCreateVertexArrays(1, &empty_vao);
    }
}

// --- Grass ---------------------------------------------------------------------------

std::pair<rg::BufferHandle, rg::BufferHandle> Renderer::Impl::add_grass_cull_pass() {
    if (grass_draws.empty()) {
        return {};
    }
    const rg::BufferHandle visible = graph->import("GrassVisible", grass_visible);
    const rg::BufferHandle args = graph->import("GrassArgs", grass_args);
    graph->add_pass(
        "Grass cull",
        [&](rg::PassBuilder& b) {
            b.write(visible, rg::Access::Storage);
            b.write(args, rg::Access::Storage);
        },
        [this, visible, args](const rg::PassContext& ctx) {
            std::vector<std::uint32_t> commands;
            for (std::size_t i = 0; i < grass_draws.size(); ++i) {
                commands.insert(commands.end(), {kGrassVertices, 0u, 0u, 0u});
            }
            glNamedBufferSubData(ctx.buf(args), 0, static_cast<GLsizeiptr>(commands.size() * sizeof(std::uint32_t)), commands.data());
            const ProgramId program = shaders.compute("grass_cull.comp");
            shaders.use(program);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch1, ctx.buf(visible));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kCommands, ctx.buf(args));
            for (std::size_t i = 0; i < grass_draws.size(); ++i) {
                const GrassDraw& g = grass_draws[i];
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, g.cache->grass.id());
                set_mat4(shaders.uniform(program, "u_model"), g.model);
                glUniform1f(shaders.uniform(program, "u_distance"), g.distance);
                glUniform1ui(shaders.uniform(program, "u_offset"), g.offset);
                glUniform1ui(shaders.uniform(program, "u_count"), g.cache->grass_count);
                glUniform1ui(shaders.uniform(program, "u_command"), static_cast<GLuint>(i));
                glDispatchCompute(gfx::groups(static_cast<int>(g.cache->grass_count), 64), 1, 1);
            }
            bind_scene_buffers();
        });
    return {visible, args};
}

void Renderer::Impl::draw_grass(bool prepass) {
    if (grass_draws.empty()) {
        return;
    }
    const ProgramId program = prepass ? shaders.graphics("grass.vert", "grass.frag", {"PASS_PREPASS"}) : shaders.graphics("grass.vert", "grass.frag");
    if (program == kNoProgram) {
        return;
    }
    shaders.use(program);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(empty_vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, grass_args.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch1, grass_visible.id());
    for (std::size_t i = 0; i < grass_draws.size(); ++i) {
        const GrassDraw& g = grass_draws[i];
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, g.cache->grass.id());
        set_mat4(shaders.uniform(program, "u_model"), g.model);
        glUniform1f(shaders.uniform(program, "u_distance"), g.distance);
        glUniform1f(shaders.uniform(program, "u_wind"), g.wind);
        glUniform1ui(shaders.uniform(program, "u_offset"), g.offset);
        glUniform3f(shaders.uniform(program, "u_color"), g.color.x, g.color.y, g.color.z);
        glDrawArraysIndirect(GL_TRIANGLES, reinterpret_cast<const void*>(i * 4 * sizeof(std::uint32_t)));
        ++stats.draw_calls;
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    bind_scene_buffers();
}

// --- Transparent stage ----------------------------------------------------------------

void Renderer::Impl::add_transparency_passes(rg::TextureHandle color, rg::TextureHandle depth, rg::TextureHandle fog, rg::BufferHandle cluster_grid_handle,
    rg::BufferHandle cluster_items_handle) {
    const FrameState& f = frame;
    const bool has_transparent = f.has_transparent;
    if (!f.has_refractive && !has_transparent && !f.has_particles) {
        return;
    }
    auto read_optional = [](rg::PassBuilder& b, rg::TextureHandle h) {
        if (h.valid()) {
            b.read(h);
        }
    };
    auto tex_or_zero = [](const rg::PassContext& ctx, rg::TextureHandle h) -> GLuint { return h.valid() ? ctx.tex(h) : 0; };
    const rg::BufferHandle commands = graph->import("DrawCommands", command_buffer);

    // Copy of the opaque scene: refraction source (with mips for rough
    // glass / deep water) and the depth behind for water and soft particles.
    struct Copy {
        rg::TextureHandle color;
        rg::TextureHandle depth;
    };
    const Copy& copy = graph->add_pass<Copy>(
        "Copy opaque scene",
        [&](rg::PassBuilder& b, Copy& d) {
            d.color = b.create("OpaqueColor", gfx::tex2d(GL_RGBA16F, f.render_width, f.render_height, gfx::mip_count(f.render_width, f.render_height)));
            d.depth = b.create("OpaqueDepth", gfx::tex2d(GL_DEPTH_COMPONENT32F, f.render_width, f.render_height));
            b.write(d.color, rg::Access::Transfer);
            b.write(d.depth, rg::Access::Transfer);
            b.read(color, rg::Access::Transfer);
            b.read(depth, rg::Access::Transfer);
        },
        [color, depth](const Copy& d, const rg::PassContext& ctx) {
            const gfx::Texture& src = ctx.texture(color);
            glCopyImageSubData(src.id(), GL_TEXTURE_2D, 0, 0, 0, 0, ctx.tex(d.color), GL_TEXTURE_2D, 0, 0, 0, 0, src.width(), src.height(), 1);
            glGenerateTextureMipmap(ctx.tex(d.color));
            glCopyImageSubData(ctx.tex(depth), GL_TEXTURE_2D, 0, 0, 0, 0, ctx.tex(d.depth), GL_TEXTURE_2D, 0, 0, 0, 0, src.width(), src.height(), 1);
        });
    const rg::TextureHandle opaque_color = copy.color;
    const rg::TextureHandle opaque_depth = copy.depth;

    // Refractive surfaces (water, glass): opaque output, depth written.
    if (f.has_refractive) {
        graph->add_pass(
            "Refractive",
            [&](rg::PassBuilder& b) {
                b.write(color);
                b.write(depth, rg::Access::Depth);
                b.read(opaque_color);
                b.read(opaque_depth);
                b.read(commands, rg::Access::Indirect);
                b.read(cluster_grid_handle, rg::Access::Storage);
                b.read(cluster_items_handle, rg::Access::Storage);
                read_optional(b, fog);
            },
            [this, color, depth, opaque_color, opaque_depth, fog, tex_or_zero](const rg::PassContext& ctx) {
                ctx.bind_targets({{color}}, {depth});
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_GREATER);
                glDepthMask(GL_TRUE);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                bind_scene_buffers();
                bind_global_textures(ctx.tex(opaque_color), ctx.tex(opaque_depth), 0, 0, 0, tex_or_zero(ctx, fog));
                const ProgramId glass = mesh_program("PASS_TRANSPARENT", false);
                const ProgramId water = shaders.graphics("water.vert", "water.frag");
                draw_batches(frame.main_union_offset, {BatchCategory::Refractive}, [glass, water](const Batch& b) { return b.water ? water : glass; }, true);
                glDisable(GL_BLEND);
            });
    }

    // Blended transparency.
    if (has_transparent) {
        if (f.settings->transparency.mode == TransparencyMode::Sorted) {
            graph->add_pass(
                "Transparent (sorted)",
                [&](rg::PassBuilder& b) {
                    b.write(color);
                    b.read(depth, rg::Access::Depth);
                    b.read(opaque_color);
                    b.read(opaque_depth);
                    b.read(commands, rg::Access::Indirect);
                    b.read(cluster_grid_handle, rg::Access::Storage);
                    b.read(cluster_items_handle, rg::Access::Storage);
                    read_optional(b, fog);
                },
                [this, color, depth, opaque_color, opaque_depth, fog, tex_or_zero](const rg::PassContext& ctx) {
                    ctx.bind_targets({{color}}, {depth});
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_GREATER);
                    glDepthMask(GL_FALSE);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    bind_scene_buffers();
                    bind_global_textures(ctx.tex(opaque_color), ctx.tex(opaque_depth), 0, 0, 0, tex_or_zero(ctx, fog));
                    const ProgramId program = mesh_program("PASS_TRANSPARENT", false);
                    draw_batches(frame.main_union_offset, {BatchCategory::Transparent}, [program](const Batch&) { return program; }, true);
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                });
        } else {
            struct Oit {
                rg::TextureHandle accum;
                rg::TextureHandle reveal;
            };
            const Oit& oit = graph->add_pass<Oit>(
                "Transparent (OIT)",
                [&](rg::PassBuilder& b, Oit& d) {
                    d.accum = b.create("OitAccum", gfx::tex2d(GL_RGBA16F, f.render_width, f.render_height));
                    d.reveal = b.create("OitReveal", gfx::tex2d(GL_R16F, f.render_width, f.render_height));
                    b.write(d.accum);
                    b.write(d.reveal);
                    b.read(depth, rg::Access::Depth);
                    b.read(opaque_color);
                    b.read(opaque_depth);
                    b.read(commands, rg::Access::Indirect);
                    b.read(cluster_grid_handle, rg::Access::Storage);
                    b.read(cluster_items_handle, rg::Access::Storage);
                    read_optional(b, fog);
                },
                [this, depth, opaque_color, opaque_depth, fog, tex_or_zero](const Oit& d, const rg::PassContext& ctx) {
                    ctx.bind_targets({{d.accum}, {d.reveal}}, {depth});
                    const float zero[4] = {0, 0, 0, 0};
                    const float one[4] = {1, 1, 1, 1};
                    glClearBufferfv(GL_COLOR, 0, zero);
                    glClearBufferfv(GL_COLOR, 1, one);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_GREATER);
                    glDepthMask(GL_FALSE);
                    glEnable(GL_BLEND);
                    glBlendFunci(0, GL_ONE, GL_ONE);
                    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
                    bind_scene_buffers();
                    bind_global_textures(ctx.tex(opaque_color), ctx.tex(opaque_depth), 0, 0, 0, tex_or_zero(ctx, fog));
                    const ProgramId program = mesh_program("PASS_TRANSPARENT", false, {"WBOIT"});
                    draw_batches(frame.main_union_offset, {BatchCategory::Transparent}, [program](const Batch&) { return program; }, true);
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                });
            const rg::TextureHandle accum = oit.accum;
            const rg::TextureHandle reveal = oit.reveal;
            graph->add_pass(
                "OIT composite",
                [&](rg::PassBuilder& b) {
                    b.write(color);
                    b.read(accum);
                    b.read(reveal);
                },
                [this, color, accum, reveal](const rg::PassContext& ctx) {
                    ctx.bind_targets({{color}});
                    glDisable(GL_DEPTH_TEST);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    shaders.use(shaders.graphics("fullscreen.vert", "oit_composite.frag"));
                    gfx::bind_texture(0, ctx.tex(accum), gfx::SamplerKind::NearestClamp);
                    gfx::bind_texture(1, ctx.tex(reveal), gfx::SamplerKind::NearestClamp);
                    gfx::draw_fullscreen_triangle();
                    glDisable(GL_BLEND);
                    glEnable(GL_DEPTH_TEST);
                });
        }
    }

    // GPU particles: simulate, then draw as soft billboards.
    if (f.has_particles) {
        graph->add_pass(
            "Particles",
            [&](rg::PassBuilder& b) {
                b.write(color);
                b.read(depth, rg::Access::Depth);
                b.read(opaque_depth);
                read_optional(b, fog);
                b.side_effect(); // simulation state persists
            },
            [this, color, depth, opaque_depth, fog, tex_or_zero](const rg::PassContext& ctx) {
                bind_global_textures(0, ctx.tex(opaque_depth), 0, 0, 0, tex_or_zero(ctx, fog));
                const ProgramId simulate = shaders.compute("particles.comp");
                const ProgramId draw = shaders.graphics("particle.vert", "particle.frag");
                if (simulate == kNoProgram || draw == kNoProgram) {
                    return;
                }
                shaders.use(simulate);
                for (const ParticleDraw& p : particle_draws) {
                    const ParticleEmitterComponent& e = *p.emitter;
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, p.state->particles.id());
                    glUniform1ui(shaders.uniform(simulate, "u_capacity"), static_cast<GLuint>(p.state->capacity));
                    glUniform1ui(shaders.uniform(simulate, "u_emit_start"), p.emit_start);
                    glUniform1ui(shaders.uniform(simulate, "u_emit_count"), p.emit_count);
                    glUniform1ui(shaders.uniform(simulate, "u_seed"), p.seed);
                    set_mat4(shaders.uniform(simulate, "u_spawn"), p.spawn);
                    glUniform1i(shaders.uniform(simulate, "u_shape"), static_cast<int>(e.shape));
                    glUniform3f(shaders.uniform(simulate, "u_shape_size"), e.shape_size.x, e.shape_size.y, e.shape_size.z);
                    glUniform1f(shaders.uniform(simulate, "u_cone_angle"), e.cone_angle);
                    glUniform2f(shaders.uniform(simulate, "u_speed"), e.speed, e.speed_variance);
                    glUniform2f(shaders.uniform(simulate, "u_lifetime"), e.lifetime, e.lifetime_variance);
                    glUniform1f(shaders.uniform(simulate, "u_gravity"), e.gravity);
                    glUniform1f(shaders.uniform(simulate, "u_drag"), e.drag);
                    glUniform1f(shaders.uniform(simulate, "u_noise"), e.noise);
                    glDispatchCompute(gfx::groups(p.state->capacity, 64), 1, 1);
                }
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

                ctx.bind_targets({{color}}, {depth});
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_GREATER);
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
                glEnable(GL_BLEND);
                // Premultiplied: additive particles output alpha 0.
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                shaders.use(draw);
                glBindVertexArray(empty_vao);
                for (const ParticleDraw& p : particle_draws) {
                    const ParticleEmitterComponent& e = *p.emitter;
                    GLuint sprite = 0;
                    if (!e.texture_path.empty()) {
                        sprite = textures.get(e.texture_path, texcodec::Usage::Color);
                        if (sprite == textures.default_texture(texcodec::Usage::Color)) {
                            sprite = 0;
                        }
                    }
                    if (sprite != 0) {
                        gfx::bind_texture(gpu::binding::kBaseColor, sprite, gfx::SamplerKind::LinearMipClamp);
                    }
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu::binding::kScratch0, p.state->particles.id());
                    set_mat4(shaders.uniform(draw, "u_model"), p.model);
                    glUniform2f(shaders.uniform(draw, "u_size"), e.size_start, e.size_end);
                    glUniform3f(shaders.uniform(draw, "u_color_start"), e.color_start.x, e.color_start.y, e.color_start.z);
                    glUniform3f(shaders.uniform(draw, "u_color_end"), e.color_end.x, e.color_end.y, e.color_end.z);
                    glUniform2f(shaders.uniform(draw, "u_alpha"), e.alpha_start, e.alpha_end);
                    glUniform1i(shaders.uniform(draw, "u_has_sprite"), sprite != 0 ? 1 : 0);
                    glUniform1i(shaders.uniform(draw, "u_additive"), e.blend == ParticleBlend::Additive ? 1 : 0);
                    glUniform1f(shaders.uniform(draw, "u_emissive"), e.emissive);
                    glUniform1f(shaders.uniform(draw, "u_softness"), 0.3f);
                    glDrawArrays(GL_TRIANGLES, 0, p.state->capacity * 6);
                    ++stats.draw_calls;
                }
                glBindVertexArray(0);
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
                glEnable(GL_CULL_FACE);
                bind_scene_buffers();
            });
    }
}

} // namespace Hollow

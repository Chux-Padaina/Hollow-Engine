// Shadows: four texel-snapped cascades for the sun (2D array, depth clamp
// so casters behind the light's near plane still cast) and a buddy-
// allocated atlas for spot lights (one tile) and point lights (six tiles),
// with tile sizes chosen by each light's importance on screen.
#include "Render/RendererImpl.hpp"

#include <algorithm>
#include <cmath>

namespace Hollow {

namespace {

// Buddy allocator over a square atlas: power-of-two square tiles.
class AtlasAllocator {
public:
    explicit AtlasAllocator(int size) : size_(size) { free_.push_back({0, 0, size}); }

    bool allocate(int tile, int& x, int& y) {
        // Smallest free square that fits, split down to the tile size.
        int best = -1;
        for (std::size_t i = 0; i < free_.size(); ++i) {
            if (free_[i].size >= tile && (best < 0 || free_[i].size < free_[static_cast<std::size_t>(best)].size)) {
                best = static_cast<int>(i);
            }
        }
        if (best < 0) {
            return false;
        }
        Square square = free_[static_cast<std::size_t>(best)];
        free_.erase(free_.begin() + best);
        while (square.size > tile) {
            const int half = square.size / 2;
            free_.push_back({square.x + half, square.y, half});
            free_.push_back({square.x, square.y + half, half});
            free_.push_back({square.x + half, square.y + half, half});
            square.size = half;
        }
        x = square.x;
        y = square.y;
        return true;
    }

private:
    struct Square {
        int x;
        int y;
        int size;
    };
    int size_;
    std::vector<Square> free_;
};

} // namespace

void Renderer::Impl::setup_cascades() {
    FrameState& f = frame;
    const ShadowSettings& s = f.settings->shadows;
    const int resolution = std::clamp(s.cascade_resolution, 256, 4096);
    cascades.ensure(gfx::tex2d_array(GL_DEPTH_COMPONENT32F, resolution, resolution, kCascadeCount), "ShadowCascades");
    f.cascades = s.enabled && f.sun_light_index >= 0;
    if (!f.cascades) {
        return;
    }
    const Camera& camera = *f.camera;
    const float aspect = static_cast<float>(f.render_width) / static_cast<float>(f.render_height);
    const float near_split = std::max(camera.near_plane, 0.05f);
    const float distance = std::max(s.distance, near_split * 2.0f);
    const float lambda = std::clamp(s.split_lambda, 0.0f, 1.0f);
    const Vec3 cam_pos = f.camera_position;
    const Vec3 cam_fwd = f.camera_forward;
    const Vec3 cam_right = camera.right();
    const Vec3 cam_up = camera.up();
    const Vec3 to_light = f.sun_direction;
    float previous = near_split;
    float previous_start = near_split;
    for (int i = 0; i < kCascadeCount; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(kCascadeCount);
        const float log_split = near_split * std::pow(distance / near_split, p);
        const float uniform_split = near_split + (distance - near_split) * p;
        const float split = lambda * log_split + (1.0f - lambda) * uniform_split;
        // The previous cascade blends into this one over the last part of
        // its range, so this one starts that much earlier.
        const float start = std::max(near_split, previous - (previous - previous_start) * s.cascade_blend);
        const float far_edge = split;

        // Bounding sphere of the frustum slice (rotation-stable size).
        Vec3 corners[8];
        int corner = 0;
        for (float depth : {start, far_edge}) {
            const float half_height = depth * std::tan(camera.fov_y_radians * 0.5f);
            const float half_width = half_height * aspect;
            const Vec3 center = cam_pos + cam_fwd * depth;
            for (float sy : {1.0f, -1.0f}) {
                for (float sx : {-1.0f, 1.0f}) {
                    corners[corner++] = center + cam_up * (half_height * sy) + cam_right * (half_width * sx);
                }
            }
        }
        Vec3 slice_center{};
        for (const Vec3& c : corners) {
            slice_center += c;
        }
        slice_center *= 1.0f / 8.0f;
        float radius = 0.0f;
        for (const Vec3& c : corners) {
            radius = std::max(radius, length(c - slice_center));
        }
        radius = std::max(std::ceil(radius * 16.0f) / 16.0f, 0.5f);

        // Snap to whole texels in light space (no shimmering).
        const float reach = std::max(radius, 30.0f);
        Mat4 light_view = Mat4::look_at(slice_center + to_light * (radius + reach), slice_center, {0.0f, 1.0f, 0.0f});
        const float texel = 2.0f * radius / static_cast<float>(resolution);
        const Vec3 light_space = transform_point(light_view, slice_center);
        const float snapped_x = std::floor(light_space.x / texel) * texel;
        const float snapped_y = std::floor(light_space.y / texel) * texel;
        const Vec3 light_right{light_view.m[0], light_view.m[4], light_view.m[8]};
        const Vec3 light_up{light_view.m[1], light_view.m[5], light_view.m[9]};
        const Vec3 snapped = slice_center + light_right * (snapped_x - light_space.x) + light_up * (snapped_y - light_space.y);
        light_view = Mat4::look_at(snapped + to_light * (radius + reach), snapped, {0.0f, 1.0f, 0.0f});

        const float near_z = 0.1f;
        const float far_z = 2.0f * radius + reach + 10.0f;
        ShadowViewInfo view;
        view.view_projection = Mat4::orthographic_reversed_z(-radius, radius, -radius, radius, near_z, far_z) * light_view;
        // Culling ignores the near plane: depth clamp keeps casters behind it.
        view.frustum = Frustum::from_matrix(Mat4::orthographic(-radius, radius, -radius, radius, -1000.0f, far_z) * light_view);
        view.size = resolution;
        view.cascade = true;
        view.cascade_index = i;
        f.shadow_views.push_back(view);
        shadow_uniforms.cascade_view_projection[static_cast<std::size_t>(i)] = view.view_projection;
        shadow_uniforms.cascade_scales[static_cast<std::size_t>(i)] = {2.0f * radius, far_z - near_z, texel, 0.0f};
        (&shadow_uniforms.cascade_splits.x)[i] = split;
        previous_start = previous;
        previous = split;
    }
}

void Renderer::Impl::allocate_atlas() {
    FrameState& f = frame;
    const ShadowSettings& s = f.settings->shadows;
    const int atlas_size = std::clamp(s.atlas_resolution, 512, 8192);
    atlas.ensure(gfx::tex2d(GL_DEPTH_COMPONENT32F, atlas_size, atlas_size), "ShadowAtlas");
    shadow_view_data.clear();
    if (!s.enabled) {
        for (gpu::Light& light : lights) {
            light.spot_shadow.z = -1.0f;
        }
        return;
    }
    struct Candidate {
        std::size_t light;
        float importance;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = static_cast<std::size_t>(f.directional_count); i < lights.size(); ++i) {
        gpu::Light& light = lights[i];
        if (light.spot_shadow.z != -2.0f) {
            light.spot_shadow.z = -1.0f;
            continue;
        }
        light.spot_shadow.z = -1.0f;
        const Vec3 position{light.position_range.x, light.position_range.y, light.position_range.z};
        const float range = light.position_range.w;
        if (!f.frustum.intersects_sphere(position, range)) {
            continue;
        }
        const float distance = std::max(length(position - f.camera_position) - range, 0.0f);
        candidates.push_back({i, range / (distance + 1.0f)});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.importance > b.importance; });
    if (static_cast<int>(candidates.size()) > s.max_shadowed_lights) {
        candidates.resize(static_cast<std::size_t>(std::max(s.max_shadowed_lights, 0)));
    }
    AtlasAllocator allocator(atlas_size);
    const float inv = 1.0f / static_cast<float>(atlas_size);
    int shadowed = 0;
    for (std::size_t rank = 0; rank < candidates.size(); ++rank) {
        gpu::Light& light = lights[candidates[rank].light];
        const bool point = light.direction_type.w < 1.5f;
        const int tier = rank < 4 ? 0 : rank < 10 ? 1 : 2;
        int tile = (point ? 512 : 1024) >> tier;
        tile = std::max(64, tile * atlas_size / 4096);
        const int faces = point ? 6 : 1;
        std::vector<std::pair<int, int>> spots;
        bool ok = true;
        for (int face = 0; face < faces; ++face) {
            int x = 0;
            int y = 0;
            if (!allocator.allocate(tile, x, y)) {
                ok = false;
                break;
            }
            spots.emplace_back(x, y);
        }
        if (!ok) {
            break; // atlas full; remaining lights stay unshadowed
        }
        const Vec3 position{light.position_range.x, light.position_range.y, light.position_range.z};
        const float range = std::max(light.position_range.w, 0.1f);
        const float near_z = std::clamp(range * 0.005f, 0.02f, 0.2f);
        light.spot_shadow.z = static_cast<float>(shadow_view_data.size());
        for (int face = 0; face < faces; ++face) {
            Mat4 view;
            float fov;
            if (point) {
                static const Vec3 directions[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                static const Vec3 ups[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
                view = Mat4::look_at(position, position + directions[face], ups[face]);
                fov = 92.0f * kDegToRad;
            } else {
                const Vec3 dir{light.direction_type.x, light.direction_type.y, light.direction_type.z};
                view = Mat4::look_at(position, position + dir, std::abs(dir.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
                // Cone: cos(outer) = -offset / scale.
                const float cos_outer = -light.spot_shadow.y / std::max(light.spot_shadow.x, 1e-4f);
                fov = std::clamp(2.0f * std::acos(std::clamp(cos_outer, -1.0f, 1.0f)) + 0.1f, 0.2f, 3.0f);
            }
            ShadowViewInfo info;
            info.view_projection = Mat4::perspective_reversed_z(fov, 1.0f, near_z, range) * view;
            info.frustum = Frustum::from_matrix(Mat4::perspective(fov, 1.0f, near_z, range) * view);
            info.atlas_x = spots[static_cast<std::size_t>(face)].first;
            info.atlas_y = spots[static_cast<std::size_t>(face)].second;
            info.size = tile;
            f.shadow_views.push_back(info);
            gpu::ShadowView g;
            g.view_projection = info.view_projection;
            g.atlas_rect = {static_cast<float>(info.atlas_x) * inv, static_cast<float>(info.atlas_y) * inv, static_cast<float>(tile) * inv, static_cast<float>(tile) * inv};
            g.params = {near_z, range, std::tan(fov * 0.5f), 0.0f};
            shadow_view_data.push_back(g);
        }
        ++shadowed;
    }
    stats.shadowed_lights = shadowed;
}

void Renderer::Impl::setup_shadows() {
    FrameState& f = frame;
    f.shadow_views.clear();
    shadow_uniforms = {};
    setup_cascades();
    allocate_atlas();
    const ShadowSettings& s = f.settings->shadows;
    shadow_uniforms.shadow_params = {s.depth_bias, s.normal_bias, s.softness, s.cascade_blend};
    const float atlas_size = static_cast<float>(atlas.width());
    shadow_uniforms.atlas_params = {atlas_size, 1.0f / atlas_size, s.contact_length, 0.0f};
    stats.shadow_views = static_cast<int>(f.shadow_views.size());
}

void Renderer::Impl::add_shadow_passes(rg::BufferHandle commands) {
    FrameState& f = frame;
    if (f.shadow_views.empty() || f.instance_count == 0) {
        return;
    }
    const rg::TextureHandle cascade_handle = graph->import("ShadowCascades", cascades);
    const rg::TextureHandle atlas_handle = graph->import("ShadowAtlas", atlas);

    if (f.gpu_driven) {
        for (const ShadowViewInfo& view : f.shadow_views) {
            add_cull_pass(view.cascade ? "Cull: cascade" : "Cull: shadow tile", view.command_offset, 0, gpu::cull_flag::kShadowCasters, view.view_projection,
                view.frustum, f.camera_position, 1.0f / std::tan(f.fov_y * 0.5f), {}, commands, {}, {});
        }
    }

    auto draw_view = [this](const ShadowViewInfo& view) {
        const ProgramId opaque = mesh_program("PASS_SHADOW", false);
        const ProgramId masked = mesh_program("PASS_SHADOW", true);
        for (const ProgramId program : {opaque, masked}) {
            shaders.use(program);
            glUniformMatrix4fv(shaders.uniform(program, "u_pass_view_projection"), 1, GL_FALSE, view.view_projection.m.data());
        }
        draw_batches(view.command_offset, {BatchCategory::Opaque, BatchCategory::Masked},
            [opaque, masked](const Batch& batch) { return batch.category == BatchCategory::Masked ? masked : opaque; }, true);
    };

    if (f.cascades) {
        graph->add_pass(
            "Shadow cascades",
            [&](rg::PassBuilder& b) {
                b.write(cascade_handle, rg::Access::Depth);
                b.read(commands, rg::Access::Indirect);
            },
            [this, cascade_handle, draw_view](const rg::PassContext& ctx) {
                glEnable(GL_DEPTH_CLAMP);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.25f * frame.settings->shadows.depth_bias, -2.0f);
                glDepthMask(GL_TRUE);
                glDepthFunc(GL_GREATER);
                bind_scene_buffers();
                for (const ShadowViewInfo& view : frame.shadow_views) {
                    if (!view.cascade) {
                        continue;
                    }
                    ctx.bind_targets({}, {cascade_handle, 0, view.cascade_index});
                    glClearDepth(0.0);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    draw_view(view);
                }
                glDisable(GL_DEPTH_CLAMP);
                glDisable(GL_POLYGON_OFFSET_FILL);
            });
    }
    const bool any_tiles = std::any_of(f.shadow_views.begin(), f.shadow_views.end(), [](const ShadowViewInfo& v) { return !v.cascade; });
    if (any_tiles) {
        graph->add_pass(
            "Shadow atlas",
            [&](rg::PassBuilder& b) {
                b.write(atlas_handle, rg::Access::Depth);
                b.read(commands, rg::Access::Indirect);
            },
            [this, atlas_handle, draw_view](const rg::PassContext& ctx) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.5f * frame.settings->shadows.depth_bias, -3.0f);
                glDepthMask(GL_TRUE);
                glDepthFunc(GL_GREATER);
                glEnable(GL_SCISSOR_TEST);
                bind_scene_buffers();
                ctx.bind_targets({}, {atlas_handle});
                for (const ShadowViewInfo& view : frame.shadow_views) {
                    if (view.cascade) {
                        continue;
                    }
                    glViewport(view.atlas_x, view.atlas_y, view.size, view.size);
                    glScissor(view.atlas_x, view.atlas_y, view.size, view.size);
                    glClearDepth(0.0);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    draw_view(view);
                }
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_POLYGON_OFFSET_FILL);
            });
    }
}

} // namespace Hollow

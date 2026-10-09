// Editor overlays, drawn on the final image at output resolution without
// TAA jitter: the infinite grid, the lime selection outline (from a mask
// of the selected actor), and line/solid geometry for gizmos, light and
// camera icons, colliders, probe and volume bounds. Hidden parts fade
// instead of vanishing (depth compared in the shader).
#include "Render/RendererImpl.hpp"

#include "Hollow/Physics/PhysicsWorld.hpp"

#include <algorithm>
#include <cmath>

namespace Hollow {

namespace {

struct GpuOverlayVertex {
    Vec3 position;
    Vec3 other;
    Vec4 color;
    Vec4 params; // side, endpoint, width, flags
};

Vec4 rgba(Vec3 c, float a = 1.0f) {
    return {c.x, c.y, c.z, a};
}

constexpr Vec3 kLime{0.831f, 1.0f, 0.247f};
constexpr Vec3 kAxisX{0.93f, 0.33f, 0.36f};
constexpr Vec3 kAxisY{0.55f, 0.85f, 0.25f};
constexpr Vec3 kAxisZ{0.33f, 0.58f, 0.98f};
constexpr Vec3 kHover{1.0f, 0.95f, 0.4f};

// Shading for solid gizmo parts (fake light so they read as 3D).
Vec3 shade(Vec3 color, Vec3 normal) {
    const Vec3 light = normalize(Vec3{0.35f, 0.9f, 0.28f});
    const float ndotl = std::max(0.0f, dot(normalize(normal), light));
    return color * (0.4f + 0.6f * ndotl);
}

struct OverlayBuilder {
    std::vector<GpuOverlayVertex> lines;
    std::vector<GpuOverlayVertex> solids;
    Vec3 camera;
    Vec3 forward;
    float near_plane = 0.05f;

    void line(Vec3 a, Vec3 b, Vec3 color, float width = 1.5f, float alpha = 1.0f, bool depth_fade = true) {
        // Clip against the near plane.
        const float da = dot(a - camera, forward) - near_plane * 1.5f;
        const float db = dot(b - camera, forward) - near_plane * 1.5f;
        if (da < 0.0f && db < 0.0f) {
            return;
        }
        if (da < 0.0f) {
            a = a + (b - a) * (da / (da - db));
        } else if (db < 0.0f) {
            b = b + (a - b) * (db / (db - da));
        }
        const Vec4 c = rgba(color, alpha);
        const float flags = depth_fade ? 1.0f : 0.0f;
        const GpuOverlayVertex v0{a, b, c, {-1.0f, 0.0f, width, flags}};
        const GpuOverlayVertex v1{a, b, c, {1.0f, 0.0f, width, flags}};
        const GpuOverlayVertex v2{a, b, c, {-1.0f, 1.0f, width, flags}};
        const GpuOverlayVertex v3{a, b, c, {1.0f, 1.0f, width, flags}};
        lines.insert(lines.end(), {v0, v1, v2, v2, v1, v3});
    }

    void circle(Vec3 center, Vec3 axis_a, Vec3 axis_b, float radius, Vec3 color, float width = 1.5f, int segments = 64, float alpha = 1.0f, bool depth_fade = true) {
        Vec3 previous = center + axis_a * radius;
        for (int i = 1; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
            const Vec3 p = center + axis_a * (std::cos(t) * radius) + axis_b * (std::sin(t) * radius);
            line(previous, p, color, width, alpha, depth_fade);
            previous = p;
        }
    }

    void box(const Aabb& b, Vec3 color, float width = 1.5f, float alpha = 1.0f) {
        const Vec3 lo = b.min;
        const Vec3 hi = b.max;
        const Vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
            {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : e) {
            line(c[edge[0]], c[edge[1]], color, width, alpha);
        }
    }

    void oriented_box(const Mat4& world, Vec3 color, float width = 1.5f, float alpha = 1.0f) {
        Vec3 c[8];
        int i = 0;
        for (float z : {-0.5f, 0.5f}) {
            for (float y : {-0.5f, 0.5f}) {
                for (float x : {-0.5f, 0.5f}) {
                    c[i++] = transform_point(world, {x, y, z});
                }
            }
        }
        const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : e) {
            line(c[edge[0]], c[edge[1]], color, width, alpha);
        }
    }

    void triangle(Vec3 a, Vec3 b, Vec3 c, Vec3 color, bool depth_fade = false) {
        const Vec3 n = cross(b - a, c - a);
        const Vec4 col = rgba(length(n) > 1e-8f ? shade(color, n) : color);
        const Vec4 params{0.0f, 0.0f, 0.0f, depth_fade ? 1.0f : 0.0f};
        solids.push_back({a, a, col, params});
        solids.push_back({b, b, col, params});
        solids.push_back({c, c, col, params});
    }

    void cube(Vec3 center, float half, Vec3 color, bool depth_fade = false) {
        const Vec3 normals[6] = {{0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (const Vec3& n : normals) {
            const Vec3 u = std::abs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            const Vec3 v = cross(n, u);
            const Vec3 c = center + n * half;
            const Vec3 p0 = c - u * half - v * half;
            const Vec3 p1 = c + u * half - v * half;
            const Vec3 p2 = c + u * half + v * half;
            const Vec3 p3 = c - u * half + v * half;
            triangle(p0, p1, p2, color, depth_fade);
            triangle(p0, p2, p3, color, depth_fade);
        }
    }

    void sphere(Vec3 center, float radius, Vec3 color, bool depth_fade = false) {
        constexpr int rings = 8;
        constexpr int segments = 14;
        auto point = [](float phi, float theta) { return Vec3{std::cos(phi) * std::cos(theta), std::sin(phi), std::cos(phi) * std::sin(theta)}; };
        for (int y = 0; y < rings; ++y) {
            const float phi0 = (static_cast<float>(y) / rings - 0.5f) * kPi;
            const float phi1 = (static_cast<float>(y + 1) / rings - 0.5f) * kPi;
            for (int x = 0; x < segments; ++x) {
                const float t0 = static_cast<float>(x) / segments * 2.0f * kPi;
                const float t1 = static_cast<float>(x + 1) / segments * 2.0f * kPi;
                const Vec3 p00 = center + point(phi0, t0) * radius;
                const Vec3 p10 = center + point(phi0, t1) * radius;
                const Vec3 p01 = center + point(phi1, t0) * radius;
                const Vec3 p11 = center + point(phi1, t1) * radius;
                triangle(p00, p11, p10, color, depth_fade);
                triangle(p00, p01, p11, color, depth_fade);
            }
        }
    }

    void cone(Vec3 base, Vec3 axis, float radius, float height, Vec3 color) {
        const Vec3 apex = base + axis * height;
        const Vec3 helper = std::abs(axis.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 t = normalize(cross(helper, axis));
        const Vec3 b = normalize(cross(axis, t));
        constexpr int segments = 16;
        for (int i = 0; i < segments; ++i) {
            const float a0 = static_cast<float>(i) / segments * 2.0f * kPi;
            const float a1 = static_cast<float>(i + 1) / segments * 2.0f * kPi;
            const Vec3 r0 = base + (t * std::cos(a0) + b * std::sin(a0)) * radius;
            const Vec3 r1 = base + (t * std::cos(a1) + b * std::sin(a1)) * radius;
            triangle(apex, r1, r0, color);
            triangle(base, r0, r1, color);
        }
    }
};

std::vector<GpuOverlayVertex> g_overlay_lines;
std::vector<GpuOverlayVertex> g_overlay_solids;

} // namespace

void Renderer::Impl::init_overlay() {
    glCreateVertexArrays(1, &overlay_vao);
    const GLuint vao = overlay_vao;
    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offsetof(GpuOverlayVertex, position)));
    glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offsetof(GpuOverlayVertex, other)));
    glVertexArrayAttribBinding(vao, 1, 0);
    glEnableVertexArrayAttrib(vao, 2);
    glVertexArrayAttribFormat(vao, 2, 4, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offsetof(GpuOverlayVertex, color)));
    glVertexArrayAttribBinding(vao, 2, 0);
    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribFormat(vao, 3, 4, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offsetof(GpuOverlayVertex, params)));
    glVertexArrayAttribBinding(vao, 3, 0);
    overlay_vertices = gfx::Buffer(64 * 1024, GL_DYNAMIC_STORAGE_BIT, nullptr, "OverlayVertices");
}

void Renderer::Impl::destroy_overlay() {
    if (overlay_vao != 0) {
        glDeleteVertexArrays(1, &overlay_vao);
        overlay_vao = 0;
    }
    overlay_vertices.reset();
}

void Renderer::Impl::collect_overlay() {
    const FrameState& f = frame;
    const Scene& scene = *f.scene;
    const RenderView& view = *f.view;
    const Camera& camera = *f.camera;
    OverlayBuilder o;
    o.camera = f.camera_position;
    o.forward = f.camera_forward;
    o.near_plane = f.near_plane;

    for (const auto& actor_ptr : scene.actors()) {
        const Actor& actor = *actor_ptr;
        if (!actor.active) {
            continue;
        }
        const bool selected = actor.id() == view.selected;
        const Mat4& world = actor.world_matrix();
        const Vec3 position = world.translation_part();
        const float icon = 0.09f * camera.gizmo_scale(position) * 1.5f;

        if (view.draw_icons) {
            if (const auto* light = actor.find_component<LightComponent>()) {
                const Vec3 color = selected ? kLime : (light->enabled ? normalize(light->color + Vec3{0.2f, 0.2f, 0.2f}) : Vec3{0.3f, 0.3f, 0.3f});
                const Vec3 direction = normalize(transform_vector(world, {0.0f, -1.0f, 0.0f}));
                if (light->light_kind == LightKind::Point) {
                    o.sphere(position, icon, color, true);
                    if (selected) {
                        o.circle(position, {1, 0, 0}, {0, 0, 1}, light->range, kLime, 1.2f, 64, 0.6f);
                        o.circle(position, {1, 0, 0}, {0, 1, 0}, light->range, kLime, 1.2f, 64, 0.35f);
                        o.circle(position, {0, 1, 0}, {0, 0, 1}, light->range, kLime, 1.2f, 64, 0.35f);
                    }
                } else {
                    o.cube(position, icon, color, true);
                    o.line(position, position + direction * (icon * 8.0f), color, 2.5f);
                    if (light->light_kind == LightKind::Spot && selected) {
                        const float half = light->spot_angle_radians * 0.5f;
                        const float r = std::tan(half) * light->range;
                        const Vec3 helper = std::abs(direction.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
                        const Vec3 t = normalize(cross(helper, direction));
                        const Vec3 b = normalize(cross(direction, t));
                        const Vec3 end = position + direction * light->range;
                        o.circle(end, t, b, r, kLime, 1.2f, 48, 0.7f);
                        for (int i = 0; i < 4; ++i) {
                            const float a = static_cast<float>(i) * kPi * 0.5f;
                            o.line(position, end + (t * std::cos(a) + b * std::sin(a)) * r, kLime, 1.2f, 0.7f);
                        }
                    }
                }
            }
            if (const auto* cam = actor.find_component<CameraComponent>()) {
                const Vec3 fwd = normalize(transform_vector(world, {0.0f, 0.0f, -1.0f}));
                const Vec3 right = normalize(transform_vector(world, {1.0f, 0.0f, 0.0f}));
                const Vec3 up = normalize(transform_vector(world, {0.0f, 1.0f, 0.0f}));
                const float s = icon * 3.0f;
                const Vec3 color = selected ? kLime : Vec3{0.85f, 0.85f, 0.9f};
                const float h = std::tan(cam->fov_y_radians * 0.5f) * s * 1.6f;
                const Vec3 far_center = position + fwd * (s * 1.6f);
                const Vec3 corners[4] = {far_center + right * h * 1.6f + up * h, far_center - right * h * 1.6f + up * h, far_center - right * h * 1.6f - up * h,
                    far_center + right * h * 1.6f - up * h};
                for (int i = 0; i < 4; ++i) {
                    o.line(position, corners[i], color, 2.0f);
                    o.line(corners[i], corners[(i + 1) % 4], color, 2.0f);
                }
                o.triangle(corners[0] + up * h * 0.3f, corners[1] + up * h * 0.3f, far_center + up * h * 1.8f, color);
            }
            if (const auto* probe = actor.find_component<ReflectionProbeComponent>()) {
                o.sphere(position, icon * 1.2f, selected ? kLime : Vec3{0.75f, 0.8f, 0.85f}, true);
                if (selected) {
                    o.box({position - probe->extents, position + probe->extents}, kLime, 1.5f, 0.8f);
                }
            }
            if (const auto* volume = actor.find_component<IrradianceVolumeComponent>()) {
                const Vec3 color = selected ? kLime : Vec3{0.6f, 0.7f, 0.9f};
                o.box({position - volume->extents, position + volume->extents}, color, selected ? 1.5f : 1.0f, selected ? 0.8f : 0.3f);
                if (selected) {
                    for (int z = 0; z < volume->probes_z; ++z) {
                        for (int y = 0; y < volume->probes_y; ++y) {
                            for (int x = 0; x < volume->probes_x; ++x) {
                                const Vec3 t{static_cast<float>(x) / static_cast<float>(std::max(volume->probes_x - 1, 1)),
                                    static_cast<float>(y) / static_cast<float>(std::max(volume->probes_y - 1, 1)),
                                    static_cast<float>(z) / static_cast<float>(std::max(volume->probes_z - 1, 1))};
                                const Vec3 p = position - volume->extents + component_mul(volume->extents * 2.0f, t);
                                o.cube(p, icon * 0.25f, kLime, true);
                            }
                        }
                    }
                }
            }
            if (actor.find_component<DecalComponent>() != nullptr) {
                o.oriented_box(world, selected ? kLime : Vec3{0.8f, 0.7f, 0.5f}, selected ? 1.6f : 1.0f, selected ? 0.9f : 0.5f);
            }
            if (actor.find_component<ParticleEmitterComponent>() != nullptr) {
                const Vec3 color = selected ? kLime : Vec3{1.0f, 0.7f, 0.35f};
                o.circle(position, {1, 0, 0}, {0, 0, 1}, icon * 2.0f, color, 1.8f, 24);
                o.line(position, position + normalize(transform_vector(world, {0.0f, 1.0f, 0.0f})) * icon * 5.0f, color, 1.8f);
            }
        }
        auto draw_shape = [&](const PhysicsWorld::WorldShape& shape, Vec3 color, float alpha) {
            const Mat4 rotation = Mat4::rotation(shape.rotation);
            const Vec3 ax = transform_vector(rotation, {1, 0, 0});
            const Vec3 ay = transform_vector(rotation, {0, 1, 0});
            const Vec3 az = transform_vector(rotation, {0, 0, 1});
            switch (shape.shape) {
            case ColliderShape::Sphere:
                o.circle(shape.center, ax, ay, shape.radius, color, 1.4f, 48, alpha);
                o.circle(shape.center, ax, az, shape.radius, color, 1.4f, 48, alpha);
                o.circle(shape.center, ay, az, shape.radius, color, 1.4f, 48, alpha);
                break;
            case ColliderShape::Capsule: {
                const Vec3 top = shape.center + ay * shape.half_height;
                const Vec3 bottom = shape.center - ay * shape.half_height;
                o.circle(top, ax, az, shape.radius, color, 1.4f, 40, alpha);
                o.circle(bottom, ax, az, shape.radius, color, 1.4f, 40, alpha);
                for (const Vec3 side : {ax, -ax, az, -az}) {
                    o.line(top + side * shape.radius, bottom + side * shape.radius, color, 1.4f, alpha);
                }
                // Hemisphere outlines: full circles through the caps' centers, clipped visually by the lines.
                o.circle(top, ax, ay, shape.radius, color, 1.4f, 40, alpha * 0.6f);
                o.circle(top, az, ay, shape.radius, color, 1.4f, 40, alpha * 0.6f);
                o.circle(bottom, ax, ay, shape.radius, color, 1.4f, 40, alpha * 0.6f);
                o.circle(bottom, az, ay, shape.radius, color, 1.4f, 40, alpha * 0.6f);
                break;
            }
            case ColliderShape::Box:
            case ColliderShape::Mesh: {
                Mat4 box = Mat4::translation(shape.center) * rotation * Mat4::scale(shape.half_extents * 2.0f);
                o.oriented_box(box, color, 1.4f, alpha);
                break;
            }
            }
        };
        const bool show_physics = view.draw_colliders || (selected && view.draw_selection_collider);
        if (const auto* collider = actor.find_component<ColliderComponent>(); collider != nullptr && show_physics) {
            const Vec3 color = collider->is_trigger ? Vec3{0.95f, 0.75f, 0.2f} : Vec3{0.3f, 1.0f, 0.55f};
            draw_shape(PhysicsWorld::world_shape(actor, *collider), color, selected ? 0.9f : 0.5f);
        }
        if (const auto* character = actor.find_component<CharacterControllerComponent>(); character != nullptr && show_physics) {
            draw_shape(PhysicsWorld::world_shape(actor, *character), Vec3{0.35f, 0.75f, 1.0f}, selected ? 0.9f : 0.5f);
        }
    }

    // Gizmo for the selection, on top of everything. App's picking uses the
    // same lengths (gizmo_scale * 1.35, or 1.15 for scale).
    if (view.draw_gizmos && view.tool != EditorTool::Select) {
        if (const Actor* actor = scene.find(view.selected)) {
            const EditorTool tool = view.tool;
            const Vec3 origin = actor->world_position();
            const float screen = camera.gizmo_scale(origin);
            const float length_scale = (tool == EditorTool::Scale ? 1.15f : 1.35f) * screen;
            const bool local_axes = view.space == TransformSpace::Local || tool == EditorTool::Scale;
            const Mat4& world = actor->world_matrix();
            auto axis = [&](Vec3 a) { return local_axes ? normalize(transform_vector(world, a)) : a; };
            const Vec3 axes[3] = {axis({1, 0, 0}), axis({0, 1, 0}), axis({0, 0, 1})};
            const Vec3 colors[3] = {kAxisX, kAxisY, kAxisZ};
            const std::uint32_t hovered = view.hovered_gizmo_axis;
            if (tool == EditorTool::Rotate) {
                for (int i = 0; i < 3; ++i) {
                    const Vec3 a = axes[(i + 1) % 3];
                    const Vec3 b = axes[(i + 2) % 3];
                    const bool hot = hovered == static_cast<std::uint32_t>(i + 1);
                    o.circle(origin, a, b, length_scale, hot ? kHover : colors[i], hot ? 4.5f : 3.0f, 96, 1.0f, false);
                }
                o.circle(origin, camera.right(), camera.up(), length_scale * 1.12f, {0.75f, 0.75f, 0.78f}, 1.5f, 96, 0.6f, false);
            } else {
                for (int i = 0; i < 3; ++i) {
                    const bool hot = hovered == static_cast<std::uint32_t>(i + 1);
                    const Vec3 color = hot ? kHover : colors[i];
                    const Vec3 tip = origin + axes[i] * length_scale;
                    if (tool == EditorTool::Move) {
                        const float cone_height = 0.24f * screen;
                        const Vec3 base = tip - axes[i] * cone_height;
                        o.line(origin, base, color, hot ? 5.0f : 3.5f, 1.0f, false);
                        o.cone(base, axes[i], 0.085f * screen, cone_height, color);
                    } else {
                        o.line(origin, tip, color, hot ? 5.0f : 3.5f, 1.0f, false);
                        o.cube(tip, 0.08f * screen, color);
                    }
                }
            }
            o.cube(origin, 0.07f * screen, hovered == 4 ? kHover : Vec3{0.95f, 0.95f, 0.95f});
        }
    }
    g_overlay_lines = std::move(o.lines);
    g_overlay_solids = std::move(o.solids);
}

void Renderer::Impl::draw_overlay(GLuint depth_texture) {
    const std::size_t lines = g_overlay_lines.size();
    const std::size_t solids = g_overlay_solids.size();
    if (lines + solids == 0) {
        return;
    }
    const std::size_t bytes = (lines + solids) * sizeof(GpuOverlayVertex);
    if (bytes > overlay_vertices.size()) {
        overlay_vertices.reserve(bytes, GL_DYNAMIC_STORAGE_BIT, "OverlayVertices");
    }
    overlay_vertices.upload(g_overlay_lines.data(), lines * sizeof(GpuOverlayVertex));
    overlay_vertices.upload(g_overlay_solids.data(), solids * sizeof(GpuOverlayVertex), lines * sizeof(GpuOverlayVertex));
    glVertexArrayVertexBuffer(overlay_vao, 0, overlay_vertices.id(), 0, sizeof(GpuOverlayVertex));
    const ProgramId program = shaders.graphics("overlay.vert", "overlay.frag");
    shaders.use(program);
    gfx::bind_texture(0, depth_texture, gfx::SamplerKind::NearestClamp);
    glBindVertexArray(overlay_vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Solids first (gizmo heads, icons), then lines on top.
    if (solids > 0) {
        glUniform1i(shaders.uniform(program, "u_solid"), 1);
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(lines), static_cast<GLsizei>(solids));
    }
    if (lines > 0) {
        glUniform1i(shaders.uniform(program, "u_solid"), 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(lines));
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

void Renderer::Impl::add_overlay_passes(rg::TextureHandle depth, rg::TextureHandle output_handle) {
    FrameState& f = frame;
    const RenderView& view = *f.view;
    collect_overlay();
    // Selection mask: the selected actor's instances at output resolution.
    rg::TextureHandle mask{};
    const bool any_selected = std::any_of(instances.begin(), instances.end(), [](const gpu::Instance& inst) { return (inst.flags & gpu::instance_flag::kSelected) != 0; });
    if (view.draw_outlines && any_selected) {
        struct Data {
            rg::TextureHandle mask;
        };
        const Data& data = graph->add_pass<Data>(
            "Selection mask",
            [&](rg::PassBuilder& b, Data& d) {
                d.mask = b.create("SelectionMask", gfx::tex2d(GL_R32F, f.output_width, f.output_height));
                b.write(d.mask);
            },
            [this](const Data& d, const rg::PassContext& ctx) {
                ctx.bind_targets({{d.mask}});
                const float zero[4] = {0, 0, 0, 0};
                glClearBufferfv(GL_COLOR, 0, zero);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                bind_scene_buffers();
                glBindVertexArray(pool.vao);
                for (std::uint32_t i = 0; i < instances.size(); ++i) {
                    const gpu::Instance& inst = instances[i];
                    if ((inst.flags & gpu::instance_flag::kSelected) == 0) {
                        continue;
                    }
                    const gpu::MeshInfo& mesh = pool.info_shadow[inst.mesh];
                    const bool masked = (materials[inst.material].flags & gpu::material_flag::kAlphaMask) != 0;
                    shaders.use(mesh_program("PASS_SELECTION", masked));
                    if (masked) {
                        bind_texture_set(batches[inst.batch].texture_set);
                    }
                    glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, static_cast<GLsizei>(mesh.lod_count[0]), GL_UNSIGNED_INT,
                        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(mesh.lod_first[0]) * sizeof(std::uint32_t)), 1, inst.base_vertex, i);
                }
                glBindVertexArray(0);
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_CULL_FACE);
            });
        mask = data.mask;
    }
    graph->add_pass(
        "Editor overlay",
        [&](rg::PassBuilder& b) {
            b.write(output_handle);
            b.read(depth);
            if (mask.valid()) {
                b.read(mask);
            }
        },
        [this, output_handle, depth, mask](const rg::PassContext& ctx) {
            ctx.bind_targets({{output_handle}});
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (frame.view->draw_grid) {
                shaders.use(shaders.graphics("fullscreen.vert", "grid.frag"));
                gfx::bind_texture(0, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
                gfx::draw_fullscreen_triangle();
            }
            if (mask.valid()) {
                const ProgramId outline = shaders.graphics("fullscreen.vert", "outline.frag");
                shaders.use(outline);
                glUniform1f(shaders.uniform(outline, "u_width"), std::max(2.0f, static_cast<float>(frame.output_height) / 500.0f));
                gfx::bind_texture(0, ctx.tex(mask), gfx::SamplerKind::NearestClamp);
                gfx::bind_texture(1, ctx.tex(depth), gfx::SamplerKind::NearestClamp);
                gfx::draw_fullscreen_triangle();
            }
            glDisable(GL_BLEND);
            draw_overlay(ctx.tex(depth));
        });
}

} // namespace Hollow

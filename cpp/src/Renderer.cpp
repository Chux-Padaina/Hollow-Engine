#include "Forge/Renderer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <Windows.h>
#include <gl/GL.h>

namespace Forge {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

void set_color(Vec3 color, float alpha = 1.0f) {
    glColor4f(color.x, color.y, color.z, alpha);
}

void vertex(Vec3 value) {
    glVertex3f(value.x, value.y, value.z);
}

Vec3 shade(const Material& material, Vec3 normal) {
    const Vec3 light_dir = normalize(Vec3{-0.35f, -0.9f, -0.28f});
    const float ndotl = std::max(0.0f, dot(normalize(normal), light_dir * -1.0f));
    const float wrapped = 0.32f + ndotl * 0.68f;
    return material.base_color * wrapped;
}

void draw_quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 color) {
    set_color(color);
    glBegin(GL_QUADS);
    vertex(a);
    vertex(b);
    vertex(c);
    vertex(d);
    glEnd();
}

void draw_line(Vec3 a, Vec3 b, Vec3 color, float width = 1.0f) {
    glLineWidth(width);
    set_color(color);
    glBegin(GL_LINES);
    vertex(a);
    vertex(b);
    glEnd();
    glLineWidth(1.0f);
}

} // namespace

Vec3 Camera::position() const {
    const float cp = std::cos(pitch);
    return {
        target.x + std::sin(yaw) * cp * distance,
        target.y + std::sin(pitch) * distance,
        target.z + std::cos(yaw) * cp * distance,
    };
}

void Renderer::initialize() {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.08f, 0.10f, 0.09f, 1.0f);
}

void Renderer::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void Renderer::render(const Scene& scene, const Camera& camera, const Engine& engine) {
    resize(width_, height_);
    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 projection = Mat4::perspective(camera.fov_y_radians, aspect, 0.05f, 500.0f);
    const Mat4 view = Mat4::look_at(camera.position(), camera.target, {0.0f, 1.0f, 0.0f});

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m.data());
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m.data());

    if (engine.render_settings().draw_world_grid) {
        draw_grid();
    }

    for (const auto& actor : scene.actors()) {
        draw_actor(*actor, actor->id() == scene.selected_actor_id());
    }

    if (const Actor* selected = scene.selected_actor()) {
        if (engine.render_settings().draw_editor_gizmos) {
            glClear(GL_DEPTH_BUFFER_BIT);
            draw_gizmo(*selected, engine.active_tool());
        }
    }
}

void Renderer::draw_grid() const {
    glDisable(GL_CULL_FACE);
    const Vec3 minor{0.13f, 0.29f, 0.27f};
    const Vec3 major{0.9f, 0.85f, 0.1f};
    constexpr int half = 80;
    for (int i = -half; i <= half; ++i) {
        const bool axis = i == 0;
        const float width = axis ? 2.0f : 1.0f;
        draw_line({static_cast<float>(i), 0.0f, -half}, {static_cast<float>(i), 0.0f, half}, axis ? major : minor, width);
        draw_line({-half, 0.0f, static_cast<float>(i)}, {half, 0.0f, static_cast<float>(i)}, axis ? major : minor, width);
    }
    glEnable(GL_CULL_FACE);
}

void Renderer::draw_actor(const Actor& actor, bool selected) const {
    if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
        if (mesh->visible) {
            for (const auto& part : mesh->parts) {
                draw_mesh_part(part, actor.root().local_transform);
            }
        }
    }
    if (const auto* light = actor.find_component<LightComponent>()) {
        draw_light_icon(actor, *light, selected);
    }
    if (selected && actor.find_component<StaticMeshComponent>()) {
        glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(2.0f);
        set_color({0.02f, 0.02f, 0.02f});
        glPushMatrix();
        const Mat4 model = actor.root().local_transform.matrix();
        glMultMatrixf(model.m.data());
        Material outline;
        outline.base_color = {0.02f, 0.02f, 0.02f};
        draw_cube(outline);
        glPopMatrix();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glLineWidth(1.0f);
        glEnable(GL_CULL_FACE);
    }
}

void Renderer::draw_mesh_part(const MeshPart& part, const Transform& transform) const {
    glPushMatrix();
    const Mat4 model = transform.matrix();
    glMultMatrixf(model.m.data());
    switch (part.primitive) {
    case MeshPrimitive::Cube:
        draw_cube(part.material);
        break;
    case MeshPrimitive::Plane:
        draw_plane();
        break;
    case MeshPrimitive::Sphere:
        draw_sphere();
        break;
    }
    glPopMatrix();
}

void Renderer::draw_cube(const Material& material) const {
    draw_quad({-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, shade(material, {0, 0, 1}));
    draw_quad({0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, shade(material, {0, 0, -1}));
    draw_quad({-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}, shade(material, {-1, 0, 0}));
    draw_quad({0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, shade(material, {1, 0, 0}));
    draw_quad({-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, shade(material, {0, 1, 0}));
    draw_quad({-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}, shade(material, {0, -1, 0}));
}

void Renderer::draw_plane() const {
    glDisable(GL_CULL_FACE);
    draw_quad({-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f}, {0.48f, 0.50f, 0.48f});
    glEnable(GL_CULL_FACE);
}

void Renderer::draw_sphere() const {
    constexpr int rings = 16;
    constexpr int segments = 24;
    glBegin(GL_TRIANGLES);
    for (int y = 0; y < rings; ++y) {
        const float v0 = static_cast<float>(y) / rings;
        const float v1 = static_cast<float>(y + 1) / rings;
        const float phi0 = (v0 - 0.5f) * kPi;
        const float phi1 = (v1 - 0.5f) * kPi;
        for (int x = 0; x < segments; ++x) {
            const float u0 = static_cast<float>(x) / segments;
            const float u1 = static_cast<float>(x + 1) / segments;
            const float theta0 = u0 * kPi * 2.0f;
            const float theta1 = u1 * kPi * 2.0f;
            const Vec3 p00{std::cos(phi0) * std::cos(theta0) * 0.5f, std::sin(phi0) * 0.5f, std::cos(phi0) * std::sin(theta0) * 0.5f};
            const Vec3 p10{std::cos(phi0) * std::cos(theta1) * 0.5f, std::sin(phi0) * 0.5f, std::cos(phi0) * std::sin(theta1) * 0.5f};
            const Vec3 p01{std::cos(phi1) * std::cos(theta0) * 0.5f, std::sin(phi1) * 0.5f, std::cos(phi1) * std::sin(theta0) * 0.5f};
            const Vec3 p11{std::cos(phi1) * std::cos(theta1) * 0.5f, std::sin(phi1) * 0.5f, std::cos(phi1) * std::sin(theta1) * 0.5f};
            set_color({0.55f, 0.49f, 0.30f});
            vertex(p00);
            vertex(p10);
            vertex(p11);
            vertex(p00);
            vertex(p11);
            vertex(p01);
        }
    }
    glEnd();
}

void Renderer::draw_light_icon(const Actor& actor, const LightComponent& light, bool selected) const {
    glPushMatrix();
    const Transform& transform = actor.root().local_transform;
    const Mat4 model = Mat4::translation(transform.translation) * Mat4::scale({0.18f, 0.18f, 0.18f});
    glMultMatrixf(model.m.data());
    set_color(selected ? Vec3{1.0f, 0.9f, 0.35f} : light.color);
    if (light.light_kind == LightKind::Directional) {
        Material marker;
        marker.base_color = light.color;
        draw_cube(marker);
        draw_line({0, 0, 0}, {0, -1.5f, 0}, light.color, 3.0f);
    } else {
        draw_sphere();
    }
    glPopMatrix();
}

void Renderer::draw_gizmo(const Actor& actor, EditorTool tool) const {
    const Vec3 origin = actor.root().local_transform.translation;
    const float scale = tool == EditorTool::Scale ? 0.85f : 1.35f;
    draw_line(origin, origin + Vec3{scale, 0.0f, 0.0f}, {0.85f, 0.2f, 0.16f}, 5.0f);
    draw_line(origin, origin + Vec3{0.0f, scale, 0.0f}, {0.25f, 0.68f, 0.31f}, 5.0f);
    draw_line(origin, origin + Vec3{0.0f, 0.0f, scale}, {0.22f, 0.48f, 0.78f}, 5.0f);

    glPushMatrix();
    glTranslatef(origin.x, origin.y, origin.z);
    glScalef(0.14f, 0.14f, 0.14f);
    Material center;
    center.base_color = {0.72f, 0.66f, 0.28f};
    draw_cube(center);
    glPopMatrix();
}

} // namespace Forge

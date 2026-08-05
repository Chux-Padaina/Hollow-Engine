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

Vec3 Camera::forward() const {
    return normalize(target - position());
}

Vec3 Camera::right() const {
    return normalize(cross(forward(), {0.0f, 1.0f, 0.0f}));
}

Vec3 Camera::up() const {
    return normalize(cross(right(), forward()));
}

void Renderer::initialize(HDC device_context) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.08f, 0.10f, 0.09f, 1.0f);

    font_base_ = glGenLists(96);
    HFONT font = CreateFontA(
        -15,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        FF_DONTCARE,
        "Segoe UI");
    HFONT old_font = static_cast<HFONT>(SelectObject(device_context, font));
    wglUseFontBitmapsA(device_context, 32, 96, font_base_);
    SelectObject(device_context, old_font);
    DeleteObject(font);
}

void Renderer::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void Renderer::render_editor_overlay(const Scene& scene, const Camera& camera, const EditorOverlayState& overlay) {
    (void)camera;
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, width_, height_, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    draw_rect(0, 0, static_cast<float>(width_), 42, {0.075f, 0.085f, 0.085f}, 0.96f);
    draw_rect(0, 42, 258, static_cast<float>(height_ - 42), {0.07f, 0.075f, 0.078f}, 0.94f);
    draw_rect(static_cast<float>(width_ - 306), 42, 306, static_cast<float>(height_ - 42), {0.07f, 0.075f, 0.078f}, 0.94f);
    draw_text(14, 25, "Forge Editor");
    draw_button(124, 9, 68, 25, "Q Select", overlay.active_tool == EditorTool::Select);
    draw_button(198, 9, 60, 25, "W Move", overlay.active_tool == EditorTool::Move);
    draw_button(264, 9, 68, 25, "E Rotate", overlay.active_tool == EditorTool::Rotate);
    draw_button(338, 9, 64, 25, "R Scale", overlay.active_tool == EditorTool::Scale);
    draw_button(414, 9, 88, 25, overlay.transform_space == TransformSpace::World ? "World" : "Local", overlay.transform_space == TransformSpace::Local);
    draw_button(510, 9, 92, 25, "Snap Grid", overlay.snap_to_grid);

    draw_text(16, 70, "Outliner");
    float row_y = 96.0f;
    for (const auto& actor : scene.actors()) {
        const bool selected = actor->id() == scene.selected_actor_id();
        draw_rect(10, row_y - 17, 238, 24, selected ? Vec3{0.22f, 0.34f, 0.43f} : Vec3{0.09f, 0.10f, 0.105f}, selected ? 0.95f : 0.65f);
        std::string icon = actor->find_component<LightComponent>() ? "[L] " : "[A] ";
        draw_text(18, row_y, icon + actor->name(), selected ? Vec3{1.0f, 0.94f, 0.6f} : Vec3{0.78f, 0.84f, 0.82f});
        row_y += 27.0f;
    }
    row_y += 14.0f;
    draw_button(14, row_y, 64, 24, "+ Cube", false);
    draw_button(84, row_y, 64, 24, "+ Plane", false);
    draw_button(154, row_y, 78, 24, "+ Sphere", false);
    row_y += 31.0f;
    draw_button(14, row_y, 104, 24, "+ Directional", false);
    draw_button(124, row_y, 72, 24, "+ Point", false);
    row_y += 31.0f;
    draw_button(14, row_y, 80, 24, "Delete", false);

    const float details_x = static_cast<float>(width_ - 292);
    draw_text(details_x, 70, "Details");
    if (const Actor* selected = scene.selected_actor()) {
        const Transform& t = selected->root().local_transform;
        draw_text(details_x, 100, selected->name());
        draw_text(details_x, 132, "Type: " + selected->display_type(), {0.68f, 0.78f, 0.78f});
        draw_text(details_x, 174, "Transform");
        draw_text(details_x, 202, "Location  X " + std::to_string(t.translation.x) + "  Y " + std::to_string(t.translation.y) + "  Z " + std::to_string(t.translation.z));
        draw_text(details_x, 228, "Rotation  X " + std::to_string(t.rotation.x) + "  Y " + std::to_string(t.rotation.y) + "  Z " + std::to_string(t.rotation.z));
        draw_text(details_x, 254, "Scale     X " + std::to_string(t.scale.x) + "  Y " + std::to_string(t.scale.y) + "  Z " + std::to_string(t.scale.z));
        if (const auto* light = selected->find_component<LightComponent>()) {
            draw_text(details_x, 302, "Light");
            draw_text(details_x, 330, "Intensity " + std::to_string(light->intensity));
            draw_text(details_x, 356, "Range " + std::to_string(light->range));
        }
        if (const auto* mesh = selected->find_component<StaticMeshComponent>()) {
            draw_text(details_x, 302, "Static Mesh");
            draw_text(details_x, 330, "Parts " + std::to_string(mesh->parts.size()));
        }
    }

    draw_text(270, static_cast<float>(height_ - 16), "RMB orbit  |  MMB pan  |  Wheel zoom  |  Click actor/gizmo  |  Del delete", {0.62f, 0.72f, 0.70f});

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

Vec2 Renderer::project(Vec3 world, const Camera& camera) const {
    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    const Mat4 vp = Mat4::perspective(camera.fov_y_radians, aspect, 0.05f, 500.0f) * Mat4::look_at(camera.position(), camera.target, {0.0f, 1.0f, 0.0f});
    const float x = world.x * vp.m[0] + world.y * vp.m[4] + world.z * vp.m[8] + vp.m[12];
    const float y = world.x * vp.m[1] + world.y * vp.m[5] + world.z * vp.m[9] + vp.m[13];
    const float w = world.x * vp.m[3] + world.y * vp.m[7] + world.z * vp.m[11] + vp.m[15];
    if (std::abs(w) < 0.0001f) {
        return {-10000.0f, -10000.0f};
    }
    const float ndc_x = x / w;
    const float ndc_y = y / w;
    return {(ndc_x * 0.5f + 0.5f) * width_, (1.0f - (ndc_y * 0.5f + 0.5f)) * height_};
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
            draw_gizmo(*selected, engine.active_tool(), engine.transform_space());
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

void Renderer::draw_gizmo(const Actor& actor, EditorTool tool, TransformSpace space) const {
    const Vec3 origin = actor.root().local_transform.translation;
    const float scale = tool == EditorTool::Scale ? 0.85f : 1.35f;
    const bool scale_tool = tool == EditorTool::Scale;
    const Mat4 rotation = (space == TransformSpace::Local || scale_tool) ? Mat4::rotation_xyz(actor.root().local_transform.rotation) : Mat4::identity();
    auto axis = [&rotation](Vec3 a) {
        return normalize({
            a.x * rotation.m[0] + a.y * rotation.m[4] + a.z * rotation.m[8],
            a.x * rotation.m[1] + a.y * rotation.m[5] + a.z * rotation.m[9],
            a.x * rotation.m[2] + a.y * rotation.m[6] + a.z * rotation.m[10],
        });
    };
    const Vec3 x_axis = axis({1.0f, 0.0f, 0.0f});
    const Vec3 y_axis = axis({0.0f, 1.0f, 0.0f});
    const Vec3 z_axis = axis({0.0f, 0.0f, 1.0f});

    if (tool == EditorTool::Rotate) {
        glDisable(GL_CULL_FACE);
        glLineWidth(3.0f);
        constexpr int segments = 96;
        auto ring = [&](Vec3 a, Vec3 b, Vec3 color) {
            set_color(color);
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < segments; ++i) {
                const float t = static_cast<float>(i) / segments * kPi * 2.0f;
                vertex(origin + a * (std::cos(t) * scale) + b * (std::sin(t) * scale));
            }
            glEnd();
        };
        ring(y_axis, z_axis, {0.85f, 0.2f, 0.16f});
        ring(x_axis, z_axis, {0.25f, 0.68f, 0.31f});
        ring(x_axis, y_axis, {0.22f, 0.48f, 0.78f});
        glLineWidth(1.0f);
        glEnable(GL_CULL_FACE);
    } else {
        draw_line(origin, origin + x_axis * scale, {0.85f, 0.2f, 0.16f}, 5.0f);
        draw_line(origin, origin + y_axis * scale, {0.25f, 0.68f, 0.31f}, 5.0f);
        draw_line(origin, origin + z_axis * scale, {0.22f, 0.48f, 0.78f}, 5.0f);

        const Vec3 endpoints[] = {origin + x_axis * scale, origin + y_axis * scale, origin + z_axis * scale};
        const Vec3 colors[] = {{0.85f, 0.2f, 0.16f}, {0.25f, 0.68f, 0.31f}, {0.22f, 0.48f, 0.78f}};
        for (int i = 0; i < 3; ++i) {
            glPushMatrix();
            glTranslatef(endpoints[i].x, endpoints[i].y, endpoints[i].z);
            glScalef(0.16f, 0.16f, 0.16f);
            Material handle;
            handle.base_color = colors[i];
            draw_cube(handle);
            glPopMatrix();
        }
    }

    glPushMatrix();
    glTranslatef(origin.x, origin.y, origin.z);
    glScalef(0.14f, 0.14f, 0.14f);
    Material center;
    center.base_color = {0.72f, 0.66f, 0.28f};
    draw_cube(center);
    glPopMatrix();
}

void Renderer::draw_text(float x, float y, const std::string& text, Vec3 color) const {
    if (font_base_ == 0 || text.empty()) {
        return;
    }
    set_color(color);
    glRasterPos2f(x, y);
    glListBase(font_base_ - 32);
    glCallLists(static_cast<GLsizei>(text.size()), GL_UNSIGNED_BYTE, text.c_str());
}

void Renderer::draw_rect(float x, float y, float w, float h, Vec3 color, float alpha) const {
    set_color(color, alpha);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void Renderer::draw_button(float x, float y, float w, float h, const std::string& label, bool active) const {
    draw_rect(x, y, w, h, active ? Vec3{0.20f, 0.34f, 0.39f} : Vec3{0.12f, 0.13f, 0.14f}, 0.95f);
    draw_text(x + 8.0f, y + 17.0f, label, active ? Vec3{1.0f, 0.94f, 0.58f} : Vec3{0.78f, 0.83f, 0.81f});
}

} // namespace Forge

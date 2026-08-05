#include "Forge/App.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace Forge {
namespace {

bool key_pressed(int key) {
    return (GetAsyncKeyState(key) & 0x0001) != 0;
}

bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x <= rx + rw && y <= ry + rh;
}

float distance_2d(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::wstring widen(const std::string& value) {
    return {value.begin(), value.end()};
}

} // namespace

int App::run() {
    if (!window_.create(L"Forge Engine C++ Prototype", 1280, 720)) {
        std::wcerr << L"Failed to create Forge window: " << window_.last_error() << L"\n";
        return 1;
    }

    renderer_.initialize(window_.device_context());
    last_frame_ = std::chrono::steady_clock::now();

    while (!window_.should_close()) {
        window_.begin_frame();
        if (!window_.poll_messages()) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto delta = std::chrono::duration<float>(now - last_frame_).count();
        last_frame_ = now;

        begin_frame();
        handle_keyboard();
        handle_mouse();
        engine_.tick(delta);
        renderer_.resize(window_.width(), window_.height());
        renderer_.render(scene_, camera_, engine_);
        renderer_.render_editor_overlay(scene_, camera_, {engine_.active_tool(), engine_.transform_space(), snap_to_grid_, 0});
        window_.swap_buffers();
        update_window_title();
    }

    return 0;
}

void App::begin_frame() {
    camera_.pitch = std::clamp(camera_.pitch, -1.45f, 1.2f);
    camera_.distance = std::clamp(camera_.distance, 1.0f, 80.0f);
}

void App::handle_keyboard() {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        PostQuitMessage(0);
    }
    if (key_pressed('W')) {
        engine_.set_active_tool(EditorTool::Move);
    }
    if (key_pressed('E')) {
        engine_.set_active_tool(EditorTool::Rotate);
    }
    if (key_pressed('R')) {
        engine_.set_active_tool(EditorTool::Scale);
    }
    if (key_pressed('Q')) {
        engine_.set_active_tool(EditorTool::Select);
    }
    if (key_pressed('X')) {
        engine_.toggle_transform_space();
    }
    if (key_pressed('G')) {
        snap_to_grid_ = !snap_to_grid_;
    }
    if (key_pressed(VK_DELETE)) {
        scene_.destroy_selected_actor();
    }
    if (key_pressed('1')) {
        add_mesh(MeshPrimitive::Cube);
    }
    if (key_pressed('2')) {
        add_mesh(MeshPrimitive::Plane);
    }
    if (key_pressed('3')) {
        add_mesh(MeshPrimitive::Sphere);
    }
}

void App::handle_mouse() {
    const MouseState& mouse = window_.mouse();

    if (mouse.wheel_delta != 0 && !is_over_editor_ui(mouse.x, mouse.y)) {
        camera_.distance *= 1.0f - static_cast<float>(mouse.wheel_delta) / 2400.0f;
    }

    if (mouse.right_down && !is_over_editor_ui(mouse.x, mouse.y)) {
        camera_.yaw -= static_cast<float>(mouse.delta_x) * 0.006f;
        camera_.pitch -= static_cast<float>(mouse.delta_y) * 0.006f;
    }
    if (mouse.middle_down && !is_over_editor_ui(mouse.x, mouse.y)) {
        const float scale = camera_.distance * 0.0018f;
        camera_.target -= camera_.right() * (static_cast<float>(mouse.delta_x) * scale);
        camera_.target += camera_.up() * (static_cast<float>(mouse.delta_y) * scale);
    }

    if (mouse.left_pressed) {
        if (is_over_editor_ui(mouse.x, mouse.y)) {
            handle_ui_click(mouse.x, mouse.y);
        } else if (const DragMode gizmo = pick_gizmo(mouse.x, mouse.y); gizmo != DragMode::None) {
            begin_drag(gizmo);
        } else if (ActorId hit = pick_actor(mouse.x, mouse.y); hit != 0) {
            scene_.select_actor(hit);
            switch (engine_.active_tool()) {
            case EditorTool::Select:
            case EditorTool::Move:
                begin_drag(DragMode::MoveCenter);
                break;
            case EditorTool::Rotate:
                begin_drag(DragMode::RotateY);
                break;
            case EditorTool::Scale:
                begin_drag(DragMode::ScaleCenter);
                break;
            }
        }
    }

    if (mouse.left_down && drag_mode_ != DragMode::None) {
        drag_selected_actor(static_cast<float>(mouse.x - drag_start_x_), static_cast<float>(mouse.y - drag_start_y_));
    }
    if (mouse.left_released) {
        drag_mode_ = DragMode::None;
    }
}

void App::handle_ui_click(int x, int y) {
    if (point_in_rect(x, y, 124, 9, 68, 25)) engine_.set_active_tool(EditorTool::Select);
    if (point_in_rect(x, y, 198, 9, 60, 25)) engine_.set_active_tool(EditorTool::Move);
    if (point_in_rect(x, y, 264, 9, 68, 25)) engine_.set_active_tool(EditorTool::Rotate);
    if (point_in_rect(x, y, 338, 9, 64, 25)) engine_.set_active_tool(EditorTool::Scale);
    if (point_in_rect(x, y, 414, 9, 88, 25)) engine_.toggle_transform_space();
    if (point_in_rect(x, y, 510, 9, 92, 25)) snap_to_grid_ = !snap_to_grid_;

    int row_y = 79;
    for (const auto& actor : scene_.actors()) {
        if (point_in_rect(x, y, 10, row_y, 238, 24)) {
            scene_.select_actor(actor->id());
            return;
        }
        row_y += 27;
    }

    row_y += 14;
    if (point_in_rect(x, y, 14, row_y, 64, 24)) add_mesh(MeshPrimitive::Cube);
    if (point_in_rect(x, y, 84, row_y, 64, 24)) add_mesh(MeshPrimitive::Plane);
    if (point_in_rect(x, y, 154, row_y, 78, 24)) add_mesh(MeshPrimitive::Sphere);
    row_y += 31;
    if (point_in_rect(x, y, 14, row_y, 104, 24)) add_light(LightKind::Directional);
    if (point_in_rect(x, y, 124, row_y, 72, 24)) add_light(LightKind::Point);
    row_y += 31;
    if (point_in_rect(x, y, 14, row_y, 80, 24)) scene_.destroy_selected_actor();
}

void App::begin_drag(DragMode mode) {
    drag_mode_ = mode;
    drag_start_x_ = window_.mouse().x;
    drag_start_y_ = window_.mouse().y;
    if (const Actor* actor = scene_.selected_actor()) {
        drag_start_transform_ = actor->root().local_transform;
    }
}

void App::drag_selected_actor(float dx, float dy) {
    Actor* actor = scene_.selected_actor();
    if (actor == nullptr) {
        return;
    }

    Transform next = drag_start_transform_;
    const float move_scale = camera_.distance * 0.0025f;
    const float scalar = (std::abs(dx) > std::abs(dy) ? dx : -dy);
    switch (drag_mode_) {
    case DragMode::MoveCenter:
        next.translation = drag_start_transform_.translation + camera_.right() * (dx * move_scale) - camera_.up() * (dy * move_scale);
        break;
    case DragMode::MoveX:
    case DragMode::MoveY:
    case DragMode::MoveZ:
        next.translation = drag_start_transform_.translation + gizmo_axis(drag_mode_, drag_start_transform_) * (scalar * move_scale);
        break;
    case DragMode::RotateX:
        next.rotation.x = drag_start_transform_.rotation.x + scalar * 0.012f;
        break;
    case DragMode::RotateY:
        next.rotation.y = drag_start_transform_.rotation.y + scalar * 0.012f;
        break;
    case DragMode::RotateZ:
        next.rotation.z = drag_start_transform_.rotation.z + scalar * 0.012f;
        break;
    case DragMode::ScaleCenter: {
        const float factor = std::max(0.05f, 1.0f + scalar * 0.008f);
        next.scale = drag_start_transform_.scale * factor;
        break;
    }
    case DragMode::ScaleX:
        next.scale.x = std::max(0.05f, drag_start_transform_.scale.x + scalar * 0.01f);
        break;
    case DragMode::ScaleY:
        next.scale.y = std::max(0.05f, drag_start_transform_.scale.y + scalar * 0.01f);
        break;
    case DragMode::ScaleZ:
        next.scale.z = std::max(0.05f, drag_start_transform_.scale.z + scalar * 0.01f);
        break;
    case DragMode::None:
    case DragMode::CameraOrbit:
    case DragMode::CameraPan:
        break;
    }

    if (snap_to_grid_ && (drag_mode_ == DragMode::MoveCenter || drag_mode_ == DragMode::MoveX || drag_mode_ == DragMode::MoveZ)) {
        next.translation.x = std::round(next.translation.x);
        next.translation.z = std::round(next.translation.z);
    }
    actor->root().local_transform = next;
}

void App::add_mesh(MeshPrimitive primitive) {
    Transform t;
    t.translation = camera_.target + camera_.forward() * 1.5f;
    if (primitive == MeshPrimitive::Cube) {
        t.translation.y += 0.5f;
        scene_.create_static_mesh_actor("Cube", primitive, t);
    } else if (primitive == MeshPrimitive::Plane) {
        t.scale = {5.0f, 1.0f, 5.0f};
        scene_.create_static_mesh_actor("Plane", primitive, t);
    } else {
        t.translation.y += 0.7f;
        t.scale = {1.4f, 1.4f, 1.4f};
        scene_.create_static_mesh_actor("Sphere", primitive, t);
    }
}

void App::add_light(LightKind kind) {
    Transform t;
    t.translation = camera_.target + camera_.forward() * 1.5f + Vec3{0.0f, 1.0f, 0.0f};
    scene_.create_light_actor(kind == LightKind::Directional ? "Directional Light" : "Point Light", kind, t);
}

ActorId App::pick_actor(int x, int y) const {
    ActorId best_id = 0;
    float best_distance = 1000000.0f;
    const Vec2 mouse{static_cast<float>(x), static_cast<float>(y)};
    for (const auto& actor : scene_.actors()) {
        const Transform& t = actor->root().local_transform;
        Vec2 p = renderer_.project(t.translation, camera_);
        float radius = actor->find_component<LightComponent>() ? 18.0f : std::max({18.0f, t.scale.x * 28.0f, t.scale.y * 28.0f, t.scale.z * 28.0f});
        float dist = distance_2d(mouse, p);
        if (dist < radius && dist < best_distance) {
            best_distance = dist;
            best_id = actor->id();
        }
    }
    return best_id;
}

DragMode App::pick_gizmo(int x, int y) const {
    const Actor* actor = scene_.selected_actor();
    if (actor == nullptr || engine_.active_tool() == EditorTool::Select) {
        return DragMode::None;
    }
    const Vec2 mouse{static_cast<float>(x), static_cast<float>(y)};
    const Vec3 origin = actor->root().local_transform.translation;
    const Vec2 center = renderer_.project(origin, camera_);
    if (distance_2d(mouse, center) < 18.0f) {
        return engine_.active_tool() == EditorTool::Scale ? DragMode::ScaleCenter : DragMode::MoveCenter;
    }
    const float len = 1.35f;
    struct Candidate { DragMode mode; Vec3 axis; };
    Candidate candidates[] = {
        {engine_.active_tool() == EditorTool::Rotate ? DragMode::RotateX : engine_.active_tool() == EditorTool::Scale ? DragMode::ScaleX : DragMode::MoveX, {1, 0, 0}},
        {engine_.active_tool() == EditorTool::Rotate ? DragMode::RotateY : engine_.active_tool() == EditorTool::Scale ? DragMode::ScaleY : DragMode::MoveY, {0, 1, 0}},
        {engine_.active_tool() == EditorTool::Rotate ? DragMode::RotateZ : engine_.active_tool() == EditorTool::Scale ? DragMode::ScaleZ : DragMode::MoveZ, {0, 0, 1}},
    };
    for (const Candidate& c : candidates) {
        Vec2 tip = renderer_.project(origin + c.axis * len, camera_);
        if (distance_2d(mouse, tip) < 24.0f) {
            return c.mode;
        }
    }
    return DragMode::None;
}

bool App::is_over_editor_ui(int x, int y) const {
    return y < 42 || x < 258 || x > window_.width() - 306;
}

Vec3 App::gizmo_axis(DragMode mode, const Transform& transform) const {
    Vec3 axis{1.0f, 0.0f, 0.0f};
    if (mode == DragMode::MoveY || mode == DragMode::ScaleY || mode == DragMode::RotateY) axis = {0.0f, 1.0f, 0.0f};
    if (mode == DragMode::MoveZ || mode == DragMode::ScaleZ || mode == DragMode::RotateZ) axis = {0.0f, 0.0f, 1.0f};
    if (engine_.transform_space() == TransformSpace::World) {
        return axis;
    }
    const Mat4 rotation = Mat4::rotation_xyz(transform.rotation);
    return normalize({
        axis.x * rotation.m[0] + axis.y * rotation.m[4] + axis.z * rotation.m[8],
        axis.x * rotation.m[1] + axis.y * rotation.m[5] + axis.z * rotation.m[9],
        axis.x * rotation.m[2] + axis.y * rotation.m[6] + axis.z * rotation.m[10],
    });
}

void App::update_window_title() {
    const Actor* selected = scene_.selected_actor();
    std::wstringstream title;
    title << L"Forge Engine C++ Prototype";
    title << L" | actors: " << scene_.actors().size();
    title << L" | selected: " << (selected != nullptr ? widen(selected->name()) : L"None");
    title << L" | RMB orbit, MMB pan, wheel zoom, Q/W/E/R tools, X local/world, G snap";
    SetWindowTextW(window_.hwnd(), title.str().c_str());
}

} // namespace Forge

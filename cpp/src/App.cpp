#include "Forge/App.hpp"
#include "Forge/Importer.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace Forge {
namespace {

bool key_pressed(int key) {
    return (GetAsyncKeyState(key) & 0x0001) != 0;
}

float distance_2d(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

float distance_to_segment_2d(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab{b.x - a.x, b.y - a.y};
    const Vec2 ap{p.x - a.x, p.y - a.y};
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    if (len2 <= 0.0001f) {
        return distance_2d(p, a);
    }
    const float t = std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f);
    return distance_2d(p, {a.x + ab.x * t, a.y + ab.y * t});
}

std::wstring widen(const std::string& value) {
    return {value.begin(), value.end()};
}

} // namespace

int App::run(const std::vector<std::filesystem::path>& startup_imports) {
    if (!window_.create(L"Forge Engine C++ Prototype", 1280, 720)) {
        std::wcerr << L"Failed to create Forge window: " << window_.last_error() << L"\n";
        return 1;
    }

    renderer_.initialize(window_.device_context());
    editor_ui_.initialize(window_);
    for (const auto& path : startup_imports) {
        import_model(path);
    }
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
        editor_ui_.begin_frame();
        if (!editor_ui_.wants_keyboard()) {
            handle_keyboard();
        }
        import_pending_drops();
        handle_mouse();
        engine_.tick(delta);
        renderer_.resize(window_.width(), window_.height());
        renderer_.render(scene_, camera_, engine_);
        execute_editor_command(editor_ui_.draw(scene_, engine_, camera_, snap_to_grid_, status_));
        editor_ui_.render();
        window_.swap_buffers();
        update_window_title();
    }

    editor_ui_.shutdown();
    renderer_.shutdown();
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
    if (key_pressed('1')) {
        engine_.set_style(StylePreset::Character);
        status_ = "Style preset: Character";
    }
    if (key_pressed('2')) {
        engine_.set_style(StylePreset::Street);
        status_ = "Style preset: Street";
    }
    if (key_pressed('3')) {
        engine_.set_style(StylePreset::Combat);
        status_ = "Style preset: Combat";
    }
    if (key_pressed(VK_F1)) engine_.set_debug_view(DebugView::Final);
    if (key_pressed(VK_F2)) engine_.set_debug_view(DebugView::Albedo);
    if (key_pressed(VK_F3)) engine_.set_debug_view(DebugView::Normals);
    if (key_pressed(VK_F4)) engine_.set_debug_view(DebugView::LightingOnly);
    if (key_pressed(VK_F5)) engine_.set_debug_view(DebugView::Roughness);
    if (key_pressed(VK_F6)) engine_.set_debug_view(DebugView::Metallic);
    if (key_pressed('X')) {
        engine_.toggle_transform_space();
    }
    if (key_pressed('G')) {
        snap_to_grid_ = !snap_to_grid_;
    }
    if (key_pressed(VK_DELETE)) {
        scene_.destroy_selected_actor();
    }
    if (key_pressed(VK_HOME)) {
        reset_scene();
    }
    if (key_pressed('4')) {
        add_light(LightKind::Directional);
    }
    if (key_pressed('5')) {
        add_light(LightKind::Point);
    }
    if (key_pressed('6')) {
        add_light(LightKind::Spot);
    }
}

void App::handle_mouse() {
    const MouseState& mouse = window_.mouse();
    const bool ui_wants_mouse = editor_ui_.wants_mouse();

    if (mouse.wheel_delta != 0 && !ui_wants_mouse) {
        camera_.distance *= 1.0f - static_cast<float>(mouse.wheel_delta) / 2400.0f;
    }

    if (mouse.right_down && !ui_wants_mouse) {
        camera_.yaw -= static_cast<float>(mouse.delta_x) * 0.006f;
        camera_.pitch -= static_cast<float>(mouse.delta_y) * 0.006f;
    }
    if (mouse.middle_down && !ui_wants_mouse) {
        const float scale = camera_.distance * 0.0018f;
        camera_.target -= camera_.right() * (static_cast<float>(mouse.delta_x) * scale);
        camera_.target += camera_.up() * (static_cast<float>(mouse.delta_y) * scale);
    }

    if (mouse.left_pressed && !ui_wants_mouse) {
        if (const DragMode gizmo = pick_gizmo(mouse.x, mouse.y); gizmo != DragMode::None) {
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

void App::execute_editor_command(EditorCommand command) {
    switch (command) {
    case EditorCommand::AddCube:
        add_mesh(MeshPrimitive::Cube);
        break;
    case EditorCommand::AddPlane:
        add_mesh(MeshPrimitive::Plane);
        break;
    case EditorCommand::AddSphere:
        add_mesh(MeshPrimitive::Sphere);
        break;
    case EditorCommand::AddDirectionalLight:
        add_light(LightKind::Directional);
        break;
    case EditorCommand::AddPointLight:
        add_light(LightKind::Point);
        break;
    case EditorCommand::AddSpotLight:
        add_light(LightKind::Spot);
        break;
    case EditorCommand::DeleteSelection:
        scene_.destroy_selected_actor();
        status_ = "Deleted selected actor";
        break;
    case EditorCommand::ResetScene:
        reset_scene();
        break;
    case EditorCommand::None:
        break;
    }
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
    const float fallback_scalar = (std::abs(dx) > std::abs(dy) ? dx : -dy);
    switch (drag_mode_) {
    case DragMode::MoveCenter:
        next.translation = drag_start_transform_.translation + camera_.right() * (dx * move_scale) - camera_.up() * (dy * move_scale);
        break;
    case DragMode::MoveX:
    case DragMode::MoveY:
    case DragMode::MoveZ: {
        const float scalar = screen_axis_drag_delta(drag_mode_, drag_start_transform_, dx, dy);
        next.translation = drag_start_transform_.translation + gizmo_axis(drag_mode_, drag_start_transform_) * (scalar * move_scale);
        break;
    }
    case DragMode::RotateX:
    case DragMode::RotateY:
    case DragMode::RotateZ: {
        const Vec3 axis = gizmo_axis(drag_mode_, drag_start_transform_);
        const float angle = screen_ring_drag_delta(drag_mode_, drag_start_transform_, window_.mouse().x, window_.mouse().y);
        const Mat4 delta = Mat4::rotation_axis_angle(axis, angle);
        const Mat4 start = Mat4::rotation_xyz(drag_start_transform_.rotation);
        next.rotation = euler_from_rotation_xyz(delta * start);
        break;
    }
    case DragMode::ScaleCenter: {
        const float factor = std::max(0.05f, 1.0f + fallback_scalar * 0.008f);
        next.scale = drag_start_transform_.scale * factor;
        break;
    }
    case DragMode::ScaleX: {
        const float scalar = screen_axis_drag_delta(drag_mode_, drag_start_transform_, dx, dy);
        next.scale.x = std::max(0.05f, drag_start_transform_.scale.x + scalar * 0.01f);
        break;
    }
    case DragMode::ScaleY: {
        const float scalar = screen_axis_drag_delta(drag_mode_, drag_start_transform_, dx, dy);
        next.scale.y = std::max(0.05f, drag_start_transform_.scale.y + scalar * 0.01f);
        break;
    }
    case DragMode::ScaleZ: {
        const float scalar = screen_axis_drag_delta(drag_mode_, drag_start_transform_, dx, dy);
        next.scale.z = std::max(0.05f, drag_start_transform_.scale.z + scalar * 0.01f);
        break;
    }
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
        status_ = "Added Cube";
    } else if (primitive == MeshPrimitive::Plane) {
        t.scale = {5.0f, 1.0f, 5.0f};
        scene_.create_static_mesh_actor("Plane", primitive, t);
        status_ = "Added Plane";
    } else {
        t.translation.y += 0.7f;
        t.scale = {1.4f, 1.4f, 1.4f};
        scene_.create_static_mesh_actor("Sphere", primitive, t);
        status_ = "Added Sphere";
    }
}

void App::add_light(LightKind kind) {
    Transform t;
    t.translation = camera_.target + camera_.forward() * 1.5f + Vec3{0.0f, 1.0f, 0.0f};
    const char* name = "Point Light";
    if (kind == LightKind::Directional) {
        name = "Directional Light";
    } else if (kind == LightKind::Spot) {
        name = "Spot Light";
    }
    scene_.create_light_actor(name, kind, t);
    status_ = std::string("Added ") + name;
}

void App::reset_scene() {
    scene_ = make_default_scene();
    camera_ = Camera{};
    drag_mode_ = DragMode::None;
    status_ = "Scene reset";
}

void App::import_model(const std::filesystem::path& path) {
    try {
        ImportedModel model = import_model_file(path);
        if (model.parts.empty()) {
            status_ = "Import produced no mesh parts";
            return;
        }
        Transform transform;
        std::string name = path.stem().string().empty() ? "Imported Model" : path.stem().string();
        const std::size_t count = model.parts.size();
        scene_.create_mesh_actor(name, std::move(model.parts), transform);
        status_ = "Imported " + std::to_string(count) + " mesh part(s): " + path.filename().string();
    } catch (const std::exception& error) {
        status_ = std::string("Import failed: ") + error.what();
    }
}

void App::import_pending_drops() {
    for (const std::wstring& file : window_.take_dropped_files()) {
        import_model(std::filesystem::path(file));
    }
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
    const bool scale_tool = engine_.active_tool() == EditorTool::Scale;
    const float len = scale_tool ? 1.15f : 1.35f;
    struct Candidate { DragMode mode; DragMode axis_mode; };
    Candidate candidates[] = {
        {axis_mode_for_tool(DragMode::MoveX), axis_mode_for_tool(DragMode::MoveX)},
        {axis_mode_for_tool(DragMode::MoveY), axis_mode_for_tool(DragMode::MoveY)},
        {axis_mode_for_tool(DragMode::MoveZ), axis_mode_for_tool(DragMode::MoveZ)},
    };

    float best_score = 1000000.0f;
    DragMode best_mode = DragMode::None;
    for (const Candidate& c : candidates) {
        if (engine_.active_tool() == EditorTool::Rotate) {
            Vec3 tangent_a;
            Vec3 tangent_b;
            ring_basis_for_mode(c.mode, actor->root().local_transform, tangent_a, tangent_b);
            constexpr int segments = 96;
            for (int i = 0; i < segments; ++i) {
                const float t0 = static_cast<float>(i) / segments * 6.2831853f;
                const float t1 = static_cast<float>(i + 1) / segments * 6.2831853f;
                const Vec2 a = renderer_.project(origin + tangent_a * (std::cos(t0) * len) + tangent_b * (std::sin(t0) * len), camera_);
                const Vec2 b = renderer_.project(origin + tangent_a * (std::cos(t1) * len) + tangent_b * (std::sin(t1) * len), camera_);
                const float score = distance_to_segment_2d(mouse, a, b);
                if (score < best_score) {
                    best_score = score;
                    best_mode = c.mode;
                }
            }
        } else {
            const Vec3 axis = gizmo_axis(c.axis_mode, actor->root().local_transform);
            const Vec2 tip = renderer_.project(origin + axis * len, camera_);
            const float line_score = distance_to_segment_2d(mouse, center, tip);
            const float tip_score = distance_2d(mouse, tip);
            const float score = std::min(line_score, tip_score);
            if (score < best_score) {
                best_score = score;
                best_mode = c.mode;
            }
        }
    }
    const float threshold = engine_.active_tool() == EditorTool::Rotate ? 16.0f : 22.0f;
    return best_score <= threshold ? best_mode : DragMode::None;
}

Vec3 App::gizmo_axis(DragMode mode, const Transform& transform) const {
    const Vec3 axis = base_axis_for_mode(mode);
    const bool scale_axis = mode == DragMode::ScaleX || mode == DragMode::ScaleY || mode == DragMode::ScaleZ;
    if (engine_.transform_space() == TransformSpace::World && !scale_axis) {
        return axis;
    }
    const Mat4 rotation = Mat4::rotation_xyz(transform.rotation);
    return normalize({
        axis.x * rotation.m[0] + axis.y * rotation.m[4] + axis.z * rotation.m[8],
        axis.x * rotation.m[1] + axis.y * rotation.m[5] + axis.z * rotation.m[9],
        axis.x * rotation.m[2] + axis.y * rotation.m[6] + axis.z * rotation.m[10],
    });
}

Vec3 App::base_axis_for_mode(DragMode mode) const {
    if (mode == DragMode::MoveY || mode == DragMode::ScaleY || mode == DragMode::RotateY) {
        return {0.0f, 1.0f, 0.0f};
    }
    if (mode == DragMode::MoveZ || mode == DragMode::ScaleZ || mode == DragMode::RotateZ) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {1.0f, 0.0f, 0.0f};
}

DragMode App::axis_mode_for_tool(DragMode move_axis_mode) const {
    if (engine_.active_tool() == EditorTool::Rotate) {
        if (move_axis_mode == DragMode::MoveY) return DragMode::RotateY;
        if (move_axis_mode == DragMode::MoveZ) return DragMode::RotateZ;
        return DragMode::RotateX;
    }
    if (engine_.active_tool() == EditorTool::Scale) {
        if (move_axis_mode == DragMode::MoveY) return DragMode::ScaleY;
        if (move_axis_mode == DragMode::MoveZ) return DragMode::ScaleZ;
        return DragMode::ScaleX;
    }
    return move_axis_mode;
}

void App::ring_basis_for_mode(DragMode rotate_mode, const Transform& transform, Vec3& a, Vec3& b) const {
    if (rotate_mode == DragMode::RotateX) {
        a = gizmo_axis(DragMode::MoveY, transform);
        b = gizmo_axis(DragMode::MoveZ, transform);
    } else if (rotate_mode == DragMode::RotateY) {
        a = gizmo_axis(DragMode::MoveX, transform);
        b = gizmo_axis(DragMode::MoveZ, transform);
    } else {
        a = gizmo_axis(DragMode::MoveX, transform);
        b = gizmo_axis(DragMode::MoveY, transform);
    }
}

float App::screen_axis_drag_delta(DragMode axis_mode, const Transform& transform, float dx, float dy) const {
    const Vec3 origin = transform.translation;
    const float len = (axis_mode == DragMode::ScaleX || axis_mode == DragMode::ScaleY || axis_mode == DragMode::ScaleZ) ? 1.15f : 1.35f;
    const Vec2 center = renderer_.project(origin, camera_);
    const Vec2 tip = renderer_.project(origin + gizmo_axis(axis_mode, transform) * len, camera_);
    Vec2 direction{tip.x - center.x, tip.y - center.y};
    const float direction_length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (direction_length <= 0.001f) {
        return std::abs(dx) > std::abs(dy) ? dx : -dy;
    }
    direction.x /= direction_length;
    direction.y /= direction_length;
    return dx * direction.x + dy * direction.y;
}

float App::screen_ring_drag_delta(DragMode rotate_mode, const Transform& transform, int current_x, int current_y) const {
    const Vec2 center = renderer_.project(transform.translation, camera_);
    const float start_angle = std::atan2(static_cast<float>(drag_start_y_) - center.y, static_cast<float>(drag_start_x_) - center.x);
    const float current_angle = std::atan2(static_cast<float>(current_y) - center.y, static_cast<float>(current_x) - center.x);
    float delta = current_angle - start_angle;
    while (delta > 3.14159265f) delta -= 6.2831853f;
    while (delta < -3.14159265f) delta += 6.2831853f;

    Vec3 ring_a;
    Vec3 ring_b;
    ring_basis_for_mode(rotate_mode, transform, ring_a, ring_b);
    const Vec2 screen_a = renderer_.project(transform.translation + ring_a, camera_);
    const Vec2 screen_b = renderer_.project(transform.translation + ring_b, camera_);
    const float handedness = ((screen_a.x - center.x) * (screen_b.y - center.y)) - ((screen_a.y - center.y) * (screen_b.x - center.x));
    return handedness < 0.0f ? -delta : delta;
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

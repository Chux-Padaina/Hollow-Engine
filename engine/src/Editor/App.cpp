// Implementation of App: the main loop, shortcuts, viewport camera, picking,
// gizmo dragging, undo/redo, scene file commands, Play mode and the
// capture/showcase mode. See App.hpp for the overview.
#include "Hollow/Editor/App.hpp"

#include "Hollow/Assets/Importer.hpp"
#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"
#include "Hollow/Scene/SceneFactory.hpp"
#include "Hollow/Scene/SceneSerializer.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <array>
#include <cctype>
#include <cmath>
#include <format>

namespace Hollow {
namespace {

constexpr const wchar_t* kWindowTitle = L"Hollow Engine v0.4";
constexpr const wchar_t* kSceneFilter = L"Hollow scenes (*.hscene)\0*.hscene\0All files\0*.*\0";
constexpr const wchar_t* kModelFilter = L"Models (*.glb;*.gltf;*.obj)\0*.glb;*.gltf;*.obj\0All files\0*.*\0";
constexpr const wchar_t* kHdrFilter = L"Radiance HDR (*.hdr)\0*.hdr\0";

std::string lower_extension(const std::filesystem::path& path) {
    std::string ext = to_utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

float distance_2d(Vec2 a, Vec2 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

float distance_to_segment_2d(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab{b.x - a.x, b.y - a.y};
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    if (len2 <= 0.0001f) {
        return distance_2d(p, a);
    }
    const float t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0f, 1.0f);
    return distance_2d(p, {a.x + ab.x * t, a.y + ab.y * t});
}

bool is_move(DragMode mode) {
    return mode == DragMode::MoveCenter || mode == DragMode::MoveX || mode == DragMode::MoveY || mode == DragMode::MoveZ;
}

bool is_rotate(DragMode mode) {
    return mode == DragMode::RotateX || mode == DragMode::RotateY || mode == DragMode::RotateZ;
}

bool is_scale(DragMode mode) {
    return mode == DragMode::ScaleCenter || mode == DragMode::ScaleX || mode == DragMode::ScaleY || mode == DragMode::ScaleZ;
}

Vec3 base_axis(DragMode mode) {
    switch (mode) {
    case DragMode::MoveY:
    case DragMode::RotateY:
    case DragMode::ScaleY:
        return {0.0f, 1.0f, 0.0f};
    case DragMode::MoveZ:
    case DragMode::RotateZ:
    case DragMode::ScaleZ:
        return {0.0f, 0.0f, 1.0f};
    default:
        return {1.0f, 0.0f, 0.0f};
    }
}

// The axis drag modes for the active tool, in X/Y/Z order.
std::array<DragMode, 3> axis_modes(EditorTool tool) {
    switch (tool) {
    case EditorTool::Rotate:
        return {DragMode::RotateX, DragMode::RotateY, DragMode::RotateZ};
    case EditorTool::Scale:
        return {DragMode::ScaleX, DragMode::ScaleY, DragMode::ScaleZ};
    default:
        return {DragMode::MoveX, DragMode::MoveY, DragMode::MoveZ};
    }
}

bool effectively_active(const Scene& scene, const Actor& actor) {
    const Actor* current = &actor;
    while (current != nullptr) {
        if (!current->active) {
            return false;
        }
        current = current->parent() != kNoActor ? scene.find(current->parent()) : nullptr;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Main loop

int App::run(const std::vector<std::filesystem::path>& startup_files) {
    if (!window_.create(kWindowTitle, 1280, 720)) {
        const std::wstring message = L"Failed to create the window: " + window_.last_error();
        MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_ICONERROR);
        return 1;
    }
    log_info("Hollow Engine v0.4 starting");
    editor_ui_.initialize(window_);
    if (!renderer_.initialize()) {
        status_ = "Renderer failed to start: " + renderer_.error();
        const std::wstring message = utf8_to_wide(renderer_.error());
        MessageBoxW(window_.hwnd(), message.c_str(), kWindowTitle, MB_ICONERROR);
    }

    // The bundled sky lives next to the executable (CMake copies assets/
    // there), so it resolves the same way however the engine is launched.
    const std::filesystem::path default_environment = executable_directory() / "assets" / "skybox" / "default_sky.hdr";
    std::error_code ec;
    if (renderer_.ready() && std::filesystem::exists(default_environment, ec)) {
        default_environment_ = to_utf8(default_environment);
        load_environment(default_environment, false);
    }

    build_default_scene(engine_.scene(), engine_.assets());
    std::filesystem::path cycle = executable_directory() / "assets" / "skybox" / "cycle";
    showcase_.directory = std::filesystem::is_directory(cycle, ec) ? cycle : executable_directory() / "assets" / "skybox";
    rescan_skybox_playlist();

    // Gameplay code: Game.dll next to the executable (hot-reloaded).
    engine_.game_module().load(executable_directory() / L"Game.dll");

    // Flags: --showcase (open the rendering showcase), --debug-view=N.
    for (const auto& path : startup_files) {
        const std::wstring arg = path.native();
        if (arg == L"--showcase") {
            new_showcase_scene();
        } else if (arg.rfind(L"--debug-view=", 0) == 0) {
            const int value = std::wcstol(arg.c_str() + 13, nullptr, 10);
            if (value >= 0 && value < static_cast<int>(DebugView::Count)) {
                debug_view_ = static_cast<DebugView>(value);
            }
        } else {
            open_file(path);
        }
    }
    dirty_ = false;
    undo_.clear();
    last_frame_ = std::chrono::steady_clock::now();

    for (;;) {
        window_.poll_messages();
        if (window_.should_close()) {
            if (engine_.playing()) {
                stop_play();
            }
            if (confirm_discard_changes()) {
                break;
            }
            window_.cancel_close();
        }

        const auto now = std::chrono::steady_clock::now();
        const float delta = std::min(std::chrono::duration<float>(now - last_frame_).count(), 0.25f);
        frame_ms_ = frame_ms_ * 0.9f + delta * 1000.0f * 0.1f;
        last_frame_ = now;

        editor_ui_.begin_frame(delta);
        // The viewport card's rectangle from the last UI layout (or the
        // whole window with the UI hidden).
        if (ui_visible_) {
            const ViewportRect& rect = editor_ui_.viewport();
            viewport_ = {rect.x, rect.y, rect.width, rect.height};
        } else {
            viewport_ = {0.0f, 0.0f, static_cast<float>(window_.width()), static_cast<float>(window_.height())};
        }
        if (!editor_ui_.wants_keyboard()) {
            handle_shortcuts();
        }
        for (const std::wstring& file : window_.take_dropped_files()) {
            open_file(std::filesystem::path(file));
        }
        handle_viewport_mouse(delta);
        update_turntable(delta);
        update_showcase(delta);
        process_skybox_preload();

        if (engine_.game_module().poll(!engine_.playing())) {
            refresh_script_params();
        }
        engine_.update(delta, window_.input());
        update_play_camera();
        camera_.clamp();

        renderer_.resize(viewport_width(), viewport_height());
        renderer_.render(engine_.scene(), camera_, engine_.render_settings(), make_render_view(), delta);

        if (ui_visible_) {
            EditorContext context{engine_, camera_, renderer_, window_, tool_, space_, debug_view_, view_, snap_, snap_translate_, snap_rotate_degrees_, showcase_, ui_visible_};
            context.can_undo = undo_.can_undo();
            context.can_redo = undo_.can_redo();
            context.dirty = dirty_;
            context.scene_name = scene_path_.empty() ? "Untitled" : to_utf8(scene_path_.filename());
            context.status = status_;
            context.frame_ms = frame_ms_;
            context.delta = delta;
            context.game_view = game_camera_active_;
            context.begin_edit = [this] { begin_edit(); };
            context.end_edit = [this] { end_edit(); };
            context.cancel_edit = [this] { cancel_edit(); };
            context.mark_dirty = [this] { dirty_ = true; };
            for (const EditorCommand& command : editor_ui_.draw(context)) {
                execute(command);
            }
        }
        editor_ui_.render();
        if (!ui_visible_) {
            renderer_.present(0, 0, window_.width(), window_.height());
        }
        window_.swap_buffers();
        update_window_title();
    }

    editor_ui_.shutdown();
    renderer_.shutdown();
    return 0;
}

RenderView App::make_render_view() const {
    RenderView view;
    view.debug_view = debug_view_;
    view.tool = tool_;
    view.space = space_;
    view.selected = engine_.scene().selected_actor_id();
    // Looking through a game camera shows the game view: no editor overlays.
    const bool game_view = game_camera_active_;
    view.draw_grid = view_.grid && !game_view;
    view.draw_gizmos = view_.gizmos && !game_view;
    view.draw_outlines = view_.outlines && !game_view;
    view.draw_icons = view_.icons && !game_view;
    view.draw_colliders = view_.colliders;
    view.draw_selection_collider = !game_view;
    view.editor = !game_view;
    // Highlight the gizmo handle under the cursor (or being dragged).
    auto axis_index = [](DragMode mode) -> std::uint32_t {
        switch (mode) {
        case DragMode::MoveX: case DragMode::RotateX: case DragMode::ScaleX: return 1;
        case DragMode::MoveY: case DragMode::RotateY: case DragMode::ScaleY: return 2;
        case DragMode::MoveZ: case DragMode::RotateZ: case DragMode::ScaleZ: return 3;
        case DragMode::MoveCenter: case DragMode::ScaleCenter: return 4;
        default: return 0;
        }
    };
    if (drag_mode_ != DragMode::None) {
        view.hovered_gizmo_axis = axis_index(drag_mode_);
    } else if (!game_view && !(ui_visible_ && editor_ui_.wants_mouse())) {
        view.hovered_gizmo_axis = axis_index(pick_gizmo(mouse_x(), mouse_y()));
    }
    return view;
}

void App::update_window_title() {
    std::wstring title = kWindowTitle;
    title += L" - " + (scene_path_.empty() ? std::wstring(L"Untitled") : scene_path_.filename().wstring());
    if (dirty_) {
        title += L"*";
    }
    if (engine_.playing()) {
        title += engine_.paused() ? L" [Paused]" : L" [Playing]";
    }
    window_.set_title(title);
}

void App::handle_shortcuts() {
    const Input& in = window_.input();
    const bool playing = engine_.playing();

    if (in.ctrl()) {
        if (in.key_pressed('N')) execute({EditorCommandType::NewScene});
        if (in.key_pressed('O')) execute({EditorCommandType::OpenScene});
        if (in.key_pressed('S')) execute({in.shift() ? EditorCommandType::SaveSceneAs : EditorCommandType::SaveScene});
        if (in.key_pressed('Z')) execute({in.shift() ? EditorCommandType::Redo : EditorCommandType::Undo});
        if (in.key_pressed('Y')) execute({EditorCommandType::Redo});
        if (in.key_pressed('D')) execute({EditorCommandType::Duplicate});
        if (in.key_pressed('I')) execute({EditorCommandType::ImportModel});
        if (in.key_pressed('P')) execute({playing ? EditorCommandType::Stop : EditorCommandType::Play});
        return;
    }

    if (in.key_pressed(VK_ESCAPE)) {
        if (drag_mode_ != DragMode::None) {
            end_drag(true);
        } else if (playing) {
            stop_play();
        } else {
            engine_.scene().clear_selection();
        }
    }

    // Function keys work in every mode.
    // F1-F8: the first debug views (Shift+F5 reloads shaders instead).
    for (int i = 0; i < 8 && !in.shift(); ++i) {
        if (in.key_pressed(VK_F1 + i)) {
            debug_view_ = static_cast<DebugView>(i);
        }
    }
    if (in.key_pressed(VK_F5) && in.shift()) {
        renderer_.reload_shaders();
    }
    if (in.key_pressed(VK_F9)) view_.gizmos = !view_.gizmos;
    if (in.key_pressed(VK_F10)) view_.grid = !view_.grid;
    // F11 must keep working with the UI hidden: it's the only way back.
    if (in.key_pressed(VK_F11)) ui_visible_ = !ui_visible_;

    // Letter keys belong to the game while playing.
    if (playing) {
        return;
    }
    if (in.key_pressed('Q')) tool_ = EditorTool::Select;
    if (in.key_pressed('W')) tool_ = EditorTool::Move;
    if (in.key_pressed('E')) tool_ = EditorTool::Rotate;
    if (in.key_pressed('R')) tool_ = EditorTool::Scale;
    if (in.key_pressed('X')) space_ = space_ == TransformSpace::World ? TransformSpace::Local : TransformSpace::World;
    if (in.key_pressed('G')) snap_ = !snap_;
    if (in.key_pressed('F')) focus_selected();
    if (in.key_pressed(VK_DELETE)) delete_selected();
    if (in.key_pressed(VK_HOME)) execute({EditorCommandType::ResetCamera});
    if (in.key_pressed('T')) {
        showcase_.turntable_enabled = !showcase_.turntable_enabled;
        showcase_.seconds_on_current = 0.0f;
        status_ = showcase_.turntable_enabled ? "Turntable on" : "Turntable off";
    }
    if (in.key_pressed(VK_OEM_4)) advance_skybox(-1); // [
    if (in.key_pressed(VK_OEM_6)) advance_skybox(1);  // ]
}

int App::mouse_x() const {
    return window_.input().mouse_x() - static_cast<int>(viewport_.x);
}

int App::mouse_y() const {
    return window_.input().mouse_y() - static_cast<int>(viewport_.y);
}

int App::viewport_width() const {
    return std::max(1, static_cast<int>(viewport_.width));
}

int App::viewport_height() const {
    return std::max(1, static_cast<int>(viewport_.height));
}

void App::handle_viewport_mouse(float) {
    const Input& in = window_.input();
    const bool inside = static_cast<float>(in.mouse_x()) >= viewport_.x && static_cast<float>(in.mouse_y()) >= viewport_.y &&
                        static_cast<float>(in.mouse_x()) < viewport_.x + viewport_.width && static_cast<float>(in.mouse_y()) < viewport_.y + viewport_.height;
    const bool ui_has_mouse = ui_visible_ && (editor_ui_.wants_mouse() || !inside);

    if (drag_mode_ != DragMode::None) {
        if (in.mouse_down(MouseButton::Left)) {
            update_drag();
        }
        if (!in.mouse_down(MouseButton::Left)) {
            end_drag(false);
        }
        return;
    }

    if (!game_camera_active_) {
        if (in.wheel_delta() != 0 && !ui_has_mouse) {
            camera_.distance *= 1.0f - static_cast<float>(in.wheel_delta()) / 1200.0f * 0.5f;
        }
        if (in.mouse_down(MouseButton::Right) && !ui_has_mouse) {
            camera_.yaw -= static_cast<float>(in.mouse_delta_x()) * 0.006f;
            camera_.pitch += static_cast<float>(in.mouse_delta_y()) * 0.006f; // drag down = look from above
        }
        if (in.mouse_down(MouseButton::Middle) && !ui_has_mouse) {
            // Pan so the point under the cursor follows it.
            const float units = camera_.world_units_per_pixel(camera_.target, viewport_height());
            camera_.target -= camera_.right() * (static_cast<float>(in.mouse_delta_x()) * units);
            camera_.target += camera_.up() * (static_cast<float>(in.mouse_delta_y()) * units);
        }
    }

    if (in.mouse_pressed(MouseButton::Left) && !ui_has_mouse) {
        const DragMode gizmo = pick_gizmo(mouse_x(), mouse_y());
        if (gizmo != DragMode::None) {
            begin_drag(gizmo);
            return;
        }
        const ActorId hit = pick_actor(mouse_x(), mouse_y());
        engine_.scene().select_actor(hit);
        if (hit != kNoActor && tool_ == EditorTool::Move) {
            begin_drag(DragMode::MoveCenter); // click-and-drag an object to move it
        }
    }
}

void App::update_play_camera() {
    game_camera_active_ = false;
    if (!engine_.playing()) {
        return;
    }
    for (const auto& actor : engine_.scene().actors()) {
        const auto* camera = actor->find_component<CameraComponent>();
        if (camera == nullptr || !camera->primary || !effectively_active(engine_.scene(), *actor)) {
            continue;
        }
        const Mat4& world = actor->world_matrix();
        camera_.set_view(world.translation_part(), transform_vector(world, {0.0f, 0.0f, -1.0f}), camera->fov_y_radians);
        game_camera_active_ = true;
        return;
    }
}

// ---------------------------------------------------------------------------
// Undo

JsonValue App::snapshot() const {
    return serialize_scene(engine_.scene());
}

void App::restore(const JsonValue& state) {
    try {
        deserialize_scene(engine_.scene(), state, engine_.assets());
    } catch (const std::exception& error) {
        log_error(std::format("Could not restore scene state: {}", error.what()));
    }
}

void App::begin_edit() {
    if (!engine_.playing() && !pending_edit_) {
        pending_edit_ = snapshot();
    }
}

void App::end_edit() {
    if (pending_edit_) {
        undo_.record(std::move(*pending_edit_));
        pending_edit_.reset();
        dirty_ = true;
    }
}

void App::cancel_edit() {
    pending_edit_.reset();
}

void App::record_undo() {
    if (!engine_.playing()) {
        undo_.record(snapshot());
        dirty_ = true;
    }
}

void App::undo() {
    if (engine_.playing()) {
        return;
    }
    if (auto previous = undo_.undo(snapshot())) {
        restore(*previous);
        dirty_ = true;
        status_ = "Undo";
    }
}

void App::redo() {
    if (engine_.playing()) {
        return;
    }
    if (auto next = undo_.redo(snapshot())) {
        restore(*next);
        dirty_ = true;
        status_ = "Redo";
    }
}

// ---------------------------------------------------------------------------
// Commands

void App::execute(const EditorCommand& command) {
    Scene& scene = engine_.scene();
    switch (command.type) {
    case EditorCommandType::None:
        break;
    case EditorCommandType::NewScene:
        new_scene(false);
        break;
    case EditorCommandType::NewDemoScene:
        new_scene(true);
        break;
    case EditorCommandType::NewShowcaseScene:
        new_showcase_scene();
        break;
    case EditorCommandType::OpenScene:
        open_scene_dialog();
        break;
    case EditorCommandType::SaveScene:
        save_scene(false);
        break;
    case EditorCommandType::SaveSceneAs:
        save_scene(true);
        break;
    case EditorCommandType::ImportModel:
        if (auto path = window_.open_file_dialog(kModelFilter, L"Import model"); !path.empty()) {
            import_model(path);
        }
        break;
    case EditorCommandType::LoadEnvironment:
        if (auto path = window_.open_file_dialog(kHdrFilter, L"Load HDRI environment"); !path.empty()) {
            load_environment(path);
            dirty_ = true;
        }
        break;
    case EditorCommandType::Exit:
        window_.request_close();
        break;
    case EditorCommandType::Undo:
        undo();
        break;
    case EditorCommandType::Redo:
        redo();
        break;
    case EditorCommandType::Duplicate:
        duplicate_selected();
        break;
    case EditorCommandType::Delete:
        delete_selected();
        break;
    case EditorCommandType::AddEmpty:
        record_undo();
        select_new(scene.create_actor("Actor"), "Added empty actor");
        scene.find(scene.selected_actor_id())->transform = spawn_transform(0.0f);
        break;
    case EditorCommandType::AddCube:
        record_undo();
        select_new(spawn_primitive(scene, engine_.assets(), BuiltinMesh::Cube, "Cube", spawn_transform(0.5f)), "Added cube");
        break;
    case EditorCommandType::AddSphere:
        record_undo();
        select_new(spawn_primitive(scene, engine_.assets(), BuiltinMesh::Sphere, "Sphere", spawn_transform(0.5f)), "Added sphere");
        break;
    case EditorCommandType::AddPlane: {
        record_undo();
        Transform t = spawn_transform(0.0f);
        t.scale = {5.0f, 1.0f, 5.0f};
        select_new(spawn_primitive(scene, engine_.assets(), BuiltinMesh::Plane, "Plane", t), "Added plane");
        break;
    }
    case EditorCommandType::AddDirectionalLight:
    case EditorCommandType::AddPointLight:
    case EditorCommandType::AddSpotLight: {
        record_undo();
        const LightKind kind = command.type == EditorCommandType::AddDirectionalLight ? LightKind::Directional
            : command.type == EditorCommandType::AddSpotLight                         ? LightKind::Spot
                                                                                      : LightKind::Point;
        Transform t = spawn_transform(2.0f);
        if (kind == LightKind::Directional) {
            t.rotation = {0.6f, -0.5f, 0.0f};
        }
        const std::string name = std::string(light_kind_name(kind)) + " Light";
        select_new(spawn_light(scene, kind, name, t), "Added " + name);
        break;
    }
    case EditorCommandType::AddCamera: {
        record_undo();
        Transform t;
        t.translation = camera_.position();
        t.rotation = rotation_for_forward(camera_.forward());
        select_new(spawn_camera(scene, "Camera", t), "Added camera at the current view");
        break;
    }
    case EditorCommandType::AddReflectionProbe:
    case EditorCommandType::AddIrradianceVolume:
    case EditorCommandType::AddDecal:
    case EditorCommandType::AddParticles:
    case EditorCommandType::AddTerrain:
    case EditorCommandType::AddWater: {
        record_undo();
        ComponentType type = ComponentType::ReflectionProbe;
        const char* name = "Reflection Probe";
        float lift = 2.0f;
        switch (command.type) {
        case EditorCommandType::AddIrradianceVolume: type = ComponentType::IrradianceVolume; name = "Irradiance Volume"; break;
        case EditorCommandType::AddDecal: type = ComponentType::Decal; name = "Decal"; lift = 0.0f; break;
        case EditorCommandType::AddParticles: type = ComponentType::ParticleEmitter; name = "Particles"; lift = 0.5f; break;
        case EditorCommandType::AddTerrain: type = ComponentType::Terrain; name = "Terrain"; lift = 0.0f; break;
        case EditorCommandType::AddWater: type = ComponentType::Water; name = "Water"; lift = 0.0f; break;
        default: break;
        }
        Transform t = spawn_transform(lift);
        if (type == ComponentType::Terrain || type == ComponentType::Water) {
            t.translation = {0.0f, type == ComponentType::Water ? 0.6f : 0.0f, 0.0f};
        }
        if (type == ComponentType::Decal) {
            t.scale = {1.5f, 1.0f, 1.5f};
        }
        select_new(spawn_component_actor(scene, type, name, t), std::string("Added ") + name);
        break;
    }
    case EditorCommandType::BakeLighting:
        renderer_.request_bake();
        status_ = "Baking reflection probes and irradiance volumes";
        break;
    case EditorCommandType::ReloadGameModule:
        if (engine_.playing()) {
            log_warning("Stop Play mode to reload the game module");
        } else if (engine_.game_module().reload()) {
            refresh_script_params();
        } else {
            log_warning(std::format("Game module not reloaded: {}", engine_.game_module().path().empty() ? std::string("no module") : to_utf8(engine_.game_module().path()) + " missing"));
        }
        break;
    case EditorCommandType::ReloadShaders:
        renderer_.reload_shaders();
        status_ = "Shaders reloaded";
        break;
    case EditorCommandType::Reparent: {
        const ActorId child = command.actor;
        const ActorId parent = command.target;
        if (child == parent || (parent != kNoActor && scene.is_ancestor(child, parent))) {
            log_warning("Can't parent an actor under itself or one of its children");
            break;
        }
        const Actor* actor = scene.find(child);
        if (actor == nullptr || actor->parent() == parent) {
            break;
        }
        record_undo();
        scene.set_parent(child, parent, true);
        break;
    }
    case EditorCommandType::AddComponent:
        add_component(command.actor, static_cast<ComponentType>(command.value));
        break;
    case EditorCommandType::RemoveComponent:
        if (Actor* actor = scene.find(command.actor)) {
            record_undo();
            actor->remove_component(static_cast<std::size_t>(command.value));
        }
        break;
    case EditorCommandType::Play:
        start_play();
        break;
    case EditorCommandType::Pause:
        if (engine_.playing()) {
            engine_.set_paused(!engine_.paused());
        }
        break;
    case EditorCommandType::Step:
        engine_.request_single_step();
        break;
    case EditorCommandType::Stop:
        stop_play();
        break;
    case EditorCommandType::FocusSelected:
        focus_selected();
        break;
    case EditorCommandType::ResetCamera:
        camera_ = Camera{};
        break;
    }
}

bool App::confirm_discard_changes() {
    if (!dirty_) {
        return true;
    }
    const std::wstring name = scene_path_.empty() ? L"Untitled" : scene_path_.filename().wstring();
    const std::wstring message = L"Save changes to " + name + L"?";
    const int answer = MessageBoxW(window_.hwnd(), message.c_str(), kWindowTitle, MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDYES) {
        return save_scene(false);
    }
    return answer == IDNO;
}

void App::new_scene(bool demo) {
    if (engine_.playing()) {
        stop_play();
    }
    if (!confirm_discard_changes()) {
        return;
    }
    if (demo) {
        build_demo_scene(engine_.scene(), engine_.assets());
        camera_ = Camera{};
        camera_.target = {0.0f, 0.5f, 0.0f};
        camera_.distance = 16.0f;
        camera_.pitch = 0.5f;
        status_ = "Demo scene: press Play (Ctrl+P), then WASD + Space to move the blue ball";
    } else {
        build_default_scene(engine_.scene(), engine_.assets());
        camera_ = Camera{};
        status_ = "New scene";
    }
    engine_.render_settings() = RenderPipelineSettings{};
    engine_.physics().settings = PhysicsSettings{};
    if (!default_environment_.empty() && renderer_.environment_path() != default_environment_) {
        load_environment(path_from_utf8(default_environment_), false);
    }
    scene_path_.clear();
    undo_.clear();
    dirty_ = false;
    log_info(status_);
}

// Scripts may have gained parameters after a game-module reload.
void App::refresh_script_params() {
    for (const auto& actor : engine_.scene().actors()) {
        for (const auto& component : actor->components()) {
            if (component->type() == ComponentType::Script) {
                ScriptRegistry::instance().fill_missing_params(static_cast<ScriptComponent&>(*component));
            }
        }
    }
}

void App::new_showcase_scene() {
    if (engine_.playing()) {
        stop_play();
    }
    if (!confirm_discard_changes()) {
        return;
    }
    build_showcase_scene(engine_.scene(), engine_.assets());
    RenderPipelineSettings settings;
    settings.sky.mode = SkyMode::Atmosphere;
    settings.fog.enabled = true;
    settings.fog.density = 0.012f;
    settings.fog.height_falloff = 0.12f;
    settings.post.bloom_intensity = 0.05f;
    engine_.render_settings() = settings;
    engine_.physics().settings = PhysicsSettings{};
    camera_ = Camera{};
    camera_.target = {0.0f, 1.6f, 3.0f};
    camera_.distance = 15.0f;
    camera_.pitch = 0.2f;
    camera_.yaw = 0.3f;
    scene_path_.clear();
    undo_.clear();
    dirty_ = false;
    status_ = "Showcase scene: terrain, water, materials, particles, probes and fog";
    log_info(status_);
}

void App::open_scene_dialog() {
    if (engine_.playing()) {
        stop_play();
    }
    if (!confirm_discard_changes()) {
        return;
    }
    if (auto path = window_.open_file_dialog(kSceneFilter, L"Open scene"); !path.empty()) {
        open_scene(path);
    }
}

bool App::open_scene(const std::filesystem::path& path) {
    try {
        const SceneDocument document = load_scene_file(path, engine_.scene(), engine_.assets());
        camera_ = document.camera;
        engine_.render_settings() = document.settings;
        engine_.physics().settings = document.physics;
        engine_.physics().reset();
        if (!document.environment_path.empty()) {
            load_environment(path_from_utf8(document.environment_path), false);
        }
        scene_path_ = path;
        undo_.clear();
        dirty_ = false;
        status_ = "Opened " + to_utf8(path.filename());
        log_info(std::format("Opened scene '{}' ({} actors)", to_utf8(path), engine_.scene().actor_count()));
        return true;
    } catch (const std::exception& error) {
        status_ = std::string("Open failed: ") + error.what();
        log_error(std::format("Could not open '{}': {}", to_utf8(path), error.what()));
        return false;
    }
}

bool App::save_scene(bool save_as) {
    if (engine_.playing()) {
        log_warning("Stop Play mode before saving (play-time changes are discarded on Stop)");
        return false;
    }
    std::filesystem::path path = scene_path_;
    if (save_as || path.empty()) {
        path = window_.save_file_dialog(kSceneFilter, L"Save scene", L"hscene");
        if (path.empty()) {
            return false;
        }
    }
    try {
        SceneDocument document;
        document.environment_path = renderer_.environment_path();
        document.camera = camera_;
        document.settings = engine_.render_settings();
        document.physics = engine_.physics().settings;
        save_scene_file(path, engine_.scene(), document);
        scene_path_ = path;
        dirty_ = false;
        status_ = "Saved " + to_utf8(path.filename());
        log_info(std::format("Saved scene '{}'", to_utf8(path)));
        return true;
    } catch (const std::exception& error) {
        status_ = std::string("Save failed: ") + error.what();
        log_error(status_);
        MessageBoxW(window_.hwnd(), utf8_to_wide(status_).c_str(), kWindowTitle, MB_ICONERROR);
        return false;
    }
}

void App::open_file(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    if (ext == ".hscene") {
        if (engine_.playing()) {
            stop_play();
        }
        if (confirm_discard_changes()) {
            open_scene(path);
        }
    } else if (ext == ".hdr") {
        load_environment(path);
        dirty_ = true;
    } else if (is_supported_model_file(path)) {
        import_model(path);
    } else {
        log_warning(std::format("Don't know how to open '{}' (supported: .hscene, .hdr, {})", to_utf8(path.filename()), supported_import_extensions()));
    }
}

void App::import_model(const std::filesystem::path& path) {
    try {
        ModelHandle model = engine_.assets().load_model(path);
        record_undo();
        Transform transform;
        // On the ground plane under the camera's focus point; models keep
        // their own pivot so modular kit pieces line up.
        transform.translation = {camera_.target.x, 0.0f, camera_.target.z};
        if (snap_) {
            transform.translation.x = std::round(transform.translation.x / snap_translate_) * snap_translate_;
            transform.translation.z = std::round(transform.translation.z / snap_translate_) * snap_translate_;
        }
        // Files authored in centimeters (or kilometers) would be unusable at
        // 1 unit = 1 meter; scale those to a sensible size on the actor
        // (the mesh data itself is left untouched).
        const float extent = max_component(model->bounds.extents()) * 2.0f;
        if (extent > 100.0f || (extent > 0.0f && extent < 0.02f)) {
            const float factor = 2.5f / extent;
            transform.scale = {factor, factor, factor};
            log_info(std::format("'{}' is {:.3g} units across; scaled the actor by {:.3g}", to_utf8(path.filename()), extent, factor));
        }
        Actor& actor = spawn_model(engine_.scene(), model, to_utf8(path.stem()), transform);
        select_new(actor, std::format("Imported {} ({} part(s))", to_utf8(path.filename()), model->parts.size()));
    } catch (const std::exception& error) {
        status_ = std::string("Import failed: ") + error.what();
        log_error(std::format("Import of '{}' failed: {}", to_utf8(path), error.what()));
    }
}

void App::load_environment(const std::filesystem::path& path, bool use_as_sky) {
    if (!renderer_.ready()) {
        return;
    }
    try {
        renderer_.load_environment(to_utf8(path));
        if (use_as_sky) {
            engine_.render_settings().sky.mode = SkyMode::Hdri;
        }
        status_ = "Environment: " + to_utf8(path.filename());
    } catch (const std::exception& error) {
        status_ = std::string("Environment load failed: ") + error.what();
        log_error(status_);
    }
}

Transform App::spawn_transform(float lift) const {
    Transform t;
    t.translation = camera_.target + Vec3{0.0f, lift, 0.0f};
    if (snap_) {
        t.translation.x = std::round(t.translation.x / snap_translate_) * snap_translate_;
        t.translation.z = std::round(t.translation.z / snap_translate_) * snap_translate_;
    }
    return t;
}

void App::select_new(Actor& actor, const std::string& message) {
    engine_.scene().select_actor(actor.id());
    engine_.scene().update_world_transforms();
    status_ = message;
}

void App::add_component(ActorId id, ComponentType type) {
    Actor* actor = engine_.scene().find(id);
    if (actor == nullptr || (!component_allows_multiple(type) && actor->has_component(type))) {
        return;
    }
    record_undo();
    switch (type) {
    case ComponentType::StaticMesh: {
        auto& mesh = actor->add_component<StaticMeshComponent>();
        mesh.parts.push_back({"Cube", engine_.assets().builtin_mesh(BuiltinMesh::Cube), Material{}});
        break;
    }
    case ComponentType::Light: {
        auto& light = actor->add_component<LightComponent>();
        light.apply_defaults_for_kind();
        break;
    }
    case ComponentType::Camera:
        actor->add_component<CameraComponent>();
        break;
    case ComponentType::RigidBody:
        actor->add_component<RigidBodyComponent>();
        if (!actor->has_component(ComponentType::Collider)) {
            fit_collider_to_mesh(*actor, actor->add_component<ColliderComponent>());
        }
        break;
    case ComponentType::Collider: {
        auto& collider = actor->add_component<ColliderComponent>();
        fit_collider_to_mesh(*actor, collider);
        break;
    }
    case ComponentType::Script:
        actor->add_component<ScriptComponent>();
        break;
    default:
        if (auto component = create_component(type)) {
            actor->add_component(std::move(component));
        }
        break;
    }
}

void App::duplicate_selected() {
    Scene& scene = engine_.scene();
    const ActorId selected = scene.selected_actor_id();
    if (selected == kNoActor) {
        return;
    }
    record_undo();
    const ActorId copy = scene.duplicate_actor(selected);
    if (Actor* actor = scene.find(copy)) {
        // Offset so the copy is visible instead of exactly overlapping.
        actor->transform.translation += Vec3{snap_ ? snap_translate_ : 0.5f, 0.0f, 0.0f};
        scene.select_actor(copy);
        status_ = "Duplicated as " + actor->name();
    }
}

void App::delete_selected() {
    Scene& scene = engine_.scene();
    const ActorId selected = scene.selected_actor_id();
    if (selected == kNoActor) {
        return;
    }
    end_drag(true);
    record_undo();
    const std::string name = scene.find(selected)->name();
    scene.destroy_actor(selected);
    status_ = "Deleted " + name;
}

void App::start_play() {
    if (engine_.playing()) {
        return;
    }
    end_drag(true);
    play_snapshot_ = snapshot();
    edit_camera_ = camera_;
    engine_.begin_play();
    status_ = "Playing - Esc or Stop to return to the editor";
}

void App::stop_play() {
    if (!engine_.playing()) {
        return;
    }
    end_drag(true);
    engine_.end_play();
    if (play_snapshot_) {
        restore(*play_snapshot_);
        play_snapshot_.reset();
    }
    camera_ = edit_camera_;
    game_camera_active_ = false;
    status_ = "Stopped - scene restored";
}

void App::focus_selected() {
    Scene& scene = engine_.scene();
    const Actor* actor = scene.selected_actor();
    if (actor == nullptr) {
        return;
    }
    const Mat4 world = scene.compute_world_matrix(actor->id());
    Vec3 center = world.translation_part();
    float radius = 1.0f;
    if (const auto* mesh = actor->find_component<StaticMeshComponent>(); mesh != nullptr && !mesh->parts.empty()) {
        const Aabb bounds = transform_aabb(mesh->local_bounds(), world);
        center = bounds.center();
        radius = std::max(length(bounds.extents()), 0.25f);
    }
    camera_.target = center;
    camera_.distance = std::clamp(radius * 2.8f, 1.5f, 150.0f);
}

// ---------------------------------------------------------------------------
// Picking & gizmos

ActorId App::pick_actor(int x, int y) const {
    const Scene& scene = engine_.scene();
    const Ray ray = camera_.pick_ray(static_cast<float>(x), static_cast<float>(y), viewport_width(), viewport_height());
    ActorId best_id = kNoActor;
    float best = 3.0e38f;

    for (const auto& actor_ptr : scene.actors()) {
        const Actor& actor = *actor_ptr;
        if (!effectively_active(scene, actor)) {
            continue;
        }
        const Mat4 world = scene.compute_world_matrix(actor.id());
        if (actor.find_component<LightComponent>() != nullptr || actor.find_component<CameraComponent>() != nullptr) {
            const float t = ray_sphere(ray, world.translation_part(), 0.18f * camera_.gizmo_scale(world.translation_part()));
            if (t >= 0.0f && t < best) {
                best = t;
                best_id = actor.id();
            }
        }
        const auto* mesh = actor.find_component<StaticMeshComponent>();
        if (mesh == nullptr || !mesh->visible) {
            continue;
        }
        // Test in the actor's local space: bounding box first, then the
        // actual triangles, so clicks are pixel-accurate. The local ray
        // direction isn't normalized, which keeps t in world units.
        const Mat4 inverse_world = inverse(world);
        const Ray local{transform_point(inverse_world, ray.origin), transform_vector(inverse_world, ray.direction)};
        for (const MeshPart& part : mesh->parts) {
            if (!part.mesh) {
                continue;
            }
            const float box_t = ray_aabb(local, part.mesh->bounds);
            if (box_t < 0.0f || box_t >= best) {
                continue;
            }
            const auto& vertices = part.mesh->vertices;
            const auto& indices = part.mesh->indices;
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                const float t = ray_triangle(local, vertices[indices[i]].position, vertices[indices[i + 1]].position, vertices[indices[i + 2]].position);
                if (t >= 0.0f && t < best) {
                    best = t;
                    best_id = actor.id();
                }
            }
        }
    }
    return best_id;
}

float App::gizmo_length(Vec3 origin) const {
    return (tool_ == EditorTool::Scale ? 1.15f : 1.35f) * camera_.gizmo_scale(origin);
}

Vec3 App::gizmo_axis(DragMode mode, const Mat4& world) const {
    const Vec3 axis = base_axis(mode);
    if (space_ == TransformSpace::World && !is_scale(mode)) {
        return axis;
    }
    return normalize(transform_vector(world, axis));
}

DragMode App::pick_gizmo(int x, int y) const {
    const Scene& scene = engine_.scene();
    const Actor* actor = scene.selected_actor();
    if (actor == nullptr || tool_ == EditorTool::Select || !view_.gizmos || game_camera_active_) {
        return DragMode::None;
    }
    const Mat4 world = scene.compute_world_matrix(actor->id());
    const Vec3 origin = world.translation_part();
    const Vec2 mouse{static_cast<float>(x), static_cast<float>(y)};
    const Vec2 center = renderer_.project(origin, camera_);
    if (tool_ != EditorTool::Rotate && distance_2d(mouse, center) < 14.0f) {
        return tool_ == EditorTool::Scale ? DragMode::ScaleCenter : DragMode::MoveCenter;
    }
    const float len = gizmo_length(origin);
    float best_score = 1.0e9f;
    DragMode best_mode = DragMode::None;
    const std::array<DragMode, 3> modes = axis_modes(tool_);
    for (int i = 0; i < 3; ++i) {
        const DragMode mode = modes[static_cast<std::size_t>(i)];
        if (tool_ == EditorTool::Rotate) {
            // Ring in the plane of the two other axes.
            const Vec3 a = gizmo_axis(axis_modes(EditorTool::Move)[static_cast<std::size_t>((i + 1) % 3)], world);
            const Vec3 b = gizmo_axis(axis_modes(EditorTool::Move)[static_cast<std::size_t>((i + 2) % 3)], world);
            constexpr int segments = 64;
            for (int s = 0; s < segments; ++s) {
                const float t0 = static_cast<float>(s) / segments * 2.0f * kPi;
                const float t1 = static_cast<float>(s + 1) / segments * 2.0f * kPi;
                const Vec2 p0 = renderer_.project(origin + a * (std::cos(t0) * len) + b * (std::sin(t0) * len), camera_);
                const Vec2 p1 = renderer_.project(origin + a * (std::cos(t1) * len) + b * (std::sin(t1) * len), camera_);
                const float score = distance_to_segment_2d(mouse, p0, p1);
                if (score < best_score) {
                    best_score = score;
                    best_mode = mode;
                }
            }
        } else {
            const Vec2 tip = renderer_.project(origin + gizmo_axis(mode, world) * len, camera_);
            const float score = distance_to_segment_2d(mouse, center, tip);
            if (score < best_score) {
                best_score = score;
                best_mode = mode;
            }
        }
    }
    const float threshold = tool_ == EditorTool::Rotate ? 12.0f : 14.0f;
    return best_score <= threshold ? best_mode : DragMode::None;
}

void App::begin_drag(DragMode mode) {
    Scene& scene = engine_.scene();
    const Actor* actor = scene.selected_actor();
    if (actor == nullptr) {
        return;
    }
    drag_mode_ = mode;
    drag_actor_ = actor->id();
    drag_start_local_ = actor->transform;
    drag_start_world_matrix_ = scene.compute_world_matrix(actor->id());
    drag_start_world_ = actor->parent() == kNoActor ? actor->transform : decompose(drag_start_world_matrix_);
    drag_parent_inverse_ = inverse(scene.compute_parent_world_matrix(actor->id()));
    drag_start_x_ = mouse_x();
    drag_start_y_ = mouse_y();
    if (!engine_.playing()) {
        drag_snapshot_ = snapshot();
    }
}

float App::screen_axis_delta(DragMode mode, float dx, float dy) const {
    // Mouse motion projected onto the axis' on-screen direction, converted
    // to world units along the axis.
    const Vec3 origin = drag_start_world_.translation;
    const Vec3 axis = gizmo_axis(mode, drag_start_world_matrix_);
    const Vec2 center = renderer_.project(origin, camera_);
    const Vec2 tip = renderer_.project(origin + axis, camera_);
    const Vec2 direction{tip.x - center.x, tip.y - center.y};
    const float pixels_per_unit = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (pixels_per_unit < 2.0f) {
        // Axis points almost straight at the camera.
        return (std::abs(dx) > std::abs(dy) ? dx : -dy) * camera_.world_units_per_pixel(origin, viewport_height());
    }
    return (dx * direction.x + dy * direction.y) / (pixels_per_unit * pixels_per_unit);
}

float App::screen_ring_angle(DragMode mode, int x, int y) const {
    const Vec3 origin = drag_start_world_.translation;
    const Vec2 center = renderer_.project(origin, camera_);
    const float start = std::atan2(static_cast<float>(drag_start_y_) - center.y, static_cast<float>(drag_start_x_) - center.x);
    const float current = std::atan2(static_cast<float>(y) - center.y, static_cast<float>(x) - center.x);
    float delta = current - start;
    while (delta > kPi) delta -= 2.0f * kPi;
    while (delta < -kPi) delta += 2.0f * kPi;
    // Screen angles grow clockwise (y down); flip when the axis faces the
    // camera so the ring follows the mouse.
    const Vec3 axis = gizmo_axis(mode, drag_start_world_matrix_);
    return dot(axis, camera_.forward()) > 0.0f ? delta : -delta;
}

void App::update_drag() {
    Scene& scene = engine_.scene();
    Actor* actor = scene.find(drag_actor_);
    if (actor == nullptr) {
        end_drag(true);
        return;
    }
    const float dx = static_cast<float>(mouse_x() - drag_start_x_);
    const float dy = static_cast<float>(mouse_y() - drag_start_y_);
    const Vec3 origin = drag_start_world_.translation;
    Transform local = drag_start_local_;

    if (is_move(drag_mode_)) {
        Vec3 position = origin;
        if (drag_mode_ == DragMode::MoveCenter) {
            const float units = camera_.world_units_per_pixel(origin, viewport_height());
            position = origin + camera_.right() * (dx * units) - camera_.up() * (dy * units);
        } else {
            position = origin + gizmo_axis(drag_mode_, drag_start_world_matrix_) * screen_axis_delta(drag_mode_, dx, dy);
        }
        if (snap_ && snap_translate_ > 0.0f) {
            auto snap = [this](float v) { return std::round(v / snap_translate_) * snap_translate_; };
            if (drag_mode_ == DragMode::MoveCenter) {
                position.x = snap(position.x);
                position.z = snap(position.z);
            } else {
                position = {snap(position.x), snap(position.y), snap(position.z)};
                // Keep axis drags on their axis when the start wasn't snapped.
                const Vec3 axis = gizmo_axis(drag_mode_, drag_start_world_matrix_);
                position = origin + axis * dot(position - origin, axis);
            }
        }
        local.translation = transform_point(drag_parent_inverse_, position);
    } else if (is_rotate(drag_mode_)) {
        float angle = screen_ring_angle(drag_mode_, mouse_x(), mouse_y());
        if (snap_ && snap_rotate_degrees_ > 0.0f) {
            const float step = snap_rotate_degrees_ * kDegToRad;
            angle = std::round(angle / step) * step;
        }
        const Vec3 axis = gizmo_axis(drag_mode_, drag_start_world_matrix_);
        const Mat4 world_rotation = Mat4::rotation_axis_angle(axis, angle) * Mat4::rotation_xyz(drag_start_world_.rotation);
        if (actor->parent() == kNoActor) {
            local.rotation = euler_from_rotation_xyz(world_rotation);
        } else {
            Transform world = drag_start_world_;
            world.rotation = euler_from_rotation_xyz(world_rotation);
            local.rotation = decompose(drag_parent_inverse_ * world.matrix()).rotation;
        }
    } else if (drag_mode_ == DragMode::ScaleCenter) {
        const float factor = std::max(0.02f, 1.0f + (std::abs(dx) > std::abs(dy) ? dx : -dy) * 0.008f);
        local.scale = drag_start_local_.scale * factor;
    } else if (is_scale(drag_mode_)) {
        // Dragging by one gizmo length doubles the size along that axis.
        const float factor = std::max(0.02f, 1.0f + screen_axis_delta(drag_mode_, dx, dy) / std::max(gizmo_length(origin), 0.001f));
        if (drag_mode_ == DragMode::ScaleX) local.scale.x = drag_start_local_.scale.x * factor;
        if (drag_mode_ == DragMode::ScaleY) local.scale.y = drag_start_local_.scale.y * factor;
        if (drag_mode_ == DragMode::ScaleZ) local.scale.z = drag_start_local_.scale.z * factor;
    }
    actor->transform = local;
}

void App::end_drag(bool cancel) {
    if (drag_mode_ == DragMode::None) {
        return;
    }
    Actor* actor = engine_.scene().find(drag_actor_);
    if (actor != nullptr) {
        if (cancel) {
            actor->transform = drag_start_local_;
        } else {
            const Transform& t = actor->transform;
            const bool changed = t.translation != drag_start_local_.translation || t.rotation != drag_start_local_.rotation || t.scale != drag_start_local_.scale;
            if (changed && drag_snapshot_) {
                undo_.record(std::move(*drag_snapshot_));
                dirty_ = true;
            }
        }
    }
    drag_snapshot_.reset();
    drag_mode_ = DragMode::None;
    drag_actor_ = kNoActor;
}

// ---------------------------------------------------------------------------
// Capture mode (turntable + skybox playlist)

void App::update_turntable(float delta) {
    if (!showcase_.turntable_enabled || game_camera_active_) {
        return;
    }
    if (const Actor* selected = engine_.scene().selected_actor()) {
        camera_.target = engine_.scene().compute_world_matrix(selected->id()).translation_part();
    }
    camera_.yaw += showcase_.turntable_radians_per_second * delta;
}

void App::update_showcase(float delta) {
    if (showcase_.request_rescan) {
        showcase_.request_rescan = false;
        rescan_skybox_playlist();
    }
    if (showcase_.request_next) {
        showcase_.request_next = false;
        advance_skybox(1);
    }
    if (showcase_.request_previous) {
        showcase_.request_previous = false;
        advance_skybox(-1);
    }
    if (showcase_.request_index != ShowcaseState::kNoRequest) {
        const std::size_t requested = showcase_.request_index;
        showcase_.request_index = ShowcaseState::kNoRequest;
        apply_skybox(requested);
    }
    // The automatic timer only runs with the turntable, so the sky doesn't
    // change under you while editing.
    if (!showcase_.turntable_enabled || !showcase_.skybox_cycle_enabled) {
        showcase_.seconds_on_current = 0.0f;
        return;
    }
    if (showcase_.playlist.size() < 2) {
        return;
    }
    showcase_.seconds_on_current += delta;
    if (showcase_.seconds_on_current >= std::max(0.5f, showcase_.skybox_cycle_seconds)) {
        advance_skybox(1);
    }
}

void App::process_skybox_preload() {
    // At most one bake per frame, so the startup cost is spread out.
    if (showcase_.preload_complete() || !renderer_.ready()) {
        return;
    }
    const std::size_t index = showcase_.preload_cursor++;
    const std::string path = to_utf8(showcase_.playlist[index]);
    if (renderer_.is_environment_cached(path)) {
        return;
    }
    try {
        renderer_.preload_environment(path);
    } catch (const std::exception& error) {
        log_warning(std::format("Skybox preload failed ({}): {}", to_utf8(showcase_.playlist[index].filename()), error.what()));
    }
}

void App::rescan_skybox_playlist() {
    showcase_.playlist.clear();
    showcase_.preload_cursor = 0;
    showcase_.seconds_on_current = 0.0f;
    showcase_.playlist_index = 0;

    std::error_code ec;
    if (!std::filesystem::is_directory(showcase_.directory, ec)) {
        log_warning("Skybox folder not found: " + to_utf8(showcase_.directory));
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(showcase_.directory, ec)) {
        if (entry.is_regular_file(ec) && lower_extension(entry.path()) == ".hdr") {
            showcase_.playlist.push_back(entry.path());
        }
    }
    std::sort(showcase_.playlist.begin(), showcase_.playlist.end());
    const std::string active = renderer_.environment_path();
    for (std::size_t i = 0; i < showcase_.playlist.size(); ++i) {
        if (to_utf8(showcase_.playlist[i]) == active) {
            showcase_.playlist_index = i;
            break;
        }
    }
}

void App::advance_skybox(int offset) {
    if (showcase_.playlist.empty()) {
        return;
    }
    const auto count = static_cast<int>(showcase_.playlist.size());
    const int next = ((static_cast<int>(showcase_.playlist_index) + offset) % count + count) % count;
    apply_skybox(static_cast<std::size_t>(next));
}

void App::apply_skybox(std::size_t index) {
    if (index >= showcase_.playlist.size()) {
        return;
    }
    const std::filesystem::path& path = showcase_.playlist[index];
    showcase_.playlist_index = index;
    showcase_.seconds_on_current = 0.0f;
    // Fast path: already baked -> a handle swap that can't stall a capture.
    if (renderer_.activate_environment(to_utf8(path))) {
        status_ = "Skybox: " + to_utf8(path.filename());
        return;
    }
    load_environment(path);
}

} // namespace Hollow

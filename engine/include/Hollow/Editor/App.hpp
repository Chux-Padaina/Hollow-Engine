// App is the editor shell around the Engine: it owns the window, renderer
// and editor UI, runs the main loop, and turns input into editor actions
// (camera orbit/pan/zoom, picking, gizmo drags, shortcuts, drag-and-drop
// import). It also owns the editor-only state: the open scene file and its
// dirty flag, the undo stack, the Play-mode snapshot that Stop restores,
// and the capture/showcase mode (turntable + skybox playlist).
#pragma once

#include "Hollow/Core/Engine.hpp"
#include "Hollow/Core/Json.hpp"
#include "Hollow/Editor/EditorUi.hpp"
#include "Hollow/Platform/Win32Window.hpp"
#include "Hollow/Render/Camera.hpp"
#include "Hollow/Render/Renderer.hpp"
#include "Hollow/Scene/UndoStack.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Hollow {

enum class DragMode {
    None,
    MoveCenter,
    MoveX,
    MoveY,
    MoveZ,
    RotateX,
    RotateY,
    RotateZ,
    ScaleCenter,
    ScaleX,
    ScaleY,
    ScaleZ,
};

class App {
public:
    // `startup_files` may contain models, .hdr environments or a .hscene.
    int run(const std::vector<std::filesystem::path>& startup_files = {});

private:
    // Main loop pieces
    void handle_shortcuts();
    void handle_viewport_mouse(float delta);
    // Mouse position relative to the viewport card, and its size.
    int mouse_x() const;
    int mouse_y() const;
    int viewport_width() const;
    int viewport_height() const;
    void update_play_camera();
    void update_window_title();
    RenderView make_render_view() const;

    // Undo / dirty tracking
    JsonValue snapshot() const;
    void restore(const JsonValue& state);
    void begin_edit();
    void end_edit();
    void cancel_edit();
    void record_undo(); // snapshot now, before an immediate edit
    void undo();
    void redo();

    // Commands
    void execute(const EditorCommand& command);
    bool confirm_discard_changes();
    void new_scene(bool demo);
    void new_showcase_scene();
    void refresh_script_params();
    void open_scene_dialog();
    bool open_scene(const std::filesystem::path& path);
    bool save_scene(bool save_as);
    void open_file(const std::filesystem::path& path);
    void import_model(const std::filesystem::path& path);
    // `use_as_sky` switches the sky to HDRI mode.
    void load_environment(const std::filesystem::path& path, bool use_as_sky = true);
    Transform spawn_transform(float lift) const;
    void select_new(Actor& actor, const std::string& message);
    void add_component(ActorId actor, ComponentType type);
    void duplicate_selected();
    void delete_selected();
    void start_play();
    void stop_play();
    void focus_selected();

    // Picking & gizmos (positions are in pixels, y down)
    ActorId pick_actor(int x, int y) const;
    DragMode pick_gizmo(int x, int y) const;
    void begin_drag(DragMode mode);
    void update_drag();
    void end_drag(bool cancel);
    Vec3 gizmo_axis(DragMode mode, const Mat4& world) const;
    float gizmo_length(Vec3 origin) const;
    float screen_axis_delta(DragMode mode, float dx, float dy) const;
    float screen_ring_angle(DragMode mode, int x, int y) const;

    // Capture mode
    void update_turntable(float delta);
    void update_showcase(float delta);
    void process_skybox_preload();
    void rescan_skybox_playlist();
    void advance_skybox(int offset);
    void apply_skybox(std::size_t index);

    Engine engine_;
    Renderer renderer_;
    EditorUi editor_ui_;
    Win32Window window_;
    Camera camera_;
    Camera edit_camera_; // restored when Play stops
    ViewportRect viewport_;
    std::chrono::steady_clock::time_point last_frame_;
    float frame_ms_ = 0.0f;

    // Editor view state
    EditorTool tool_ = EditorTool::Move;
    TransformSpace space_ = TransformSpace::World;
    DebugView debug_view_ = DebugView::Final;
    EditorViewOptions view_;
    bool snap_ = false;
    float snap_translate_ = 0.5f;
    float snap_rotate_degrees_ = 15.0f;
    bool ui_visible_ = true;
    bool game_camera_active_ = false;

    // Scene file / undo
    std::filesystem::path scene_path_;
    bool dirty_ = false;
    UndoStack undo_;
    std::optional<JsonValue> pending_edit_;
    std::optional<JsonValue> play_snapshot_;
    std::string default_environment_;

    // Gizmo drag
    DragMode drag_mode_ = DragMode::None;
    ActorId drag_actor_ = kNoActor;
    Transform drag_start_world_;
    Transform drag_start_local_;
    Mat4 drag_start_world_matrix_ = Mat4::identity();
    Mat4 drag_parent_inverse_ = Mat4::identity();
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;
    std::optional<JsonValue> drag_snapshot_;

    ShowcaseState showcase_;
    std::string status_ = "Drop a model (.glb/.gltf/.obj), an .hdr environment, or a .hscene onto the window";
};

} // namespace Hollow

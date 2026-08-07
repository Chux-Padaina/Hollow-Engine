// App is the engine's top-level owner and main-loop driver: it creates the
// window/renderer/scene, ticks the Engine each frame, and turns raw window
// input into editor actions (camera orbit/pan, gizmo dragging, ray-based
// actor picking, drag-and-drop model/HDR import). It also loads the bundled
// default environment on startup so a fresh checkout renders lit immediately.
// EditorUi owns and draws the Dear ImGui panels; App just feeds it state each
// frame and applies back whatever EditorCommand it returns.
#pragma once

#include "Hollow/EditorUi.hpp"
#include "Hollow/Engine.hpp"
#include "Hollow/Renderer.hpp"
#include "Hollow/Scene.hpp"
#include "Hollow/Win32Window.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Hollow {

enum class DragMode {
    None,
    CameraOrbit,
    CameraPan,
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
    int run(const std::vector<std::filesystem::path>& startup_imports = {});

private:
    void begin_frame();
    void handle_keyboard();
    void handle_mouse();
    void execute_editor_command(EditorCommand command);
    void begin_drag(DragMode mode);
    void drag_selected_actor(float dx, float dy);
    void add_mesh(MeshPrimitive primitive);
    void add_light(LightKind kind);
    void reset_scene();
    void import_model(const std::filesystem::path& path);
    void import_pending_drops();
    ActorId pick_actor(int x, int y) const;
    DragMode pick_gizmo(int x, int y) const;
    Vec3 gizmo_axis(DragMode mode, const Transform& transform) const;
    Vec3 base_axis_for_mode(DragMode mode) const;
    DragMode axis_mode_for_tool(DragMode move_axis_mode) const;
    void ring_basis_for_mode(DragMode rotate_mode, const Transform& transform, Vec3& a, Vec3& b) const;
    float screen_axis_drag_delta(DragMode axis_mode, const Transform& transform, float dx, float dy) const;
    float screen_ring_drag_delta(DragMode rotate_mode, const Transform& transform, int current_x, int current_y) const;
    void update_window_title();

    Engine engine_;
    Scene scene_ = make_default_scene();
    Renderer renderer_;
    EditorUi editor_ui_;
    Win32Window window_;
    Camera camera_;
    std::chrono::steady_clock::time_point last_frame_;
    DragMode drag_mode_ = DragMode::None;
    Transform drag_start_transform_;
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;
    bool snap_to_grid_ = false;
    std::string status_ = "Drop a .glb/.gltf/.obj to import, or a .hdr to set the environment";
};

} // namespace Hollow

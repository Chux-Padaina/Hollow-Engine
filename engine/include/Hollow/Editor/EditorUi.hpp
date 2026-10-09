// EditorUi draws the Dear ImGui editor around the viewport:
//
//   top bar      logo, menus (File/Edit/Add/View/Render), play controls,
//                undo/redo/save
//   outliner     hierarchy with search and drag-and-drop parenting
//                (collapses to a rail)
//   inspector    Details (transform + every component), Render (all render
//                settings, sky, baking) and Stats (frame/GPU timings per
//                render-graph pass, resources, scene and physics counts)
//   console      engine log (collapses to its header)
//   viewport     the renderer's output as a rounded card, with floating
//                tool/view controls
//
// The look (dark greys, neon lime, rounded cards, animated collapsing
// sections) comes from src/Editor/EditorTheme. Structural edits are
// reported back to App as EditorCommands; widget edits are bracketed by
// the context's begin_edit/end_edit callbacks for undo.
#pragma once

#include "Hollow/Core/Engine.hpp"
#include "Hollow/Platform/Win32Window.hpp"
#include "Hollow/Render/Camera.hpp"
#include "Hollow/Render/Renderer.hpp"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Hollow {

// Capture mode: an auto-orbiting turntable camera plus an optional skybox
// playlist that advances on a timer while it runs (see App::update_showcase).
struct ShowcaseState {
    bool turntable_enabled = false;
    float turntable_radians_per_second = 0.5f;

    bool skybox_cycle_enabled = true;
    float skybox_cycle_seconds = 4.0f;
    std::filesystem::path directory;
    std::vector<std::filesystem::path> playlist;
    std::size_t playlist_index = 0;
    float seconds_on_current = 0.0f;
    std::size_t preload_cursor = 0;

    bool request_next = false;
    bool request_previous = false;
    bool request_rescan = false;
    static constexpr std::size_t kNoRequest = static_cast<std::size_t>(-1);
    std::size_t request_index = kNoRequest;

    bool preload_complete() const { return preload_cursor >= playlist.size(); }
};

// Which editor overlays are visible.
struct EditorViewOptions {
    bool grid = true;
    bool gizmos = true;
    bool outlines = true;
    bool icons = true;
    bool colliders = false; // every collider (the selection's always shows)
};

enum class EditorCommandType {
    None,
    NewScene,
    NewDemoScene,
    NewShowcaseScene,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    ImportModel,
    LoadEnvironment,
    Exit,
    Undo,
    Redo,
    Duplicate,
    Delete,
    AddEmpty,
    AddCube,
    AddPlane,
    AddSphere,
    AddDirectionalLight,
    AddPointLight,
    AddSpotLight,
    AddCamera,
    AddReflectionProbe,
    AddIrradianceVolume,
    AddDecal,
    AddParticles,
    AddTerrain,
    AddWater,
    Reparent,        // actor -> target (kNoActor = root)
    AddComponent,    // actor, value = ComponentType
    RemoveComponent, // actor, value = component index
    Play,
    Pause,
    Step,
    Stop,
    FocusSelected,
    ResetCamera,
    BakeLighting,
    ReloadShaders,
    ReloadGameModule,
};

struct EditorCommand {
    EditorCommandType type = EditorCommandType::None;
    ActorId actor = kNoActor;
    ActorId target = kNoActor;
    int value = 0;
};

struct EditorContext {
    Engine& engine;
    Camera& camera;
    Renderer& renderer;
    Win32Window& window;
    EditorTool& tool;
    TransformSpace& space;
    DebugView& debug_view;
    EditorViewOptions& view;
    bool& snap;
    float& snap_translate;
    float& snap_rotate_degrees;
    ShowcaseState& showcase;
    bool& ui_visible;
    bool can_undo = false;
    bool can_redo = false;
    bool dirty = false;
    std::string scene_name;
    std::string status;
    float frame_ms = 0.0f;
    float delta = 0.0f;
    bool game_view = false; // looking through a Play-mode camera
    // Undo bracketing for widget edits (see InspectorVisitor).
    std::function<void()> begin_edit;
    std::function<void()> end_edit;
    std::function<void()> cancel_edit;
    // Edits that aren't part of the scene snapshot (render settings).
    std::function<void()> mark_dirty;
};

struct ViewportRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

class EditorUi {
public:
    void initialize(Win32Window& window);
    void shutdown();
    void begin_frame(float delta_seconds);
    std::vector<EditorCommand> draw(EditorContext& context);
    // Clears the window and renders the UI draw lists.
    void render();
    bool wants_mouse() const;
    bool wants_keyboard() const;
    // Where the viewport card is (window pixels). Valid after draw().
    const ViewportRect& viewport() const { return viewport_; }

    bool show_console = true;

private:
    void draw_top_bar(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_menus(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_outliner(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_outliner_node(EditorContext& context, std::vector<EditorCommand>& commands, ActorId id, int depth);
    void draw_inspector(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_details(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_component(EditorContext& context, std::vector<EditorCommand>& commands, Actor& actor, std::size_t index);
    void draw_static_mesh(EditorContext& context, Actor& actor, StaticMeshComponent& mesh);
    void draw_script(EditorContext& context, ScriptComponent& script);
    void draw_render_settings(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_physics_settings(EditorContext& context);
    void draw_stats(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_console(EditorContext& context);
    void draw_viewport(EditorContext& context, std::vector<EditorCommand>& commands);
    void draw_showcase_window(EditorContext& context);
    // Drag handles in the gaps between panels (resize like other engines).
    void draw_splitters();
    void load_layout();
    void save_layout() const;
    bool matches_filter(const Scene& scene, ActorId id) const;

    bool initialized_ = false;
    Win32Window* window_ = nullptr;
    ViewportRect viewport_;
    // Panel state
    bool outliner_open_ = true;
    bool inspector_open_ = true;
    bool show_showcase_ = false;
    int inspector_tab_ = 0;
    float outliner_t_ = 1.0f;
    float inspector_t_ = 1.0f;
    float console_t_ = 1.0f;
    // Panel sizes in unscaled pixels (multiplied by ui_scale_), user-resizable.
    float outliner_size_ = 250.0f;
    float inspector_size_ = 340.0f;
    float console_size_ = 170.0f;
    int active_splitter_ = 0; // 1 outliner, 2 inspector, 3 console
    float splitter_start_size_ = 0.0f;  // size + mouse position when a drag began
    float splitter_start_mouse_ = 0.0f;
    int saved_open_flags_ = -1; // panel open states last written to the layout file
    std::string outliner_filter_;
    // Material editing on multi-part meshes
    int material_part_ = -1; // -1 = all parts
    ActorId material_part_actor_ = kNoActor;
    std::uint64_t console_revision_ = 0;
    bool console_show_info_ = true;
    bool console_show_warnings_ = true;
    bool console_show_errors_ = true;
    char showcase_directory_buffer_[512] = {};
    bool showcase_directory_synced_ = false;
    std::deque<float> frame_history_;
    float ui_scale_ = 1.0f;
    float delta_ = 1.0f / 60.0f;
    ActorId rename_actor_ = kNoActor;
};

} // namespace Hollow

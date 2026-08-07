// EditorUi owns and draws the Dear ImGui editor chrome: the toolbar, the
// scene outliner, the details/inspector panel, the render-graph and post-FX
// debug windows, and the "urban tech HUD" theme (a mostly-gray palette with
// two neon accents) and hand-drawn toolbar icons. It is driven once per
// frame by App::run, which passes in the live Scene/Engine/Camera state;
// EditorUi edits selection, transforms, and material/light properties on
// that state directly through the widgets, and reports higher-level actions
// (add/delete/reset) back to App as an EditorCommand for it to apply.
#pragma once

#include "Hollow/Engine.hpp"
#include "Hollow/Renderer.hpp"
#include "Hollow/Scene.hpp"
#include "Hollow/Win32Window.hpp"

namespace Hollow {

enum class EditorCommand {
    None,
    AddCube,
    AddPlane,
    AddSphere,
    AddDirectionalLight,
    AddPointLight,
    AddSpotLight,
    DeleteSelection,
    ResetScene,
};

class EditorUi {
public:
    void initialize(Win32Window& window);
    void shutdown();
    void begin_frame();
    EditorCommand draw(Scene& scene, Engine& engine, Camera& camera, bool& snap_to_grid, const std::string& status, const Renderer& renderer);
    void render();
    bool wants_mouse() const;
    bool wants_keyboard() const;

private:
    bool initialized_ = false;
    bool show_render_graph_ = false;
    bool show_post_fx_ = false;
    // Set in initialize(); used by the toolbar's Resolution combo to resize
    // the real OS window in place (Win32Window::set_client_size). Not
    // owned — the window outlives the UI.
    Win32Window* window_ = nullptr;
    // Cached width of the Details panel (right-aligned) from the previous
    // frame's Begin() call, since this frame's SetNextWindowPos needs the
    // panel's width (to anchor it to the right edge) before Begin() has run
    // and can report the user's live drag-resized width. One-frame-lag is
    // invisible in practice. Also used as the panel's ImGuiCond_FirstUseEver
    // initial width, so it now doubles as "default width" too.
    float details_panel_width_ = 322.0f;
};

} // namespace Hollow

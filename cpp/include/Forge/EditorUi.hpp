#pragma once

#include "Forge/Engine.hpp"
#include "Forge/Renderer.hpp"
#include "Forge/Scene.hpp"
#include "Forge/Win32Window.hpp"

namespace Forge {

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
    EditorCommand draw(Scene& scene, Engine& engine, Camera& camera, bool& snap_to_grid, const std::string& status);
    void render();
    bool wants_mouse() const;
    bool wants_keyboard() const;

private:
    bool initialized_ = false;
};

} // namespace Forge

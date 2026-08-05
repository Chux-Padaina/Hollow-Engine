#pragma once

#include "Forge/Engine.hpp"
#include "Forge/Renderer.hpp"
#include "Forge/Scene.hpp"
#include "Forge/Win32Window.hpp"

#include <chrono>

namespace Forge {

class App {
public:
    int run();

private:
    void handle_keyboard();
    void update_window_title();

    Engine engine_;
    Scene scene_ = make_default_scene();
    Renderer renderer_;
    Win32Window window_;
    Camera camera_;
    std::chrono::steady_clock::time_point last_frame_;
};

} // namespace Forge

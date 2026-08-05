#include "Forge/App.hpp"

#include <Windows.h>

#include <sstream>

namespace Forge {

int App::run() {
    if (!window_.create(L"Forge Engine C++ Prototype", 1280, 720)) {
        return 1;
    }

    renderer_.initialize();
    last_frame_ = std::chrono::steady_clock::now();

    while (!window_.should_close() && window_.poll_messages()) {
        const auto now = std::chrono::steady_clock::now();
        const auto delta = std::chrono::duration<float>(now - last_frame_).count();
        last_frame_ = now;

        handle_keyboard();
        engine_.tick(delta);
        renderer_.resize(window_.width(), window_.height());
        renderer_.render(scene_, camera_, engine_);
        window_.swap_buffers();
        update_window_title();
    }

    return 0;
}

void App::handle_keyboard() {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        PostQuitMessage(0);
    }
    if ((GetAsyncKeyState('W') & 0x0001) != 0) {
        engine_.set_active_tool(EditorTool::Move);
    }
    if ((GetAsyncKeyState('E') & 0x0001) != 0) {
        engine_.set_active_tool(EditorTool::Rotate);
    }
    if ((GetAsyncKeyState('R') & 0x0001) != 0) {
        engine_.set_active_tool(EditorTool::Scale);
    }
    if ((GetAsyncKeyState('Q') & 0x0001) != 0) {
        engine_.set_active_tool(EditorTool::Select);
    }
    if ((GetAsyncKeyState('X') & 0x0001) != 0) {
        engine_.toggle_transform_space();
    }
    if ((GetAsyncKeyState(VK_DELETE) & 0x0001) != 0) {
        scene_.destroy_selected_actor();
    }
    if ((GetAsyncKeyState('1') & 0x0001) != 0) {
        Transform t;
        t.translation = {0.0f, 0.5f, 0.0f};
        scene_.create_static_mesh_actor("Cube", MeshPrimitive::Cube, t);
    }
    if ((GetAsyncKeyState('2') & 0x0001) != 0) {
        Transform t;
        t.scale = {5.0f, 1.0f, 5.0f};
        scene_.create_static_mesh_actor("Plane", MeshPrimitive::Plane, t);
    }
    if ((GetAsyncKeyState('3') & 0x0001) != 0) {
        Transform t;
        t.translation = {0.0f, 0.7f, 0.0f};
        t.scale = {1.4f, 1.4f, 1.4f};
        scene_.create_static_mesh_actor("Sphere", MeshPrimitive::Sphere, t);
    }
}

void App::update_window_title() {
    const Actor* selected = scene_.selected_actor();
    std::wstringstream title;
    title << L"Forge Engine C++ Prototype";
    title << L" | actors: " << scene_.actors().size();
    title << L" | selected: " << (selected != nullptr ? std::wstring(selected->name().begin(), selected->name().end()) : L"None");
    title << L" | W/E/R tools, X local/world, 1/2/3 add mesh, Del delete";
    SetWindowTextW(window_.hwnd(), title.str().c_str());
}

} // namespace Forge

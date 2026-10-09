// Win32 window creation, the message pump, and the WGL OpenGL context.
// Window messages are translated into the engine's Input state (see
// Core/Input.hpp). GL function pointers are loaded by the renderer; there's
// no GLFW/SDL dependency. Also hosts the native open/save file dialogs.
#pragma once

#include "Hollow/Core/Input.hpp"

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace Hollow {

class Win32Window {
public:
    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool create(const wchar_t* title, int width, int height);
    // Resizes the OS window so its client area is exactly width x height.
    void set_client_size(int width, int height);
    // Pumps pending messages. Returns false once the window should close.
    bool poll_messages();
    void swap_buffers();
    void set_title(const std::wstring& title);
    void request_close() { should_close_ = true; }
    // Undo a pending close request (e.g. the user cancelled "save changes?").
    void cancel_close() { should_close_ = false; }

    HWND hwnd() const { return hwnd_; }
    HDC device_context() const { return device_context_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool should_close() const { return should_close_; }
    bool focused() const { return focused_; }
    const std::wstring& last_error() const { return last_error_; }

    Input& input() { return input_; }
    const Input& input() const { return input_; }
    std::vector<std::wstring> take_dropped_files();

    // Native file dialogs. `filter` uses the Win32 double-null format, e.g.
    // L"Scenes (*.hscene)\0*.hscene\0". Return an empty path on cancel.
    std::filesystem::path open_file_dialog(const wchar_t* filter, const wchar_t* title) const;
    std::filesystem::path save_file_dialog(const wchar_t* filter, const wchar_t* title, const wchar_t* default_extension) const;

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void set_last_error(std::wstring message);
    void update_capture();

    HWND hwnd_ = nullptr;
    HDC device_context_ = nullptr;
    HGLRC gl_context_ = nullptr;
    int width_ = 1280;
    int height_ = 720;
    bool should_close_ = false;
    bool focused_ = true;
    std::wstring last_error_;
    std::wstring title_;
    Input input_;
    std::vector<std::wstring> dropped_files_;
};

} // namespace Hollow

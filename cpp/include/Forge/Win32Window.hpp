#pragma once

#include <Windows.h>

#include <string>

namespace Forge {

class Win32Window {
public:
    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool create(const wchar_t* title, int width, int height);
    bool poll_messages();
    void swap_buffers();

    HWND hwnd() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool should_close() const { return should_close_; }
    const std::wstring& last_error() const { return last_error_; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void set_last_error(std::wstring message);

    HWND hwnd_ = nullptr;
    HDC device_context_ = nullptr;
    HGLRC gl_context_ = nullptr;
    int width_ = 1280;
    int height_ = 720;
    bool should_close_ = false;
    std::wstring last_error_;
};

} // namespace Forge

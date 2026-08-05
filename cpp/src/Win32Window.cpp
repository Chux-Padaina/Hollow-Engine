#include "Forge/Win32Window.hpp"

#include <gl/GL.h>

namespace Forge {

Win32Window::~Win32Window() {
    if (gl_context_ != nullptr) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gl_context_);
    }
    if (hwnd_ != nullptr && device_context_ != nullptr) {
        ReleaseDC(hwnd_, device_context_);
    }
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
}

bool Win32Window::create(const wchar_t* title, int width, int height) {
    width_ = width;
    height_ = height;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"ForgeEngineWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = Win32Window::window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_OWNDC;
    RegisterClassW(&wc);

    RECT rect{0, 0, width_, height_};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(
        0,
        class_name,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        this);
    if (hwnd_ == nullptr) {
        return false;
    }

    device_context_ = GetDC(hwnd_);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    const int pixel_format = ChoosePixelFormat(device_context_, &pfd);
    if (pixel_format == 0 || SetPixelFormat(device_context_, pixel_format, &pfd) == FALSE) {
        return false;
    }

    gl_context_ = wglCreateContext(device_context_);
    if (gl_context_ == nullptr || wglMakeCurrent(device_context_, gl_context_) == FALSE) {
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    return true;
}

bool Win32Window::poll_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !should_close_;
}

void Win32Window::swap_buffers() {
    SwapBuffers(device_context_);
}

LRESULT CALLBACK Win32Window::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Win32Window* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        window = static_cast<Win32Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr) {
        return window->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Win32Window::handle_message(UINT message, WPARAM, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        width_ = LOWORD(lparam);
        height_ = HIWORD(lparam);
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        should_close_ = true;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, 0, lparam);
    }
}

} // namespace Forge

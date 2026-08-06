#include "Forge/Win32Window.hpp"

#include <Windows.h>
#include <windowsx.h>
#include <gl/GL.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

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
    if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        set_last_error(L"RegisterClassW failed");
        return false;
    }

    RECT rect{0, 0, width_, height_};
    if (AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE) == FALSE) {
        set_last_error(L"AdjustWindowRect failed");
        return false;
    }

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
        set_last_error(L"CreateWindowExW failed");
        return false;
    }

    device_context_ = GetDC(hwnd_);
    if (device_context_ == nullptr) {
        set_last_error(L"GetDC failed");
        return false;
    }

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
        set_last_error(L"OpenGL pixel format setup failed");
        return false;
    }

    gl_context_ = wglCreateContext(device_context_);
    if (gl_context_ == nullptr || wglMakeCurrent(device_context_, gl_context_) == FALSE) {
        set_last_error(L"OpenGL context creation failed");
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    DragAcceptFiles(hwnd_, TRUE);
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

void Win32Window::begin_frame() {
    mouse_.previous_x = mouse_.x;
    mouse_.previous_y = mouse_.y;
    mouse_.delta_x = 0;
    mouse_.delta_y = 0;
    mouse_.wheel_delta = 0;
    mouse_.left_pressed = false;
    mouse_.left_released = false;
}

void Win32Window::swap_buffers() {
    SwapBuffers(device_context_);
}

std::vector<std::wstring> Win32Window::take_dropped_files() {
    std::vector<std::wstring> files = std::move(dropped_files_);
    dropped_files_.clear();
    return files;
}

LRESULT CALLBACK Win32Window::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Win32Window* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        window = static_cast<Win32Window*>(create->lpCreateParams);
        window->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr) {
        return window->handle_message(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Win32Window::handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        mouse_.delta_x += x - mouse_.x;
        mouse_.delta_y += y - mouse_.y;
        mouse_.x = x;
        mouse_.y = y;
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        mouse_.left_down = true;
        mouse_.left_pressed = true;
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        mouse_.left_down = false;
        mouse_.left_released = true;
        return 0;
    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        mouse_.right_down = true;
        return 0;
    case WM_RBUTTONUP:
        ReleaseCapture();
        mouse_.right_down = false;
        return 0;
    case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        mouse_.middle_down = true;
        return 0;
    case WM_MBUTTONUP:
        ReleaseCapture();
        mouse_.middle_down = false;
        return 0;
    case WM_MOUSEWHEEL:
        mouse_.wheel_delta += GET_WHEEL_DELTA_WPARAM(wparam);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            dropped_files_.push_back(path);
        }
        DragFinish(drop);
        return 0;
    }
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
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

void Win32Window::set_last_error(std::wstring message) {
    const DWORD code = GetLastError();
    message += L" (GetLastError=" + std::to_wstring(code) + L")";
    last_error_ = std::move(message);
}

} // namespace Forge

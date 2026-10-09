// Implementation of Win32Window: window class registration, an OpenGL 4.5
// core-profile context (created through WGL_ARB_create_context after a
// bootstrap legacy context loads the WGL extensions), the window procedure
// (input, drag-and-drop, resize, focus), and the comdlg32 file dialogs.
#include "Hollow/Platform/Win32Window.hpp"
#include "Platform/Resource.h"

#include <glad/wgl.h>

#include <Windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <cstdint>
#include <iterator>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace Hollow {
namespace {

// wglGetProcAddress only returns extension/post-1.1 functions; the GL 1.1
// core lives in opengl32.dll itself.
GLADapiproc load_gl_proc(const char* name) {
    PROC proc = wglGetProcAddress(name);
    const auto value = reinterpret_cast<std::intptr_t>(proc);
    if (proc == nullptr || value == 1 || value == 2 || value == 3 || value == -1) {
        static HMODULE opengl32 = LoadLibraryW(L"opengl32.dll");
        proc = GetProcAddress(opengl32, name);
    }
    return reinterpret_cast<GLADapiproc>(proc);
}

} // namespace

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
    // Must happen before any window exists, or Windows bitmap-stretches the
    // whole (already rendered) framebuffer on scaled displays.
    SetProcessDPIAware();

    width_ = width;
    height_ = height;
    title_ = title;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"HollowEngineWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = Win32Window::window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_HOLLOW_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
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

    hwnd_ = CreateWindowExW(0, class_name, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        set_last_error(L"CreateWindowExW failed");
        return false;
    }
    // Title bar / alt-tab icons at their native sizes (the class icon is the large one).
    if (HANDLE small = LoadImageW(instance, MAKEINTRESOURCEW(IDI_HOLLOW_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED)) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    }
    if (HANDLE big = LoadImageW(instance, MAKEINTRESOURCEW(IDI_HOLLOW_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED)) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
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

    // A legacy context is needed first, only to load
    // wglCreateContextAttribsARB.
    HGLRC bootstrap = wglCreateContext(device_context_);
    if (bootstrap == nullptr || wglMakeCurrent(device_context_, bootstrap) == FALSE) {
        set_last_error(L"OpenGL context creation failed");
        return false;
    }
    gladLoadWGL(device_context_, load_gl_proc);
    if (wglCreateContextAttribsARB == nullptr) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(bootstrap);
        set_last_error(L"The GPU driver doesn't support WGL_ARB_create_context (OpenGL 4.5 is required). Update the GPU driver.");
        return false;
    }
    const int attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 5,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#ifndef NDEBUG
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
        0,
    };
    gl_context_ = wglCreateContextAttribsARB(device_context_, nullptr, attributes);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(bootstrap);
    if (gl_context_ == nullptr || wglMakeCurrent(device_context_, gl_context_) == FALSE) {
        gl_context_ = nullptr;
        set_last_error(L"Could not create an OpenGL 4.5 core context. Hollow Engine needs a GPU/driver with OpenGL 4.5.");
        return false;
    }
    if (gladLoadGL(load_gl_proc) == 0) {
        set_last_error(L"Loading the OpenGL 4.5 functions failed.");
        return false;
    }
    if (wglSwapIntervalEXT != nullptr) {
        wglSwapIntervalEXT(1);
    }

    ShowWindow(hwnd_, SW_SHOW);
    DragAcceptFiles(hwnd_, TRUE);
    return true;
}

void Win32Window::set_client_size(int width, int height) {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rect{0, 0, width, height};
    if (AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE) == FALSE) {
        return;
    }
    // WM_SIZE then updates width_/height_ like a manual border drag.
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
}

bool Win32Window::poll_messages() {
    input_.begin_frame();
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        // WM_QUIT is never dispatched to a window procedure, so it has to be
        // caught here.
        if (message.message == WM_QUIT) {
            should_close_ = true;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !should_close_;
}

void Win32Window::swap_buffers() {
    SwapBuffers(device_context_);
}

void Win32Window::set_title(const std::wstring& title) {
    if (title == title_ || hwnd_ == nullptr) {
        return;
    }
    title_ = title;
    SetWindowTextW(hwnd_, title_.c_str());
}

std::vector<std::wstring> Win32Window::take_dropped_files() {
    std::vector<std::wstring> files = std::move(dropped_files_);
    dropped_files_.clear();
    return files;
}

std::filesystem::path Win32Window::open_file_dialog(const wchar_t* filter, const wchar_t* title) const {
    wchar_t buffer[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = title;
    // NOCHANGEDIR: without it the dialog changes the process working
    // directory as a side effect.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) == FALSE) {
        return {};
    }
    return std::filesystem::path(buffer);
}

std::filesystem::path Win32Window::save_file_dialog(const wchar_t* filter, const wchar_t* title, const wchar_t* default_extension) const {
    wchar_t buffer[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = default_extension;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn) == FALSE) {
        return {};
    }
    return std::filesystem::path(buffer);
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

void Win32Window::update_capture() {
    // Keep the mouse captured while any button is held so drags that leave
    // the window still deliver their button-up.
    const bool any_down = input_.mouse_down(MouseButton::Left) || input_.mouse_down(MouseButton::Right) || input_.mouse_down(MouseButton::Middle);
    if (any_down && GetCapture() != hwnd_) {
        SetCapture(hwnd_);
    } else if (!any_down && GetCapture() == hwnd_) {
        ReleaseCapture();
    }
}

LRESULT Win32Window::handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        input_.on_key(static_cast<int>(wparam), true);
        // Let Alt+F4 through; swallow everything else (notably F10, which
        // would otherwise activate the non-existent window menu).
        if (message == WM_SYSKEYDOWN && wparam == VK_F4) {
            break;
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        input_.on_key(static_cast<int>(wparam), false);
        return 0;
    case WM_SETFOCUS:
        focused_ = true;
        return 0;
    case WM_KILLFOCUS:
        focused_ = false;
        input_.on_focus_lost();
        return 0;
    case WM_MOUSEMOVE:
        input_.on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        input_.on_mouse_button(MouseButton::Left, message == WM_LBUTTONDOWN);
        update_capture();
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        input_.on_mouse_button(MouseButton::Right, message == WM_RBUTTONDOWN);
        update_capture();
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        input_.on_mouse_button(MouseButton::Middle, message == WM_MBUTTONDOWN);
        update_capture();
        return 0;
    case WM_MOUSEWHEEL:
        input_.on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
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
        should_close_ = true;
        return 0;
    case WM_DESTROY:
        should_close_ = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Win32Window::set_last_error(std::wstring message) {
    const DWORD code = GetLastError();
    message += L" (GetLastError=" + std::to_wstring(code) + L")";
    last_error_ = std::move(message);
}

} // namespace Hollow

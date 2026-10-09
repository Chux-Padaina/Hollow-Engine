// Implementation of EditorTheme.hpp.
#include "Editor/EditorTheme.hpp"

#include "Hollow/Core/FileUtil.hpp"

#include <imgui_internal.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace Hollow::theme {

namespace {

float g_scale = 1.0f;
float g_delta = 1.0f / 60.0f;
ImFont* g_body = nullptr;
ImFont* g_heading = nullptr;

struct SectionState {
    bool open = true;
    float t = 1.0f;
    float content_height = 0.0f;
    bool initialized = false;
};
std::unordered_map<ImGuiID, SectionState> g_sections;
std::unordered_map<ImGuiID, float> g_animations;

struct SectionFrame {
    bool visible = false;
    bool child = false;
    ImGuiID id = 0;
    ImVec2 start{};
    float width = 0.0f;
    float pad = 0.0f;
};
std::vector<SectionFrame> g_section_stack;
std::vector<float> g_right_pad_stack;

float ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

} // namespace

ImU32 u32(const ImVec4& c, float alpha_scale) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha_scale));
}

float scale() {
    return g_scale;
}

ImFont* heading_font() {
    return g_heading;
}

ImFont* body_font() {
    return g_body;
}

void new_frame(float delta_seconds) {
    g_delta = std::clamp(delta_seconds, 0.0f, 0.1f);
}

float animate(ImGuiID id, bool target, float speed) {
    auto [it, inserted] = g_animations.try_emplace(id, target ? 1.0f : 0.0f);
    float& v = it->second;
    const float goal = target ? 1.0f : 0.0f;
    const float step = speed * g_delta;
    v = v < goal ? std::min(goal, v + step) : std::max(goal, v - step);
    return v;
}

// --- Setup -------------------------------------------------------------------

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::vector<std::filesystem::path> regular{"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/selawik.ttf", "C:/Windows/Fonts/tahoma.ttf", "C:/Windows/Fonts/arial.ttf"};
    std::vector<std::filesystem::path> bold{"C:/Windows/Fonts/segoeuisb.ttf", "C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/tahomabd.ttf", "C:/Windows/Fonts/arialbd.ttf"};
    auto load = [&io](const std::vector<std::filesystem::path>& candidates) -> ImFont* {
        for (const auto& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                continue;
            }
            ImFontConfig config;
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(to_utf8(path).c_str(), 15.0f, &config)) {
                return font;
            }
        }
        return nullptr;
    };
    g_body = load(regular);
    if (g_body != nullptr) {
        g_heading = load(bold);
        if (g_heading == nullptr) {
            g_heading = g_body;
        }
    } else {
        g_body = io.Fonts->AddFontDefault();
        g_heading = g_body;
    }
    io.FontDefault = g_body;
}

void apply_style(float s) {
    g_scale = s;
    ImGuiStyle& style = ImGui::GetStyle();
    // Fonts are rasterized at whatever size is needed (ImGui 1.92+).
    style.FontSizeBase = 15.0f;
    style.FontScaleMain = s;
    style.WindowRounding = 12.0f * s;
    style.ChildRounding = 10.0f * s;
    style.FrameRounding = 7.0f * s;
    style.PopupRounding = 10.0f * s;
    style.ScrollbarRounding = 8.0f * s;
    style.GrabRounding = 2.0f * s;
    style.TabRounding = 7.0f * s;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = {10.0f * s, 10.0f * s};
    style.FramePadding = {9.0f * s, 5.0f * s};
    style.ItemSpacing = {8.0f * s, 6.0f * s};
    style.ItemInnerSpacing = {6.0f * s, 4.0f * s};
    style.IndentSpacing = 14.0f * s;
    style.ScrollbarSize = 10.0f * s;
    style.GrabMinSize = 4.0f * s; // a thin marker so it never hides the value text
    style.WindowTitleAlign = {0.0f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.5f};
    style.DisabledAlpha = 0.45f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = color::kText;
    c[ImGuiCol_TextDisabled] = color::kMuted;
    c[ImGuiCol_WindowBg] = color::kPanel;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.107f, 0.98f);
    c[ImGuiCol_Border] = color::kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = color::kWell;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.09f, 0.09f, 0.095f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.11f, 0.11f, 0.115f, 1.0f);
    c[ImGuiCol_TitleBg] = color::kPanel;
    c[ImGuiCol_TitleBgActive] = color::kPanel;
    c[ImGuiCol_TitleBgCollapsed] = color::kPanel;
    c[ImGuiCol_MenuBarBg] = color::kPanel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = color::kRaised;
    c[ImGuiCol_ScrollbarGrabHovered] = color::kHover;
    c[ImGuiCol_ScrollbarGrabActive] = color::kLimeDim;
    c[ImGuiCol_CheckMark] = color::kLime;
    c[ImGuiCol_SliderGrab] = color::kLime;
    c[ImGuiCol_SliderGrabActive] = color::kWhite;
    c[ImGuiCol_Button] = color::kRaised;
    c[ImGuiCol_ButtonHovered] = color::kHover;
    c[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.25f, 0.265f, 1.0f);
    c[ImGuiCol_Header] = color::kLimeSoft;
    c[ImGuiCol_HeaderHovered] = color::kRaised;
    c[ImGuiCol_HeaderActive] = ImVec4(0.831f, 1.0f, 0.247f, 0.22f);
    c[ImGuiCol_Separator] = color::kBorder;
    c[ImGuiCol_SeparatorHovered] = color::kLimeDim;
    c[ImGuiCol_SeparatorActive] = color::kLime;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = color::kLimeDim;
    c[ImGuiCol_ResizeGripActive] = color::kLime;
    c[ImGuiCol_Tab] = color::kRaised;
    c[ImGuiCol_TabHovered] = color::kHover;
    c[ImGuiCol_PlotLines] = color::kLime;
    c[ImGuiCol_PlotHistogram] = color::kLime;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.831f, 1.0f, 0.247f, 0.30f);
    c[ImGuiCol_DragDropTarget] = color::kLime;
    c[ImGuiCol_NavCursor] = color::kLime;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_TableHeaderBg] = color::kRaised;
    c[ImGuiCol_TableBorderLight] = color::kBorder;
    c[ImGuiCol_TableBorderStrong] = color::kBorder;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
}

// --- Icons ----------------------------------------------------------------------

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 col) {
    const float h = size * 0.5f;
    const float t = std::max(1.2f, size * 0.11f); // stroke
    auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine({c.x + x0 * h, c.y + y0 * h}, {c.x + x1 * h, c.y + y1 * h}, col, t); };
    auto tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) {
        dl->AddTriangleFilled({c.x + x0 * h, c.y + y0 * h}, {c.x + x1 * h, c.y + y1 * h}, {c.x + x2 * h, c.y + y2 * h}, col);
    };
    auto rect = [&](float x0, float y0, float x1, float y1, float r = 0.0f) { dl->AddRect({c.x + x0 * h, c.y + y0 * h}, {c.x + x1 * h, c.y + y1 * h}, col, r * h, 0, t); };
    auto rect_filled = [&](float x0, float y0, float x1, float y1, float r = 0.0f) { dl->AddRectFilled({c.x + x0 * h, c.y + y0 * h}, {c.x + x1 * h, c.y + y1 * h}, col, r * h); };
    auto circle = [&](float x, float y, float r) { dl->AddCircle({c.x + x * h, c.y + y * h}, r * h, col, 0, t); };
    auto circle_filled = [&](float x, float y, float r) { dl->AddCircleFilled({c.x + x * h, c.y + y * h}, r * h, col); };
    auto arc = [&](float x, float y, float r, float a0, float a1) {
        dl->PathArcTo({c.x + x * h, c.y + y * h}, r * h, a0, a1, 20);
        dl->PathStroke(col, 0, t);
    };
    switch (icon) {
    case Icon::Select:
        tri(-0.55f, -0.85f, 0.55f, 0.1f, -0.1f, 0.25f);
        tri(-0.55f, -0.85f, -0.1f, 0.25f, -0.35f, 0.65f);
        line(-0.05f, 0.2f, 0.4f, 0.85f);
        break;
    case Icon::Move:
        line(-0.85f, 0, 0.85f, 0);
        line(0, -0.85f, 0, 0.85f);
        tri(0.95f, 0, 0.6f, -0.28f, 0.6f, 0.28f);
        tri(-0.95f, 0, -0.6f, -0.28f, -0.6f, 0.28f);
        tri(0, -0.95f, -0.28f, -0.6f, 0.28f, -0.6f);
        tri(0, 0.95f, -0.28f, 0.6f, 0.28f, 0.6f);
        break;
    case Icon::Rotate:
        arc(0, 0, 0.7f, -0.6f, 4.0f);
        tri(0.75f, -0.05f, 0.35f, -0.45f, 1.0f, -0.6f);
        break;
    case Icon::Scale:
        rect(-0.8f, -0.1f, 0.1f, 0.8f, 0.1f);
        line(-0.1f, 0.1f, 0.75f, -0.75f);
        line(0.25f, -0.8f, 0.8f, -0.8f);
        line(0.8f, -0.8f, 0.8f, -0.25f);
        break;
    case Icon::World:
        circle(0, 0, 0.82f);
        line(-0.82f, 0, 0.82f, 0);
        dl->AddEllipse(c, ImVec2(0.38f * h, 0.82f * h), col, 0.0f, 0, t);
        break;
    case Icon::Local:
        rect(-0.7f, -0.7f, 0.7f, 0.7f, 0.15f);
        line(-0.3f, 0.3f, 0.35f, 0.3f);
        line(-0.3f, 0.3f, -0.3f, -0.35f);
        break;
    case Icon::Magnet:
        arc(0, -0.05f, 0.55f, 0.0f, 3.14159f);
        line(-0.55f, -0.05f, -0.55f, -0.75f);
        line(0.55f, -0.05f, 0.55f, -0.75f);
        rect_filled(-0.72f, -0.85f, -0.38f, -0.6f);
        rect_filled(0.38f, -0.85f, 0.72f, -0.6f);
        break;
    case Icon::Grid:
        rect(-0.8f, -0.8f, 0.8f, 0.8f, 0.1f);
        line(-0.27f, -0.8f, -0.27f, 0.8f);
        line(0.27f, -0.8f, 0.27f, 0.8f);
        line(-0.8f, -0.27f, 0.8f, -0.27f);
        line(-0.8f, 0.27f, 0.8f, 0.27f);
        break;
    case Icon::Play:
        tri(-0.5f, -0.75f, 0.75f, 0.0f, -0.5f, 0.75f);
        break;
    case Icon::Pause:
        rect_filled(-0.6f, -0.7f, -0.15f, 0.7f, 0.1f);
        rect_filled(0.15f, -0.7f, 0.6f, 0.7f, 0.1f);
        break;
    case Icon::Stop:
        rect_filled(-0.62f, -0.62f, 0.62f, 0.62f, 0.15f);
        break;
    case Icon::Step:
        tri(-0.7f, -0.7f, 0.35f, 0.0f, -0.7f, 0.7f);
        rect_filled(0.45f, -0.7f, 0.75f, 0.7f);
        break;
    case Icon::Plus:
        line(-0.7f, 0, 0.7f, 0);
        line(0, -0.7f, 0, 0.7f);
        break;
    case Icon::Close:
        line(-0.6f, -0.6f, 0.6f, 0.6f);
        line(-0.6f, 0.6f, 0.6f, -0.6f);
        break;
    case Icon::Trash:
        line(-0.75f, -0.55f, 0.75f, -0.55f);
        line(-0.25f, -0.8f, 0.25f, -0.8f);
        rect(-0.55f, -0.45f, 0.55f, 0.85f, 0.1f);
        line(-0.15f, -0.2f, -0.15f, 0.55f);
        line(0.2f, -0.2f, 0.2f, 0.55f);
        break;
    case Icon::Copy:
        rect(-0.75f, -0.4f, 0.35f, 0.8f, 0.12f);
        rect(-0.35f, -0.8f, 0.75f, 0.4f, 0.12f);
        break;
    case Icon::Eye:
    case Icon::EyeOff:
        dl->AddEllipse(c, ImVec2(0.85f * h, 0.5f * h), col, 0.0f, 0, t);
        circle_filled(0, 0, 0.25f);
        if (icon == Icon::EyeOff) {
            line(-0.85f, 0.75f, 0.85f, -0.75f);
        }
        break;
    case Icon::Chevron:
        line(-0.25f, -0.5f, 0.25f, 0.0f);
        line(0.25f, 0.0f, -0.25f, 0.5f);
        break;
    case Icon::ChevronDown:
        line(-0.5f, -0.25f, 0.0f, 0.25f);
        line(0.0f, 0.25f, 0.5f, -0.25f);
        break;
    case Icon::Cube:
        line(0, -0.85f, 0.75f, -0.45f);
        line(0, -0.85f, -0.75f, -0.45f);
        line(-0.75f, -0.45f, -0.75f, 0.45f);
        line(0.75f, -0.45f, 0.75f, 0.45f);
        line(-0.75f, 0.45f, 0, 0.85f);
        line(0.75f, 0.45f, 0, 0.85f);
        line(-0.75f, -0.45f, 0, -0.05f);
        line(0.75f, -0.45f, 0, -0.05f);
        line(0, -0.05f, 0, 0.85f);
        break;
    case Icon::Sphere:
        circle(0, 0, 0.8f);
        dl->AddEllipse(c, ImVec2(0.8f * h, 0.3f * h), col, 0.0f, 0, t);
        break;
    case Icon::Plane:
        line(-0.85f, 0.35f, -0.35f, -0.35f);
        line(-0.35f, -0.35f, 0.85f, -0.35f);
        line(0.85f, -0.35f, 0.35f, 0.35f);
        line(0.35f, 0.35f, -0.85f, 0.35f);
        break;
    case Icon::Light:
        circle(0, -0.2f, 0.52f);
        line(-0.25f, 0.45f, 0.25f, 0.45f);
        line(-0.18f, 0.7f, 0.18f, 0.7f);
        break;
    case Icon::Sun:
        circle_filled(0, 0, 0.36f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.785398f;
            line(std::cos(a) * 0.58f, std::sin(a) * 0.58f, std::cos(a) * 0.88f, std::sin(a) * 0.88f);
        }
        break;
    case Icon::Spot:
        line(-0.3f, -0.8f, 0.3f, -0.8f);
        line(-0.3f, -0.8f, -0.75f, 0.6f);
        line(0.3f, -0.8f, 0.75f, 0.6f);
        dl->AddEllipse({c.x, c.y + 0.6f * h}, ImVec2(0.75f * h, 0.22f * h), col, 0.0f, 0, t);
        break;
    case Icon::Camera:
        rect(-0.85f, -0.45f, 0.35f, 0.5f, 0.15f);
        tri(0.4f, 0.0f, 0.9f, -0.4f, 0.9f, 0.4f);
        break;
    case Icon::Folder:
    case Icon::Open:
        line(-0.85f, -0.6f, -0.3f, -0.6f);
        line(-0.3f, -0.6f, -0.15f, -0.4f);
        rect(-0.85f, -0.4f, 0.85f, 0.7f, 0.12f);
        break;
    case Icon::Empty:
        circle(0, 0, 0.35f);
        break;
    case Icon::Gear:
        circle(0, 0, 0.32f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.785398f;
            line(std::cos(a) * 0.5f, std::sin(a) * 0.5f, std::cos(a) * 0.85f, std::sin(a) * 0.85f);
        }
        circle(0, 0, 0.6f);
        break;
    case Icon::Layers:
        line(0, -0.8f, 0.85f, -0.35f);
        line(0.85f, -0.35f, 0, 0.1f);
        line(0, 0.1f, -0.85f, -0.35f);
        line(-0.85f, -0.35f, 0, -0.8f);
        line(-0.85f, 0.1f, 0, 0.55f);
        line(0, 0.55f, 0.85f, 0.1f);
        break;
    case Icon::Chart:
        rect_filled(-0.75f, 0.1f, -0.35f, 0.8f, 0.1f);
        rect_filled(-0.2f, -0.5f, 0.2f, 0.8f, 0.1f);
        rect_filled(0.35f, -0.2f, 0.75f, 0.8f, 0.1f);
        break;
    case Icon::Terminal:
        rect(-0.85f, -0.7f, 0.85f, 0.7f, 0.15f);
        line(-0.5f, -0.25f, -0.15f, 0.05f);
        line(-0.15f, 0.05f, -0.5f, 0.35f);
        line(0.05f, 0.35f, 0.45f, 0.35f);
        break;
    case Icon::Probe:
        circle(0, 0, 0.75f);
        circle_filled(-0.25f, -0.25f, 0.18f);
        break;
    case Icon::Volume:
        rect(-0.75f, -0.75f, 0.75f, 0.75f, 0.1f);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                circle_filled(static_cast<float>(x) * 0.42f, static_cast<float>(y) * 0.42f, 0.09f);
            }
        }
        break;
    case Icon::Decal:
        rect(-0.75f, -0.75f, 0.75f, 0.75f, 0.2f);
        tri(-0.45f, 0.45f, 0.0f, -0.2f, 0.45f, 0.45f);
        circle_filled(0.35f, -0.4f, 0.13f);
        break;
    case Icon::Particles:
        circle_filled(-0.45f, 0.4f, 0.17f);
        circle_filled(0.1f, -0.1f, 0.22f);
        circle_filled(0.55f, -0.6f, 0.13f);
        circle_filled(-0.4f, -0.5f, 0.1f);
        circle_filled(0.5f, 0.5f, 0.11f);
        break;
    case Icon::Terrain:
        line(-0.9f, 0.6f, -0.35f, -0.3f);
        line(-0.35f, -0.3f, 0.0f, 0.15f);
        line(0.0f, 0.15f, 0.4f, -0.6f);
        line(0.4f, -0.6f, 0.9f, 0.6f);
        line(-0.9f, 0.6f, 0.9f, 0.6f);
        break;
    case Icon::Water:
        arc(-0.45f, -0.15f, 0.3f, 3.14159f, 6.28318f);
        arc(0.15f, -0.15f, 0.3f, 0.0f, 3.14159f);
        arc(-0.45f, 0.4f, 0.3f, 3.14159f, 6.28318f);
        arc(0.15f, 0.4f, 0.3f, 0.0f, 3.14159f);
        break;
    case Icon::Bone:
        line(-0.5f, 0.5f, 0.5f, -0.5f);
        circle(-0.6f, 0.6f, 0.22f);
        circle(0.6f, -0.6f, 0.22f);
        break;
    case Icon::Search:
        circle(-0.15f, -0.15f, 0.55f);
        line(0.25f, 0.25f, 0.8f, 0.8f);
        break;
    case Icon::Save:
        rect(-0.8f, -0.8f, 0.8f, 0.8f, 0.12f);
        rect_filled(-0.45f, -0.8f, 0.45f, -0.3f);
        rect(-0.5f, 0.15f, 0.5f, 0.8f);
        break;
    case Icon::Import:
        line(0, -0.85f, 0, 0.35f);
        tri(0, 0.6f, -0.4f, 0.1f, 0.4f, 0.1f);
        line(-0.8f, 0.85f, 0.8f, 0.85f);
        break;
    case Icon::Undo:
        arc(0.05f, 0.1f, 0.6f, 3.6f, 6.6f);
        tri(-0.85f, -0.35f, -0.25f, -0.5f, -0.6f, 0.1f);
        break;
    case Icon::Redo:
        arc(-0.05f, 0.1f, 0.6f, 2.82f, 5.82f);
        tri(0.85f, -0.35f, 0.25f, -0.5f, 0.6f, 0.1f);
        break;
    case Icon::Bake:
        rect(-0.8f, -0.45f, 0.8f, 0.75f, 0.12f);
        line(-0.5f, -0.75f, -0.5f, -0.55f);
        line(0.0f, -0.85f, 0.0f, -0.55f);
        line(0.5f, -0.75f, 0.5f, -0.55f);
        line(-0.5f, 0.15f, 0.5f, 0.15f);
        break;
    case Icon::Sparkle:
        tri(0, -0.9f, 0.18f, -0.18f, -0.18f, -0.18f);
        tri(0, 0.9f, 0.18f, 0.18f, -0.18f, 0.18f);
        tri(-0.9f, 0, -0.18f, 0.18f, -0.18f, -0.18f);
        tri(0.9f, 0, 0.18f, 0.18f, 0.18f, -0.18f);
        break;
    case Icon::Physics:
        circle(0, 0, 0.25f);
        dl->AddEllipse(c, ImVec2(0.85f * h, 0.35f * h), col, 0.5f, 0, t);
        dl->AddEllipse(c, ImVec2(0.85f * h, 0.35f * h), col, -0.5f, 0, t);
        break;
    case Icon::Script:
        line(-0.35f, -0.6f, -0.8f, 0.0f);
        line(-0.8f, 0.0f, -0.35f, 0.6f);
        line(0.35f, -0.6f, 0.8f, 0.0f);
        line(0.8f, 0.0f, 0.35f, 0.6f);
        line(0.12f, -0.75f, -0.12f, 0.75f);
        break;
    case Icon::Collider:
        rect(-0.75f, -0.75f, 0.75f, 0.75f, 0.0f);
        line(-0.75f, -0.75f, -0.4f, -0.75f);
        circle(0, 0, 0.35f);
        break;
    }
}

// --- Widgets -------------------------------------------------------------------------

bool pill_button(const char* label, Pill style, ImVec2 size, Icon icon, bool has_icon) {
    const float s = g_scale;
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const float icon_w = has_icon ? 15.0f * s : 0.0f;
    const float gap = has_icon && text.x > 0.0f ? 6.0f * s : 0.0f;
    const float height = size.y > 0.0f ? size.y : ImGui::GetFrameHeight();
    const float width = size.x > 0.0f ? size.x : (size.x < 0.0f ? ImGui::GetContentRegionAvail().x : text.x + icon_w + gap + 22.0f * s);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, {width, height});
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImVec4 bg;
    ImVec4 fg;
    switch (style) {
    case Pill::Primary:
        bg = hovered ? ImVec4(0.88f, 1.0f, 0.45f, 1.0f) : color::kLime;
        fg = color::kBlack;
        break;
    case Pill::White:
        bg = hovered ? ImVec4(0.86f, 0.86f, 0.86f, 1.0f) : color::kWhite;
        fg = color::kBlack;
        break;
    case Pill::Danger:
        bg = hovered ? ImVec4(0.55f, 0.18f, 0.17f, 1.0f) : ImVec4(0.42f, 0.14f, 0.14f, 1.0f);
        fg = color::kText;
        break;
    case Pill::Ghost:
        bg = hovered ? color::kRaised : ImVec4(0, 0, 0, 0);
        fg = hovered ? color::kText : color::kMuted;
        break;
    case Pill::Secondary:
    default:
        bg = hovered ? color::kHover : color::kRaised;
        fg = color::kText;
        break;
    }
    if (held) {
        bg.x *= 0.85f;
        bg.y *= 0.85f;
        bg.z *= 0.85f;
    }
    const float alpha = disabled ? 0.4f : 1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + width, pos.y + height}, u32(bg, alpha), height * 0.5f);
    const float content = icon_w + gap + text.x;
    float x = pos.x + (width - content) * 0.5f;
    const float cy = pos.y + height * 0.5f;
    if (has_icon) {
        draw_icon(dl, icon, {x + icon_w * 0.5f, cy}, icon_w * 0.82f, u32(fg, alpha));
        x += icon_w + gap;
    }
    if (text.x > 0.0f) {
        dl->AddText({x, cy - text.y * 0.5f}, u32(fg, alpha), label, ImGui::FindRenderedTextEnd(label));
    }
    return clicked && !disabled;
}

bool icon_button(const char* id, Icon icon, bool active, const char* tooltip, float size) {
    const float s = g_scale;
    const float side = size > 0.0f ? size : ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, {side, side});
    const bool hovered = ImGui::IsItemHovered();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = disabled ? 0.35f : 1.0f;
    const float hover_t = animate(ImGui::GetItemID(), hovered || active, 14.0f);
    if (active) {
        dl->AddRectFilled(pos, {pos.x + side, pos.y + side}, u32(color::kLimeSoft, alpha * 1.4f), 7.0f * s);
    } else if (hover_t > 0.0f) {
        dl->AddRectFilled(pos, {pos.x + side, pos.y + side}, u32(color::kHover, alpha * hover_t), 7.0f * s);
    }
    const ImVec4 fg = active ? color::kLime : (hovered ? color::kText : ImVec4(0.75f, 0.75f, 0.77f, 1.0f));
    draw_icon(dl, icon, {pos.x + side * 0.5f, pos.y + side * 0.5f}, side * 0.5f, u32(fg, alpha));
    if (tooltip != nullptr && hovered) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked && !disabled;
}

bool toggle(const char* id, bool* value) {
    const float s = g_scale;
    const float height = ImGui::GetFrameHeight() * 0.78f;
    const float width = height * 1.85f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float y_off = (ImGui::GetFrameHeight() - height) * 0.5f;
    const ImVec2 pos{cursor.x, cursor.y + y_off};
    ImGui::InvisibleButton(id, {width, ImGui::GetFrameHeight()});
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
        ImGui::MarkItemEdited(ImGui::GetItemID());
    }
    const bool hovered = ImGui::IsItemHovered();
    const float t = ease(animate(ImGui::GetItemID(), *value, 9.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 off = hovered ? color::kHover : color::kRaised;
    const ImVec4 on = color::kLime;
    const ImVec4 track{off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1.0f};
    dl->AddRectFilled(pos, {pos.x + width, pos.y + height}, u32(track), height * 0.5f);
    const float radius = height * 0.5f - 2.5f * s;
    const float knob_x = pos.x + height * 0.5f + (width - height) * t;
    const ImVec4 knob = t > 0.5f ? color::kBlack : color::kText;
    dl->AddCircleFilled({knob_x, pos.y + height * 0.5f}, radius, u32(knob));
    return changed;
}

void chip(const char* text, const ImVec4& fg, const ImVec4& bg) {
    const float s = g_scale;
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float h = size.y + 4.0f * s;
    const float w = size.x + 14.0f * s;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float y = cursor.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({cursor.x, y}, {cursor.x + w, y + h}, u32(bg), h * 0.5f);
    dl->AddText({cursor.x + 7.0f * s, y + 2.0f * s}, u32(fg), text);
    ImGui::Dummy({w, ImGui::GetFrameHeight()});
}

void caption(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color::kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void divider() {
    ImGui::Dummy({0.0f, 2.0f * g_scale});
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(p, {p.x + w, p.y}, u32(color::kBorder), 1.0f);
    ImGui::Dummy({0.0f, 4.0f * g_scale});
}

void title(const char* text, const char* subtitle) {
    const float s = g_scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    dl->AddCircleFilled({cursor.x + 4.0f * s, cursor.y + h * 0.5f}, 3.5f * s, u32(color::kLime));
    ImGui::SetCursorScreenPos({cursor.x + 14.0f * s, cursor.y});
    ImGui::PushFont(g_heading, 0.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    if (subtitle != nullptr) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color::kMuted);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }
}

namespace {
bool section_impl(const char* id, const char* label, bool default_open, const char* subtitle, Icon icon, bool has_icon, Icon action_icon, const char* action_tooltip,
    bool* action_clicked);
} // namespace

bool begin_section(const char* id, const char* label, bool default_open, const char* subtitle, Icon icon, bool has_icon) {
    return section_impl(id, label, default_open, subtitle, icon, has_icon, Icon::Empty, nullptr, nullptr);
}

bool begin_section_action(const char* id, const char* label, bool default_open, Icon icon, Icon action_icon, const char* action_tooltip, bool* action_clicked) {
    return section_impl(id, label, default_open, nullptr, icon, true, action_icon, action_tooltip, action_clicked);
}

namespace {
bool section_impl(const char* id, const char* label, bool default_open, const char* subtitle, Icon icon, bool has_icon, Icon action_icon, const char* action_tooltip,
    bool* action_clicked) {
    const float s = g_scale;
    ImGui::PushID(id);
    const ImGuiID sid = ImGui::GetID("##section");
    SectionState& st = g_sections[sid];
    if (!st.initialized) {
        st.initialized = true;
        st.open = default_open;
        st.t = default_open ? 1.0f : 0.0f;
    }
    const float pad = 10.0f * s;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
    const float header_h = ImGui::GetFrameHeight() + 8.0f * s;

    // Card background sized from last frame's content height (one-frame lag
    // when the content changes size; invisible in practice).
    const float eased = ease(st.t);
    const float content_h = st.content_height > 0.0f ? st.content_height : 120.0f * s;
    const float body_h = st.t > 0.0f ? content_h * eased + pad * eased : 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 card_max{start.x + width, start.y + header_h + body_h};
    dl->AddRectFilled(start, card_max, u32(color::kRaised, 0.55f), 10.0f * s);

    if (action_clicked != nullptr) {
        ImGui::SetNextItemAllowOverlap();
    }
    const bool clicked = ImGui::InvisibleButton("##header", {width, header_h});
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        st.open = !st.open;
    }
    if (action_clicked != nullptr) {
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const float button = ImGui::GetFrameHeight() * 0.85f;
        ImGui::SetCursorScreenPos({start.x + width - pad * 0.6f - button, start.y + (header_h - button) * 0.5f});
        *action_clicked = icon_button("##action", action_icon, false, action_tooltip, button);
        ImGui::SetCursorScreenPos(after);
    }
    if (hovered) {
        dl->AddRectFilled(start, {start.x + width, start.y + header_h}, u32(color::kHover, 0.5f), 10.0f * s,
            st.t > 0.01f ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    }
    // Chevron rotates from right (closed) to down (open).
    const float angle = ease(st.t) * 1.5708f;
    const ImVec2 chevron_center{start.x + pad + 5.0f * s, start.y + header_h * 0.5f};
    const float ch = 4.0f * s;
    auto rot = [&](float x, float y) {
        return ImVec2{chevron_center.x + x * std::cos(angle) - y * std::sin(angle), chevron_center.y + x * std::sin(angle) + y * std::cos(angle)};
    };
    const ImU32 chevron_col = u32(st.open ? color::kLime : color::kMuted);
    dl->AddLine(rot(-ch * 0.5f, -ch), rot(ch * 0.5f, 0.0f), chevron_col, 1.8f * s);
    dl->AddLine(rot(ch * 0.5f, 0.0f), rot(-ch * 0.5f, ch), chevron_col, 1.8f * s);
    float x = start.x + pad + 18.0f * s;
    if (has_icon) {
        draw_icon(dl, icon, {x + 7.0f * s, start.y + header_h * 0.5f}, 13.0f * s, u32(st.open ? color::kText : color::kMuted));
        x += 22.0f * s;
    }
    ImGui::PushFont(g_heading, 0.0f);
    const ImVec2 text = ImGui::CalcTextSize(label);
    dl->AddText({x, start.y + (header_h - text.y) * 0.5f}, u32(color::kText), label);
    ImGui::PopFont();
    if (subtitle != nullptr) {
        const ImVec2 sub = ImGui::CalcTextSize(subtitle);
        dl->AddText({start.x + width - pad - sub.x, start.y + (header_h - sub.y) * 0.5f}, u32(color::kMuted), subtitle);
    }

    st.t = std::clamp(st.t + (st.open ? 1.0f : -1.0f) * g_delta * 7.0f, 0.0f, 1.0f);

    SectionFrame frame;
    frame.id = sid;
    frame.start = start;
    frame.width = width;
    frame.pad = pad;
    if (st.t <= 0.0f) {
        frame.visible = false;
        g_section_stack.push_back(frame);
        return false;
    }
    frame.visible = true;
    ImGui::SetCursorScreenPos({start.x + pad, start.y + header_h});
    const float inner_w = width - pad * 2.0f;
    if (st.t < 1.0f) {
        frame.child = true;
        ImGui::BeginChild("##content", {inner_w, std::max(content_h * eased, 1.0f)}, ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    } else {
        frame.child = false;
        ImGui::BeginGroup();
        ImGui::PushItemWidth(inner_w);
        g_right_pad_stack.push_back(pad);
    }
    g_section_stack.push_back(frame);
    return true;
}
} // namespace

void end_section() {
    if (g_section_stack.empty()) {
        return;
    }
    const SectionFrame frame = g_section_stack.back();
    g_section_stack.pop_back();
    SectionState& st = g_sections[frame.id];
    const float s = g_scale;
    if (frame.visible) {
        if (frame.child) {
            ImGui::EndChild();
        } else {
            ImGui::PopItemWidth();
            g_right_pad_stack.pop_back();
            ImGui::EndGroup();
            st.content_height = ImGui::GetItemRectSize().y;
        }
        ImGui::Dummy({0.0f, frame.pad * ease(st.t) - ImGui::GetStyle().ItemSpacing.y + 2.0f * s});
    }
    ImGui::Dummy({0.0f, 2.0f * s});
    ImGui::PopID();
}

bool begin_subsection(const char* id, const char* label, bool default_open) {
    const float s = g_scale;
    ImGui::PushID(id);
    const ImGuiID sid = ImGui::GetID("##sub");
    SectionState& st = g_sections[sid];
    if (!st.initialized) {
        st.initialized = true;
        st.open = default_open;
    }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton("##subheader", {width, h})) {
        st.open = !st.open;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = ease(animate(sid, st.open, 10.0f));
    const float angle = t * 1.5708f;
    const ImVec2 cc{start.x + 6.0f * s, start.y + h * 0.5f};
    const float ch = 3.5f * s;
    auto rot = [&](float x, float y) { return ImVec2{cc.x + x * std::cos(angle) - y * std::sin(angle), cc.y + x * std::sin(angle) + y * std::cos(angle)}; };
    const ImU32 col = u32(hovered || st.open ? color::kText : color::kMuted);
    dl->AddLine(rot(-ch * 0.5f, -ch), rot(ch * 0.5f, 0.0f), col, 1.6f * s);
    dl->AddLine(rot(ch * 0.5f, 0.0f), rot(-ch * 0.5f, ch), col, 1.6f * s);
    const ImVec2 text = ImGui::CalcTextSize(label);
    dl->AddText({start.x + 18.0f * s, start.y + (h - text.y) * 0.5f}, col, label);
    dl->AddLine({start.x + 24.0f * s + text.x, start.y + h * 0.5f}, {start.x + width, start.y + h * 0.5f}, u32(color::kBorder), 1.0f);
    if (!st.open) {
        ImGui::PopID();
        return false;
    }
    ImGui::Indent(8.0f * s);
    return true;
}

void end_subsection() {
    ImGui::Unindent(8.0f * g_scale);
    ImGui::PopID();
}

float label_column_width() {
    const float avail = ImGui::GetContentRegionAvail().x;
    return std::clamp(avail * 0.4f, 70.0f * g_scale, 150.0f * g_scale);
}

float begin_row(const char* label, const char* tooltip) {
    const float label_w = label_column_width();
    const float right_pad = g_right_pad_stack.empty() ? 0.0f : 0.0f;
    const float avail = ImGui::GetContentRegionAvail().x - right_pad;
    ImGui::AlignTextToFramePadding();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, color::kMuted);
    // Clip long labels to the column.
    ImGui::PushClipRect(pos, {pos.x + label_w - 4.0f * g_scale, pos.y + ImGui::GetFrameHeight()}, true);
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    ImGui::PopClipRect();
    ImGui::PopStyleColor();
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos({pos.x + label_w, pos.y});
    const float widget_w = std::max(avail - label_w, 40.0f);
    ImGui::SetNextItemWidth(widget_w);
    return widget_w;
}

void end_row() {}

bool segmented(const char* id, int* current, const char* const* labels, int count, float width) {
    const float s = g_scale;
    ImGui::PushID(id);
    const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() + 4.0f * s;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + total, pos.y + h}, u32(color::kWell), h * 0.5f);
    const float seg = total / static_cast<float>(count);
    // Animated highlight position.
    const ImGuiID anim_id = ImGui::GetID("##seg_anim");
    auto [it, inserted] = g_animations.try_emplace(anim_id, static_cast<float>(*current));
    float& slot = it->second;
    slot += (static_cast<float>(*current) - slot) * std::min(1.0f, g_delta * 14.0f);
    const float hx = pos.x + slot * seg + 3.0f * s;
    dl->AddRectFilled({hx, pos.y + 3.0f * s}, {hx + seg - 6.0f * s, pos.y + h - 3.0f * s}, u32(color::kRaised), (h - 6.0f * s) * 0.5f);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos({pos.x + seg * static_cast<float>(i), pos.y});
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", {seg, h})) {
            if (*current != i) {
                *current = i;
                changed = true;
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 text = ImGui::CalcTextSize(labels[i]);
        const ImVec4 fg = i == *current ? color::kLime : (hovered ? color::kText : color::kMuted);
        dl->AddText({pos.x + seg * static_cast<float>(i) + (seg - text.x) * 0.5f, pos.y + (h - text.y) * 0.5f}, u32(fg), labels[i]);
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy({total, h});
    ImGui::PopID();
    return changed;
}


namespace {

void draw_slider_track(float fraction) {
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = std::clamp(fraction, 0.0f, 1.0f);
    const float x = a.x + (b.x - a.x) * t;
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImVec4 fill = color::kLime;
    fill.w = ImGui::IsItemActive() ? 0.22f : (ImGui::IsItemHovered() ? 0.17f : 0.12f);
    if (x > a.x + 1.0f) {
        dl->AddRectFilled(a, {x, b.y}, ImGui::GetColorU32(fill), rounding, x >= b.x - rounding ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
    }
    const float thickness = std::max(2.0f, ImGui::GetFontSize() * 0.14f);
    dl->AddRectFilled({a.x + rounding * 0.5f, b.y - thickness}, {std::max(x, a.x + rounding * 0.5f), b.y}, ImGui::GetColorU32(color::kLime), thickness * 0.5f);
}

} // namespace

bool slider_float(const char* id, float* value, float min, float max, const char* format, bool logarithmic) {
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
    const bool changed = ImGui::SliderFloat(id, value, min, max, format, logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    ImGui::PopStyleColor(2);
    float fraction = 0.0f;
    if (max > min) {
        if (logarithmic && min > 0.0f) {
            fraction = std::log(std::max(*value, min) / min) / std::log(max / min);
        } else {
            fraction = (*value - min) / (max - min);
        }
    }
    draw_slider_track(fraction);
    return changed;
}

bool slider_int(const char* id, int* value, int min, int max) {
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
    const bool changed = ImGui::SliderInt(id, value, min, max);
    ImGui::PopStyleColor(2);
    draw_slider_track(max > min ? static_cast<float>(*value - min) / static_cast<float>(max - min) : 0.0f);
    return changed;
}


void draw_logo(ImDrawList* dl, ImVec2 top_left, float size) {
    // Artwork coordinates are a 100x100 box (see hollow_logo.svg).
    const float k = size / 100.0f;
    auto P = [&](float x, float y) { return ImVec2{top_left.x + x * k, top_left.y + y * k}; };
    const ImU32 lime = u32(color::kLime);
    const ImU32 badge = IM_COL32(12, 13, 15, 255);
    const ImU32 edge = IM_COL32(43, 46, 51, 255);
    const ImU32 shadow = IM_COL32(51, 54, 60, 255);
    const float pi = 3.14159265f;

    // Badge: rounded square with the top-right corner chamfered.
    auto badge_path = [&] {
        dl->PathLineTo(P(14, 4));
        dl->PathLineTo(P(74, 4));
        dl->PathLineTo(P(96, 26));
        dl->PathArcTo(P(86, 86), 10.0f * k, 0.0f, pi * 0.5f);
        dl->PathArcTo(P(14, 86), 10.0f * k, pi * 0.5f, pi);
        dl->PathArcTo(P(14, 14), 10.0f * k, pi, pi * 1.5f);
    };
    badge_path();
    dl->PathFillConvex(badge);
    badge_path();
    dl->PathStroke(edge, ImDrawFlags_Closed, std::max(1.0f, 2.5f * k));
    // Lime corner piece in the chamfer.
    dl->PathLineTo(P(79, 4));
    dl->PathLineTo(P(96, 21));
    dl->PathLineTo(P(96, 13));
    dl->PathArcTo(P(87, 13), 9.0f * k, 0.0f, -pi * 0.5f);
    dl->PathFillConvex(lime);

    // Broken ring: two arcs with gaps along the slash direction.
    const ImVec2 center = P(50, 51);
    const float radius = 24.0f * k;
    const float thickness = 10.5f * k;
    const float angle = -53.0f * pi / 180.0f;
    const float gap = 16.0f * pi / 180.0f;
    for (int layer = 0; layer < 2; ++layer) {
        const ImVec2 c = layer == 0 ? ImVec2{center.x + 3.0f * k, center.y + 3.0f * k} : center;
        for (int half = 0; half < 2; ++half) {
            const float a0 = angle + gap + pi * static_cast<float>(half);
            dl->PathArcTo(c, radius, a0, a0 + pi - 2.0f * gap, 24);
            dl->PathStroke(layer == 0 ? shadow : lime, 0, thickness);
        }
    }
    // The slash.
    const ImVec2 u{std::cos(angle), std::sin(angle)};
    const ImVec2 n{-u.y, u.x};
    const float len = 33.0f * k;
    const float half_w = 2.6f * k;
    dl->AddQuadFilled({center.x + u.x * len + n.x * half_w, center.y + u.y * len + n.y * half_w},
        {center.x + u.x * len - n.x * half_w, center.y + u.y * len - n.y * half_w},
        {center.x - u.x * len - n.x * half_w, center.y - u.y * len - n.y * half_w},
        {center.x - u.x * len + n.x * half_w, center.y - u.y * len + n.y * half_w}, lime);
    // Ticks (only when large enough to read).
    if (size >= 40.0f) {
        for (int i = 0; i < 3; ++i) {
            const float x = 12.0f + static_cast<float>(i) * 6.0f;
            dl->AddRectFilled(P(x, 87), P(x + 3.5f, 90.5f), i == 0 ? lime : IM_COL32(77, 81, 88, 255));
        }
    }
}

} // namespace Hollow::theme

// Implementation of EditorUi: theme/style setup, the hand-drawn toolbar tool
// icons, the toolbar/outliner/details panels, the render-graph and post-FX
// debug windows, and the window-size-driven UI scale (g_ui_scale) used to
// keep hand-drawn elements and hardcoded layout constants in proportion with
// ImGui's own font-scaled widgets. See EditorUi.hpp for how this fits into
// the App main loop.

#include "Hollow/EditorUi.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>

namespace Hollow {
namespace {

// Theme: a cool, mostly-grayscale base (charcoal/graphite panels) with
// exactly two neon accents — a saturated "signal" blue for active/primary
// state and a brighter, cyan-leaning light blue for hover/secondary state —
// referencing an urban/tech-HUD look (Zenless Zone Zero's UI specifically
// was the brief: lots of gray, one or two glowing accent colors). Shared
// here so apply_style(), section_header(), and the hand-drawn toolbar icons
// below all pull from the same two colors instead of each hardcoding their
// own slightly-different blue.
const ImVec4 kAccentBlue{0.20f, 0.55f, 0.95f, 1.0f};
const ImVec4 kAccentLightBlue{0.45f, 0.85f, 1.0f, 1.0f};
const ImVec4 kAccentBlueMutedBg{0.10f, 0.22f, 0.38f, 1.0f};

// Set once per frame at the top of EditorUi::draw() from the window's live
// DisplaySize, then read by the small helpers below (section_header,
// draw_tool_icon call sites, icon_tool_button) to scale their otherwise
// -hardcoded pixel constants. A plain anonymous-namespace global rather than
// threading a `scale` parameter through every one of those helpers — this
// file only has one UI, drawn from one place, so the usual "avoid globals"
// argument (multiple call sites disagreeing on state) doesn't apply, and a
// global keeps this whole scaling pass from turning into a signature-churn
// diff across every helper.
float g_ui_scale = 1.0f;

void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    // Sharply reduced rounding vs. the previous soft flat-UI look — mostly
    // square corners read as more "angular tech HUD," matching the diagonal
    // -cut section headers below rather than a generic rounded-rect theme.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 1.0f * scale;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 1.0f * scale;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = {7.0f * scale, 6.0f * scale};
    style.WindowPadding = {10.0f * scale, 9.0f * scale};
    style.ScrollbarSize = 14.0f * scale;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.058f, 0.066f, 0.97f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.048f, 0.055f, 0.94f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.065f, 0.069f, 0.079f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.25f, 0.31f, 0.9f);
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.96f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.55f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.96f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.19f, 0.27f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.12f, 0.26f, 0.40f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.10f, 0.11f, 0.13f, 0.96f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.21f, 0.31f, 1.0f);
    colors[ImGuiCol_ButtonActive] = kAccentBlueMutedBg;
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.18f, 0.28f, 0.9f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.13f, 0.27f, 0.42f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.42f, 0.68f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.035f, 0.038f, 0.045f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.035f, 0.038f, 0.045f, 1.0f);
    colors[ImGuiCol_CheckMark] = kAccentLightBlue;
    colors[ImGuiCol_SliderGrab] = kAccentBlue;
    colors[ImGuiCol_SliderGrabActive] = kAccentLightBlue;
    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.25f, 0.31f, 0.9f);
    colors[ImGuiCol_SeparatorHovered] = kAccentBlue;
    colors[ImGuiCol_SeparatorActive] = kAccentLightBlue;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.045f, 0.048f, 0.055f, 0.9f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.16f, 0.30f, 0.46f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = kAccentBlue;
}

void apply_editor_font() {
    ImGuiIO& io = ImGui::GetIO();
    std::vector<std::filesystem::path> candidates;

    // Segoe UI ships with every Windows install since Vista, so it's tried
    // first: a clean proportional sans-serif reads as a real application
    // UI, unlike the previous default of using a monospace coding font
    // (JetBrains Mono) for every label in the editor. JetBrains Mono stays
    // as a fallback chain below in case Segoe UI is somehow missing.
    candidates.emplace_back("C:/Windows/Fonts/segoeui.ttf");

    char* local_app_data = nullptr;
    std::size_t local_app_data_length = 0;
    if (_dupenv_s(&local_app_data, &local_app_data_length, "LOCALAPPDATA") == 0 && local_app_data != nullptr) {
        candidates.emplace_back(std::filesystem::path(local_app_data) / "Microsoft" / "Windows" / "Fonts" / "JetBrainsMono-Regular.ttf");
        std::free(local_app_data);
    }

    candidates.emplace_back("C:/Windows/Fonts/JetBrainsMono-Regular.ttf");
    candidates.emplace_back("C:/Program Files/JetBrains/CLion 2025.3.4/jbr/lib/fonts/JetBrainsMono-Regular.ttf");
    candidates.emplace_back("C:/Program Files/JetBrains/JetBrains Rider 2024.3.4/jbr/lib/fonts/JetBrainsMono-Regular.ttf");

    for (const auto& path : candidates) {
        if (!std::filesystem::exists(path)) {
            continue;
        }
        std::string font_path = path.string();
        if (io.Fonts->AddFontFromFileTTF(font_path.c_str(), 15.0f) != nullptr) {
            return;
        }
    }

    io.Fonts->AddFontDefault();
}

// A small diagonal-cut accent "flag" drawn before a section label, standing
// in for ImGui::SeparatorText's plain centered-line style — every
// ImGui::SeparatorText call in this file was replaced with this instead
// (see the many call sites below), since it's the single change that
// carries the angular urban-HUD look into every panel at once.
void section_header(const char* label) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight();
    const float slant = height * 0.6f;
    const float width = 6.0f * g_ui_scale;
    draw_list->AddQuadFilled(
        {pos.x, pos.y + height}, {pos.x + slant, pos.y},
        {pos.x + slant + width, pos.y}, {pos.x + width, pos.y + height},
        ImGui::GetColorU32(kAccentBlue));

    ImGui::SetCursorScreenPos({pos.x + slant + width + 8.0f * g_ui_scale, pos.y});
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentLightBlue);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// Which hand-drawn glyph draw_tool_icon should render. Kept to the 4 main
// transform tools for this pass — the toolbar's toggle checkboxes
// (Snap/Shadows/Outlines/HDR/IBL/...) stay plain text/checkbox controls for
// now, a deliberate scope cut to keep this round bounded.
enum class ToolIcon {
    Select,
    Move,
    Rotate,
    Scale,
};

// Every icon is drawn with plain, long-stable ImDrawList primitives
// (AddLine/AddTriangleFilled) rather than PathArcTo/PathStroke or any
// texture/font asset, since there's no way to visually preview these before
// they land on the user's machine — simple, generously-stroked shapes are
// the safest bet to read correctly at ~13px with zero iteration.
void draw_tool_icon(ImDrawList* draw_list, ToolIcon icon, ImVec2 center, float size, ImU32 color) {
    const float h = size * 0.5f;
    switch (icon) {
    case ToolIcon::Select: {
        // Simple cursor-arrow: a filled triangle "head" plus a short tail
        // stroke, angled like a mouse pointer.
        const ImVec2 tip{center.x - h * 0.7f, center.y - h * 0.9f};
        const ImVec2 right{center.x + h * 0.5f, center.y - h * 0.05f};
        const ImVec2 bottom{center.x - h * 0.15f, center.y + h * 0.4f};
        draw_list->AddTriangleFilled(tip, right, bottom, color);
        draw_list->AddLine({center.x - h * 0.15f, center.y + h * 0.2f}, {center.x + h * 0.35f, center.y + h * 0.9f}, color, 2.0f);
        break;
    }
    case ToolIcon::Move: {
        // A 4-way cross with a small arrowhead at each end — echoes the
        // move gizmo's own arrowhead handles (Renderer::draw_gizmo).
        draw_list->AddLine({center.x - h, center.y}, {center.x + h, center.y}, color, 1.6f);
        draw_list->AddLine({center.x, center.y - h}, {center.x, center.y + h}, color, 1.6f);
        const float a = size * 0.22f;
        draw_list->AddTriangleFilled({center.x + h, center.y}, {center.x + h - a, center.y - a * 0.6f}, {center.x + h - a, center.y + a * 0.6f}, color);
        draw_list->AddTriangleFilled({center.x - h, center.y}, {center.x - h + a, center.y - a * 0.6f}, {center.x - h + a, center.y + a * 0.6f}, color);
        draw_list->AddTriangleFilled({center.x, center.y - h}, {center.x - a * 0.6f, center.y - h + a}, {center.x + a * 0.6f, center.y - h + a}, color);
        draw_list->AddTriangleFilled({center.x, center.y + h}, {center.x - a * 0.6f, center.y + h - a}, {center.x + a * 0.6f, center.y + h - a}, color);
        break;
    }
    case ToolIcon::Rotate: {
        // A ~270-degree arc (built from straight segments, not
        // PathArcTo/PathStroke, to avoid depending on an ImDrawFlags
        // signature that might not match an older vendored ImGui) with a
        // small arrowhead at the open end.
        const float radius = h * 0.75f;
        constexpr int segments = 16;
        constexpr float start_angle = -0.3f;
        constexpr float end_angle = 4.4f;
        ImVec2 previous{center.x + std::cos(start_angle) * radius, center.y + std::sin(start_angle) * radius};
        for (int i = 1; i <= segments; ++i) {
            const float t = start_angle + (end_angle - start_angle) * (static_cast<float>(i) / segments);
            const ImVec2 current{center.x + std::cos(t) * radius, center.y + std::sin(t) * radius};
            draw_list->AddLine(previous, current, color, 1.8f);
            previous = current;
        }
        const float ex = center.x + std::cos(end_angle) * radius;
        const float ey = center.y + std::sin(end_angle) * radius;
        const float a = h * 0.4f;
        draw_list->AddTriangleFilled({ex, ey - a}, {ex + a, ey + a * 0.3f}, {ex - a * 0.3f, ey + a}, color);
        break;
    }
    case ToolIcon::Scale: {
        // Camera-viewfinder-style corner brackets around the tool's
        // bounds — fits the tech-HUD reference better than a plain square
        // outline, and reads clearly as "bounds" even at small size.
        const float s = h * 0.85f;
        const float t = h * 0.4f;
        const ImVec2 corners[4] = {
            {center.x - s, center.y - s}, {center.x + s, center.y - s},
            {center.x - s, center.y + s}, {center.x + s, center.y + s},
        };
        const float dx[4] = {1.0f, -1.0f, 1.0f, -1.0f};
        const float dy[4] = {1.0f, 1.0f, -1.0f, -1.0f};
        for (int i = 0; i < 4; ++i) {
            draw_list->AddLine(corners[i], {corners[i].x + t * dx[i], corners[i].y}, color, 1.8f);
            draw_list->AddLine(corners[i], {corners[i].x, corners[i].y + t * dy[i]}, color, 1.8f);
        }
        break;
    }
    }
}

// Replaces the plain-text tool_button: still a real ImGui::Button for hit
// -testing/hover/active styling (pushed via the same accent colors as the
// rest of the theme), just with an empty "##id"-only visible label so the
// icon and text can be hand-drawn into its rect afterward via the window's
// draw list — the standard technique for custom ImGui button content
// without reimplementing button interaction from scratch.
bool icon_tool_button(const char* id, const char* label, ToolIcon icon, Engine& engine, EditorTool tool) {
    const bool active = engine.active_tool() == tool;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccentBlueMutedBg);
        ImGui::PushStyleColor(ImGuiCol_Border, kAccentBlue);
    }
    const bool clicked = ImGui::Button(id, ImVec2(82.0f * g_ui_scale, 0.0f));
    if (active) {
        ImGui::PopStyleColor(2);
    }

    const ImVec2 rect_min = ImGui::GetItemRectMin();
    const ImVec2 rect_max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float icon_cx = rect_min.x + 15.0f * g_ui_scale;
    const float icon_cy = (rect_min.y + rect_max.y) * 0.5f;
    const ImU32 content_color = active ? ImGui::GetColorU32(kAccentLightBlue) : ImGui::GetColorU32(ImGuiCol_Text);
    draw_tool_icon(draw_list, icon, {icon_cx, icon_cy}, 13.0f * g_ui_scale, content_color);

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    draw_list->AddText({rect_min.x + 30.0f * g_ui_scale, icon_cy - text_size.y * 0.5f}, content_color, label);

    if (clicked) {
        engine.set_active_tool(tool);
    }
    return clicked;
}

// Common preset resolutions for the toolbar's Resolution combo. Picking one
// resizes the real OS window via Win32Window::set_client_size; the renderer
// and every render target it owns already re-derive their size from the
// window every frame (App::run -> Renderer::resize), so nothing else needs
// to change here.
constexpr const char* kResolutionLabels[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"};
constexpr int kResolutionWidths[] = {1280, 1600, 1920, 2560};
constexpr int kResolutionHeights[] = {720, 900, 1080, 1440};
constexpr int kResolutionPresetCount = 4;

void draw_resolution_combo(Win32Window* window) {
    int current_index = -1;
    if (window != nullptr) {
        for (int i = 0; i < kResolutionPresetCount; ++i) {
            if (window->width() == kResolutionWidths[i] && window->height() == kResolutionHeights[i]) {
                current_index = i;
                break;
            }
        }
    }
    const char* preview = current_index >= 0 ? kResolutionLabels[current_index] : "Custom";

    ImGui::TextDisabled("Res");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f * g_ui_scale);
    if (ImGui::BeginCombo("##Resolution", preview)) {
        for (int i = 0; i < kResolutionPresetCount; ++i) {
            const bool selected = i == current_index;
            if (ImGui::Selectable(kResolutionLabels[i], selected) && window != nullptr) {
                window->set_client_size(kResolutionWidths[i], kResolutionHeights[i]);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void transform_editor(Transform& transform) {
    section_header("Transform");
    ImGui::DragFloat3("Position", &transform.translation.x, 0.05f);
    Vec3 degrees = transform.rotation * (180.0f / std::numbers::pi_v<float>);
    if (ImGui::DragFloat3("Rotation", &degrees.x, 0.4f)) {
        transform.rotation = degrees * (std::numbers::pi_v<float> / 180.0f);
    }
    ImGui::DragFloat3("Scale", &transform.scale.x, 0.02f, 0.01f, 100.0f);
}

// Debug view onto the renderer's named render-target registry and declared
// pass reads/writes (Renderer::render_target_info / describe_passes). This
// is a read-only introspection window, not a pass editor.
void draw_render_graph_window(const Renderer& renderer, bool& open) {
    ImGui::SetNextWindowSize({420.0f * g_ui_scale, 360.0f * g_ui_scale}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render Graph", &open)) {
        ImGui::End();
        return;
    }

    section_header("Passes");
    for (const RenderPassDescriptor& pass : renderer.describe_passes()) {
        ImGui::TextUnformatted(pass.name.c_str());
        std::string reads = pass.reads.empty() ? "-" : "";
        for (std::size_t i = 0; i < pass.reads.size(); ++i) {
            reads += pass.reads[i];
            if (i + 1 < pass.reads.size()) {
                reads += ", ";
            }
        }
        std::string writes = pass.writes.empty() ? "-" : "";
        for (std::size_t i = 0; i < pass.writes.size(); ++i) {
            writes += pass.writes[i];
            if (i + 1 < pass.writes.size()) {
                writes += ", ";
            }
        }
        ImGui::TextDisabled("  reads:  %s", reads.c_str());
        ImGui::TextDisabled("  writes: %s", writes.c_str());
    }

    section_header("Resolved Targets");
    const std::vector<RenderTargetInfo> targets = renderer.render_target_info();
    if (targets.empty()) {
        ImGui::TextDisabled("No targets created yet.");
    }
    for (const RenderTargetInfo& target : targets) {
        std::string format;
        if (target.has_color) {
            format += "color";
        }
        if (target.has_depth) {
            format += format.empty() ? "depth" : "+depth";
        }
        ImGui::Text("%s  %dx%d  (%s)", target.name.c_str(), target.width, target.height, format.c_str());
    }
    ImGui::TextDisabled("Point-light shadow cube is not in the registry yet (see AAA_PBR_ROADMAP.md).");

    ImGui::End();
}

// Milestone 6 post-processing controls. Bloom, vignette, grading, and FXAA
// only have a visible effect with HDR on (they run in the HDR tonemap/FXAA
// passes), which is called out inline rather than disabling the whole
// window, since the values are still worth setting up in advance of
// turning HDR back on. Ambient occlusion is independent of HDR — it
// darkens the same ambient term the glTF occlusion texture already does,
// in the main object shader.
void draw_post_fx_window(RenderPipelineSettings& settings, bool& open) {
    ImGui::SetNextWindowSize({300.0f * g_ui_scale, 470.0f * g_ui_scale}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Post FX", &open)) {
        ImGui::End();
        return;
    }

    section_header("Ambient Occlusion");
    ImGui::Checkbox("Enabled##Ao", &settings.ao_enabled);
    ImGui::SliderFloat("Radius", &settings.ao_radius, 0.05f, 2.0f, "%.2f");
    ImGui::SliderFloat("Bias", &settings.ao_bias, 0.0f, 0.1f, "%.3f");
    ImGui::SliderFloat("Strength", &settings.ao_intensity, 0.0f, 2.0f, "%.2f");

    if (!settings.hdr_enabled) {
        ImGui::Spacing();
        ImGui::TextDisabled("Enable HDR to see bloom/vignette/grading/FXAA below.");
    }

    section_header("Bloom");
    ImGui::Checkbox("Enabled##Bloom", &settings.bloom_enabled);
    ImGui::SliderFloat("Threshold", &settings.bloom_threshold, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Intensity", &settings.bloom_intensity, 0.0f, 2.0f, "%.2f");

    section_header("Vignette");
    ImGui::Checkbox("Enabled##Vignette", &settings.vignette_enabled);
    ImGui::SliderFloat("Amount", &settings.vignette_intensity, 0.0f, 1.0f, "%.2f");

    section_header("Color Grading");
    ImGui::SliderFloat("Contrast", &settings.color_grade_contrast, 0.5f, 1.5f, "%.2f");
    ImGui::SliderFloat("Saturation", &settings.color_grade_saturation, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Temperature", &settings.color_grade_temperature, -1.0f, 1.0f, "%.2f");

    section_header("Anti-Aliasing");
    ImGui::Checkbox("FXAA", &settings.fxaa_enabled);

    ImGui::End();
}

} // namespace

void EditorUi::initialize(Win32Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_editor_font();
    apply_style(1.0f);
    ImGui_ImplWin32_Init(window.hwnd());
    ImGui_ImplOpenGL2_Init();
    window_ = &window;
    initialized_ = true;
}

void EditorUi::shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void EditorUi::begin_frame() {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

EditorCommand EditorUi::draw(Scene& scene, Engine& engine, Camera& camera, bool& snap_to_grid, const std::string& status, const Renderer& renderer) {
    EditorCommand command = EditorCommand::None;
    ImGuiIO& io = ImGui::GetIO();

    // Whole-UI scale factor, derived from window height against a 720p
    // baseline (1.0x at 720p and below, growing up to 2.5x on very tall/4K
    // windows). FontGlobalScale handles all ImGui-drawn text; g_ui_scale is
    // read by the handful of helpers above that draw their own primitives or
    // hardcode pixel sizes (section_header, icon_tool_button/draw_tool_icon,
    // the resolution combo), and the layout constants below.
    const float ui_scale = std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 2.5f);
    g_ui_scale = ui_scale;
    io.FontGlobalScale = ui_scale;
    apply_style(ui_scale);

    const float toolbar_height = 44.0f * ui_scale;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({io.DisplaySize.x, toolbar_height});
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Hollow Editor");
    ImGui::SameLine(92.0f * ui_scale);
    ImGui::TextDisabled("Tool");
    ImGui::SameLine();
    icon_tool_button("##tool_select", "Select", ToolIcon::Select, engine, EditorTool::Select);
    ImGui::SameLine();
    icon_tool_button("##tool_move", "Move", ToolIcon::Move, engine, EditorTool::Move);
    ImGui::SameLine();
    icon_tool_button("##tool_rotate", "Rotate", ToolIcon::Rotate, engine, EditorTool::Rotate);
    ImGui::SameLine();
    icon_tool_button("##tool_scale", "Scale", ToolIcon::Scale, engine, EditorTool::Scale);
    ImGui::SameLine();
    if (ImGui::Button(engine.transform_space() == TransformSpace::World ? "World" : "Local")) {
        engine.toggle_transform_space();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap_to_grid);
    ImGui::SameLine();
    ImGui::Checkbox("Shadows", &engine.render_settings().shadows_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Outlines", &engine.render_settings().draw_actor_outlines);
    ImGui::SameLine();
    ImGui::Checkbox("HDR", &engine.render_settings().hdr_enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * ui_scale);
    ImGui::SliderFloat("Exposure", &engine.render_settings().exposure, 0.1f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::Checkbox("IBL", &engine.render_settings().ibl_enabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(renderer.has_environment() ? "Toggles the skybox and image-based ambient lighting." : "Drop a .hdr file onto the window to load an environment first.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * ui_scale);
    ImGui::SliderFloat("Sky", &engine.render_settings().sky_intensity, 0.1f, 2.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dims/brightens just the skybox background, independent of scene Exposure.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Render Graph", &show_render_graph_);
    ImGui::SameLine();
    ImGui::Checkbox("Post FX", &show_post_fx_);
    ImGui::SameLine();
    draw_resolution_combo(window_);
    ImGui::SameLine();
    ImGui::TextDisabled("View: %s", name_of(engine.debug_view()));
    ImGui::SameLine();
    ImGui::TextDisabled("Style: %s", name_of(engine.style()));
    ImGui::SameLine();
    if (renderer.has_environment()) {
        ImGui::TextDisabled("Env: %s", std::filesystem::path(renderer.environment_path()).filename().string().c_str());
    } else {
        ImGui::TextDisabled("Env: none");
    }
    ImGui::End();

    if (show_render_graph_) {
        draw_render_graph_window(renderer, show_render_graph_);
    }
    if (show_post_fx_) {
        draw_post_fx_window(engine.render_settings(), show_post_fx_);
    }

    // Panels are resizable horizontally by dragging their border, using
    // ImGui's own native resize grip rather than a hand-rolled splitter:
    // SetNextWindowSizeConstraints locks the Y axis (min==max==panel_height)
    // so only the width can change, and SetNextWindowSize is now only
    // applied ImGuiCond_FirstUseEver (an initial default) instead of every
    // frame, so ImGui's per-window size state can remember a manual drag
    // across frames instead of being stomped back to the default each frame.
    const float panel_height = io.DisplaySize.y - toolbar_height;
    const float min_panel_width = 190.0f * ui_scale;
    const float max_panel_width = io.DisplaySize.x * 0.32f;

    ImGui::SetNextWindowPos({0.0f, toolbar_height});
    ImGui::SetNextWindowSizeConstraints({min_panel_width, panel_height}, {max_panel_width, panel_height});
    ImGui::SetNextWindowSize({260.0f * ui_scale, panel_height}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Outliner", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Add Object")) {
        ImGui::OpenPopup("AddObjectPopup");
    }
    if (ImGui::BeginPopup("AddObjectPopup")) {
        if (ImGui::MenuItem("Cube")) command = EditorCommand::AddCube;
        if (ImGui::MenuItem("Plane")) command = EditorCommand::AddPlane;
        if (ImGui::MenuItem("Sphere")) command = EditorCommand::AddSphere;
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Light")) {
        ImGui::OpenPopup("AddLightPopup");
    }
    if (ImGui::BeginPopup("AddLightPopup")) {
        if (ImGui::MenuItem("Directional")) command = EditorCommand::AddDirectionalLight;
        if (ImGui::MenuItem("Point")) command = EditorCommand::AddPointLight;
        if (ImGui::MenuItem("Spot")) command = EditorCommand::AddSpotLight;
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        command = EditorCommand::DeleteSelection;
    }

    section_header("Actors");
    for (const auto& actor : scene.actors()) {
        const bool selected = actor->id() == scene.selected_actor_id();
        const char* icon = actor->find_component<LightComponent>() ? "[Light]" : "[Actor]";
        std::string label = std::string(icon) + " " + actor->name();
        if (ImGui::Selectable(label.c_str(), selected)) {
            scene.select_actor(actor->id());
        }
    }
    ImGui::End();

    // Details is right-aligned, so this frame's X position depends on the
    // panel's width — but Begin() (which is what actually knows the live,
    // possibly user-resized width) hasn't run yet. Position from last
    // frame's cached width (details_panel_width_) and refresh the cache
    // right after Begin(); one frame of lag is imperceptible and this is
    // the standard pattern for self-positioning resizable immediate-mode
    // panels.
    ImGui::SetNextWindowPos({io.DisplaySize.x - details_panel_width_, toolbar_height});
    ImGui::SetNextWindowSizeConstraints({min_panel_width, panel_height}, {max_panel_width, panel_height});
    ImGui::SetNextWindowSize({details_panel_width_, panel_height}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    details_panel_width_ = ImGui::GetWindowSize().x;
    if (Actor* selected = scene.selected_actor()) {
        ImGui::Text("Actor: %s", selected->name().c_str());
        ImGui::TextDisabled("Type: %s", selected->display_type().c_str());
        transform_editor(selected->root().local_transform);

        if (auto* light = selected->find_component<LightComponent>()) {
            section_header("Light");
            ImGui::Checkbox("Enabled", &light->enabled);
            ImGui::ColorEdit3("Color", &light->color.x);
            ImGui::SliderFloat("Intensity", &light->intensity, 0.0f, 8.0f);
            if (light->light_kind != LightKind::Directional) {
                ImGui::SliderFloat("Range", &light->range, 0.1f, 30.0f);
            }
            if (light->light_kind == LightKind::Spot) {
                float angle = light->spot_angle_radians * (180.0f / std::numbers::pi_v<float>);
                if (ImGui::SliderFloat("Spot angle", &angle, 5.0f, 90.0f)) {
                    light->spot_angle_radians = angle * (std::numbers::pi_v<float> / 180.0f);
                }
            }
        }

        if (auto* mesh = selected->find_component<StaticMeshComponent>()) {
            section_header("Static Mesh");
            ImGui::Text("Mesh parts: %zu", mesh->parts.size());
            if (!mesh->parts.empty()) {
                section_header("Material");
                Material material = mesh->parts.front().material;
                bool changed = false;
                changed |= ImGui::ColorEdit3("Base color", &material.base_color.x);
                changed |= ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Roughness", &material.roughness, 0.04f, 1.0f);
                changed |= ImGui::SliderFloat("Normal strength", &material.normal_strength, 0.0f, 2.0f);
                // HDR + Float flags let the picker accept values above 1.0, since
                // glTF emissive factors are not clamped to the [0,1] display range.
                changed |= ImGui::ColorEdit3("Emissive", &material.emissive_factor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                changed |= ImGui::SliderFloat("Occlusion strength", &material.occlusion_strength, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Opacity", &material.base_color_alpha, 0.0f, 1.0f);
                {
                    static const char* kAlphaModeNames[] = {"Opaque", "Mask", "Blend"};
                    int alpha_mode_index = static_cast<int>(material.alpha_mode);
                    if (ImGui::Combo("Alpha mode", &alpha_mode_index, kAlphaModeNames, IM_ARRAYSIZE(kAlphaModeNames))) {
                        material.alpha_mode = static_cast<AlphaMode>(alpha_mode_index);
                        changed = true;
                    }
                    if (material.alpha_mode == AlphaMode::Mask) {
                        changed |= ImGui::SliderFloat("Alpha cutoff", &material.alpha_cutoff, 0.0f, 1.0f);
                    }
                }
                if (!material.base_color_texture_path.empty()) {
                    ImGui::TextWrapped("Base texture: %s", material.base_color_texture_path.c_str());
                }
                if (!material.metallic_roughness_texture_path.empty()) {
                    ImGui::TextWrapped("Metallic/roughness texture: %s", material.metallic_roughness_texture_path.c_str());
                }
                if (!material.normal_texture_path.empty()) {
                    ImGui::TextWrapped("Normal texture: %s", material.normal_texture_path.c_str());
                }
                if (!material.emissive_texture_path.empty()) {
                    ImGui::TextWrapped("Emissive texture: %s", material.emissive_texture_path.c_str());
                }
                if (!material.occlusion_texture_path.empty()) {
                    ImGui::TextWrapped("Occlusion texture: %s", material.occlusion_texture_path.c_str());
                }
                if (changed) {
                    for (MeshPart& part : mesh->parts) {
                        part.material = material;
                    }
                }
            }
        }
    } else {
        ImGui::TextDisabled("No actor selected");
    }

    section_header("Camera");
    ImGui::DragFloat3("Target", &camera.target.x, 0.05f);
    ImGui::SliderFloat("Distance", &camera.distance, 1.2f, 80.0f);
    if (ImGui::Button("Home Reset")) {
        command = EditorCommand::ResetScene;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", status.c_str());
    ImGui::End();

    return command;
}

void EditorUi::render() {
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

bool EditorUi::wants_mouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool EditorUi::wants_keyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace Hollow

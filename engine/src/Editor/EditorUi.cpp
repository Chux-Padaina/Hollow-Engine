// Implementation of EditorUi: panel layout, top bar + menus, outliner,
// inspector (Details / Render / Stats), console and the viewport card.
// Widgets and colors come from EditorTheme; component and settings fields
// are drawn through InspectorVisitor from each type's describe().
#include "Hollow/Editor/EditorUi.hpp"

#include "Editor/EditorTheme.hpp"
#include "Editor/PropertyInspector.hpp"
#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"
#include "Hollow/Core/PropertiesJson.hpp"
#include "Hollow/Scene/SceneFactory.hpp"
#include "Hollow/Scripting/Script.hpp"

#include <glad/gl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

namespace Hollow {

using theme::Icon;
using theme::Pill;
namespace col = theme::color;

namespace {

constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;

float ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float approach(float value, bool target, float speed, float dt) {
    const float goal = target ? 1.0f : 0.0f;
    const float step = speed * dt;
    return value < goal ? std::min(goal, value + step) : std::max(goal, value - step);
}

// Begins a panel window at an exact rectangle.
bool begin_panel(const char* name, ImVec2 pos, ImVec2 size, ImGuiWindowFlags extra = 0, ImVec2 padding = {-1, -1}) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    if (padding.x >= 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    }
    const bool open = ImGui::Begin(name, nullptr, kPanelFlags | extra);
    if (padding.x >= 0.0f) {
        ImGui::PopStyleVar();
    }
    return open;
}

Icon actor_icon(const Actor& actor) {
    if (const auto* light = actor.find_component<LightComponent>()) {
        switch (light->light_kind) {
        case LightKind::Directional: return Icon::Sun;
        case LightKind::Spot: return Icon::Spot;
        case LightKind::Point: return Icon::Light;
        }
    }
    if (actor.find_component<CameraComponent>() != nullptr) return Icon::Camera;
    if (actor.find_component<TerrainComponent>() != nullptr) return Icon::Terrain;
    if (actor.find_component<WaterComponent>() != nullptr) return Icon::Water;
    if (actor.find_component<ParticleEmitterComponent>() != nullptr) return Icon::Particles;
    if (actor.find_component<ReflectionProbeComponent>() != nullptr) return Icon::Probe;
    if (actor.find_component<IrradianceVolumeComponent>() != nullptr) return Icon::Volume;
    if (actor.find_component<DecalComponent>() != nullptr) return Icon::Decal;
    if (actor.find_component<AnimatorComponent>() != nullptr) return Icon::Bone;
    if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
        if (!mesh->parts.empty() && mesh->parts.front().mesh) {
            const std::string& source = mesh->parts.front().mesh->source;
            if (source == "builtin:sphere") return Icon::Sphere;
            if (source == "builtin:plane") return Icon::Plane;
        }
        return Icon::Cube;
    }
    if (!actor.children().empty()) return Icon::Folder;
    return Icon::Empty;
}

Icon component_icon(ComponentType type) {
    switch (type) {
    case ComponentType::StaticMesh: return Icon::Cube;
    case ComponentType::Light: return Icon::Light;
    case ComponentType::Camera: return Icon::Camera;
    case ComponentType::RigidBody: return Icon::Physics;
    case ComponentType::Collider: return Icon::Collider;
    case ComponentType::Script: return Icon::Script;
    case ComponentType::ReflectionProbe: return Icon::Probe;
    case ComponentType::IrradianceVolume: return Icon::Volume;
    case ComponentType::Decal: return Icon::Decal;
    case ComponentType::ParticleEmitter: return Icon::Particles;
    case ComponentType::Terrain: return Icon::Terrain;
    case ComponentType::Water: return Icon::Water;
    case ComponentType::Animator: return Icon::Bone;
    case ComponentType::CharacterController: return Icon::Physics;
    case ComponentType::Count: break;
    }
    return Icon::Empty;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Keys whose values differ between two JSON objects (recursively), as a
// JSON object holding the new values.
JsonValue json_diff(const JsonValue& before, const JsonValue& after) {
    JsonValue diff = JsonValue::object();
    for (const auto& [key, value] : after.object_value) {
        const JsonValue* old = before.find(key);
        if (value.is_object()) {
            JsonValue nested = json_diff(old != nullptr ? *old : JsonValue::object(), value);
            if (nested.size() > 0) {
                diff.set(key, std::move(nested));
            }
        } else if (old == nullptr || write_json(*old, false) != write_json(value, false)) {
            diff.set(key, value);
        }
    }
    return diff;
}

void menu_item(const char* label, const char* shortcut, std::vector<EditorCommand>& commands, EditorCommandType type, bool enabled = true) {
    if (ImGui::MenuItem(label, shortcut, false, enabled)) {
        commands.push_back({type});
    }
}

} // namespace

// --- Lifetime -----------------------------------------------------------------------

void EditorUi::initialize(Win32Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // layout is computed
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    theme::load_fonts();
    theme::apply_style(1.0f);
    ImGui_ImplWin32_Init(window.hwnd());
    ImGui_ImplOpenGL3_Init("#version 410 core");
    window_ = &window;
    load_layout();
    initialized_ = true;
}

void EditorUi::shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void EditorUi::begin_frame(float delta_seconds) {
    delta_ = std::clamp(delta_seconds, 0.0f, 0.1f);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // UI scale from the window height: 1.0x at 800 px up to 2.5x on 4K.
    const float height = ImGui::GetIO().DisplaySize.y;
    ui_scale_ = std::clamp(height / 820.0f, 1.0f, 2.5f);
    theme::apply_style(ui_scale_);
    theme::new_frame(delta_);
    ImGui::NewFrame();
}

void EditorUi::render() {
    ImGui::Render();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    glViewport(0, 0, static_cast<GLsizei>(size.x), static_cast<GLsizei>(size.y));
    glClearColor(col::kBackground.x, col::kBackground.y, col::kBackground.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool EditorUi::wants_mouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool EditorUi::wants_keyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

// --- Frame --------------------------------------------------------------------------

std::vector<EditorCommand> EditorUi::draw(EditorContext& ctx) {
    std::vector<EditorCommand> commands;
    frame_history_.push_back(ctx.frame_ms);
    while (frame_history_.size() > 120) {
        frame_history_.pop_front();
    }
    const int open_flags = (outliner_open_ ? 1 : 0) | (inspector_open_ ? 2 : 0) | (show_console ? 4 : 0);
    if (saved_open_flags_ >= 0 && open_flags != saved_open_flags_) {
        save_layout();
    }
    saved_open_flags_ = open_flags;
    outliner_t_ = approach(outliner_t_, outliner_open_, 6.0f, delta_);
    inspector_t_ = approach(inspector_t_, inspector_open_, 6.0f, delta_);
    console_t_ = approach(console_t_, show_console, 6.0f, delta_);

    draw_top_bar(ctx, commands);
    draw_outliner(ctx, commands);
    draw_inspector(ctx, commands);
    draw_console(ctx);
    draw_viewport(ctx, commands);
    draw_splitters();
    if (show_showcase_) {
        draw_showcase_window(ctx);
    }
    return commands;
}

// --- Top bar --------------------------------------------------------------------------

void EditorUi::draw_top_bar(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const float s = ui_scale_;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = 8.0f * s;
    const float height = 46.0f * s;
    if (!begin_panel("##topbar", {margin, margin}, {display.x - margin * 2.0f, height}, ImGuiWindowFlags_NoScrollbar, {10.0f * s, 0.0f})) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float row_h = ImGui::GetFrameHeight();
    const float y = (height - row_h) * 0.5f;

    // Logo (assets/branding/hollow_logo.svg).
    const float logo = 32.0f * s;
    const ImVec2 lp{wpos.x + 8.0f * s, wpos.y + (height - logo) * 0.5f};
    theme::draw_logo(dl, lp, logo);
    ImGui::PushFont(theme::heading_font(), 0.0f);
    ImGui::SetCursorPos({10.0f * s + logo + 10.0f * s, y});
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hollow");
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 6.0f * s);
    const std::string scene_label = ctx.scene_name + (ctx.dirty ? " *" : "");
    theme::chip(scene_label.c_str());
    ImGui::SameLine(0.0f, 14.0f * s);

    // Menus
    ImGui::SetCursorPosY(y);
    draw_menus(ctx, commands);

    // Play controls, centered.
    const bool playing = ctx.engine.playing();
    const float play_w = playing ? 3.0f * row_h + 2.0f * 6.0f * s + 70.0f * s : 96.0f * s;
    ImGui::SetCursorPos({(display.x - margin * 2.0f - play_w) * 0.5f, y});
    if (!playing) {
        if (theme::pill_button("Play##play", Pill::Primary, {96.0f * s, row_h}, Icon::Play, true)) {
            commands.push_back({EditorCommandType::Play});
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play (Ctrl+P): scripts and physics run; Stop restores the scene.");
    } else {
        if (theme::pill_button("Stop##stop", Pill::White, {70.0f * s, row_h}, Icon::Stop, true)) {
            commands.push_back({EditorCommandType::Stop});
        }
        ImGui::SameLine(0.0f, 6.0f * s);
        if (theme::icon_button("##pause", ctx.engine.paused() ? Icon::Play : Icon::Pause, ctx.engine.paused(), ctx.engine.paused() ? "Resume" : "Pause")) {
            commands.push_back({EditorCommandType::Pause});
        }
        ImGui::SameLine(0.0f, 6.0f * s);
        ImGui::BeginDisabled(!ctx.engine.paused());
        if (theme::icon_button("##step", Icon::Step, false, "Step one physics tick")) {
            commands.push_back({EditorCommandType::Step});
        }
        ImGui::EndDisabled();
    }

    // Right side: undo / redo / save and frame stats.
    const RenderStats& stats = ctx.renderer.stats();
    const std::string fps = std::format("{:.0f} fps", ctx.frame_ms > 0.0f ? 1000.0f / ctx.frame_ms : 0.0f);
    const std::string gpu = std::format("GPU {:.2f} ms", stats.gpu_ms);
    const float chip_w = ImGui::CalcTextSize(fps.c_str()).x + ImGui::CalcTextSize(gpu.c_str()).x + 40.0f * s;
    const float right = display.x - margin * 2.0f - 10.0f * s;
    ImGui::SetCursorPos({right - chip_w - 3.0f * (row_h + 4.0f * s) - 10.0f * s, y});
    ImGui::BeginDisabled(!ctx.can_undo || playing);
    if (theme::icon_button("##undo", Icon::Undo, false, "Undo (Ctrl+Z)")) commands.push_back({EditorCommandType::Undo});
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 4.0f * s);
    ImGui::BeginDisabled(!ctx.can_redo || playing);
    if (theme::icon_button("##redo", Icon::Redo, false, "Redo (Ctrl+Y)")) commands.push_back({EditorCommandType::Redo});
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 4.0f * s);
    ImGui::BeginDisabled(playing);
    if (theme::icon_button("##save", Icon::Save, ctx.dirty, "Save (Ctrl+S)")) commands.push_back({EditorCommandType::SaveScene});
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 10.0f * s);
    theme::chip(fps.c_str(), col::kText, col::kRaised);
    ImGui::SameLine(0.0f, 4.0f * s);
    theme::chip(gpu.c_str(), col::kLime, col::kLimeSoft);
    ImGui::End();
}

void EditorUi::draw_menus(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const bool playing = ctx.engine.playing();
    const bool has_selection = ctx.engine.scene().selected_actor() != nullptr;
    auto menu = [&](const char* label, const char* popup) {
        if (theme::pill_button(label, Pill::Ghost)) {
            ImGui::OpenPopup(popup);
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::SetNextWindowPos({min.x, max.y + 6.0f * ui_scale_});
        ImGui::SameLine(0.0f, 2.0f * ui_scale_);
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f * ui_scale_, 8.0f * ui_scale_});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * ui_scale_, 7.0f * ui_scale_});

    menu("File", "##menu_file");
    if (ImGui::BeginPopup("##menu_file")) {
        menu_item("New Scene", "Ctrl+N", commands, EditorCommandType::NewScene, !playing);
        menu_item("New Demo Scene", nullptr, commands, EditorCommandType::NewDemoScene, !playing);
        menu_item("New Showcase Scene", nullptr, commands, EditorCommandType::NewShowcaseScene, !playing);
        menu_item("Open Scene...", "Ctrl+O", commands, EditorCommandType::OpenScene, !playing);
        ImGui::Separator();
        menu_item("Save Scene", "Ctrl+S", commands, EditorCommandType::SaveScene, !playing);
        menu_item("Save Scene As...", "Ctrl+Shift+S", commands, EditorCommandType::SaveSceneAs, !playing);
        ImGui::Separator();
        menu_item("Import Model...", "Ctrl+I", commands, EditorCommandType::ImportModel);
        menu_item("Load HDRI Environment...", nullptr, commands, EditorCommandType::LoadEnvironment);
        ImGui::Separator();
        menu_item("Exit", nullptr, commands, EditorCommandType::Exit);
        ImGui::EndPopup();
    }
    menu("Edit", "##menu_edit");
    if (ImGui::BeginPopup("##menu_edit")) {
        menu_item("Undo", "Ctrl+Z", commands, EditorCommandType::Undo, ctx.can_undo && !playing);
        menu_item("Redo", "Ctrl+Y", commands, EditorCommandType::Redo, ctx.can_redo && !playing);
        ImGui::Separator();
        menu_item("Duplicate", "Ctrl+D", commands, EditorCommandType::Duplicate, has_selection);
        menu_item("Delete", "Del", commands, EditorCommandType::Delete, has_selection);
        menu_item("Focus Selection", "F", commands, EditorCommandType::FocusSelected, has_selection);
        ImGui::Separator();
        ImGui::SetNextItemWidth(120.0f * ui_scale_);
        ImGui::DragFloat("Move snap", &ctx.snap_translate, 0.05f, 0.05f, 10.0f, "%.2f m");
        ImGui::SetNextItemWidth(120.0f * ui_scale_);
        ImGui::DragFloat("Rotate snap", &ctx.snap_rotate_degrees, 1.0f, 1.0f, 90.0f, "%.0f deg");
        ImGui::EndPopup();
    }
    menu("Add", "##menu_add");
    if (ImGui::BeginPopup("##menu_add")) {
        theme::caption("Basic");
        menu_item("Empty Actor", nullptr, commands, EditorCommandType::AddEmpty);
        menu_item("Cube", nullptr, commands, EditorCommandType::AddCube);
        menu_item("Sphere", nullptr, commands, EditorCommandType::AddSphere);
        menu_item("Plane", nullptr, commands, EditorCommandType::AddPlane);
        theme::caption("Lights");
        menu_item("Directional Light (Sun)", nullptr, commands, EditorCommandType::AddDirectionalLight);
        menu_item("Point Light", nullptr, commands, EditorCommandType::AddPointLight);
        menu_item("Spot Light", nullptr, commands, EditorCommandType::AddSpotLight);
        theme::caption("World");
        menu_item("Terrain", nullptr, commands, EditorCommandType::AddTerrain);
        menu_item("Water", nullptr, commands, EditorCommandType::AddWater);
        menu_item("Particle Emitter", nullptr, commands, EditorCommandType::AddParticles);
        menu_item("Decal", nullptr, commands, EditorCommandType::AddDecal);
        theme::caption("Lighting");
        menu_item("Reflection Probe", nullptr, commands, EditorCommandType::AddReflectionProbe);
        menu_item("Irradiance Volume", nullptr, commands, EditorCommandType::AddIrradianceVolume);
        menu_item("Camera", nullptr, commands, EditorCommandType::AddCamera);
        ImGui::EndPopup();
    }
    menu("View", "##menu_view");
    if (ImGui::BeginPopup("##menu_view")) {
        ImGui::MenuItem("Grid", "F10", &ctx.view.grid);
        ImGui::MenuItem("Gizmos", "F9", &ctx.view.gizmos);
        ImGui::MenuItem("Selection outline", nullptr, &ctx.view.outlines);
        ImGui::MenuItem("Light / camera icons", nullptr, &ctx.view.icons);
        ImGui::MenuItem("All colliders", nullptr, &ctx.view.colliders);
        ImGui::Separator();
        ImGui::MenuItem("Outliner", nullptr, &outliner_open_);
        ImGui::MenuItem("Inspector", nullptr, &inspector_open_);
        ImGui::MenuItem("Console", nullptr, &show_console);
        ImGui::MenuItem("Showcase / capture", nullptr, &show_showcase_);
        ImGui::Separator();
        if (ImGui::MenuItem("Hide editor UI", "F11")) ctx.ui_visible = false;
        menu_item("Reset camera", "Home", commands, EditorCommandType::ResetCamera);
        ImGui::EndPopup();
    }
    menu("Render", "##menu_render");
    if (ImGui::BeginPopup("##menu_render")) {
        theme::caption("Debug view");
        for (int i = 0; i < static_cast<int>(DebugView::Count); ++i) {
            const auto view = static_cast<DebugView>(i);
            const std::string shortcut = i < 8 ? "F" + std::to_string(i + 1) : std::string();
            if (ImGui::MenuItem(debug_view_name(view), shortcut.empty() ? nullptr : shortcut.c_str(), ctx.debug_view == view)) {
                ctx.debug_view = view;
            }
        }
        ImGui::Separator();
        menu_item("Bake lighting (probes + volumes)", nullptr, commands, EditorCommandType::BakeLighting);
        menu_item("Reload shaders", "Shift+F5", commands, EditorCommandType::ReloadShaders);
        menu_item("Reload game code (Game.dll)", nullptr, commands, EditorCommandType::ReloadGameModule);
        if (ImGui::MenuItem("Render settings...")) {
            inspector_open_ = true;
            inspector_tab_ = 1;
        }
        if (ImGui::MenuItem("GPU timings...")) {
            inspector_open_ = true;
            inspector_tab_ = 2;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::NewLine();
}

// --- Layout helpers ------------------------------------------------------------------

namespace {

struct Layout {
    float margin;
    float top;
    float bottom;
    float left_w;
    float right_w;
    float console_h;
};

constexpr float kMinViewportWidth = 320.0f;
constexpr float kMinViewportHeight = 180.0f;

Layout compute_layout(float s, float outliner_t, float inspector_t, float console_t, float outliner_size, float inspector_size, float console_size) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    Layout l;
    l.margin = 8.0f * s;
    l.top = l.margin + 46.0f * s + l.margin;
    l.bottom = display.y - l.margin;
    const float rail = 44.0f * s;
    // Keep room for the viewport when the window is small.
    float left = outliner_size * s;
    float right = inspector_size * s;
    const float available_w = display.x - l.margin * 4.0f - kMinViewportWidth * s;
    if (left + right > available_w && left + right > 0.0f) {
        const float k = std::max(available_w, rail * 2.0f) / (left + right);
        left *= k;
        right *= k;
    }
    l.left_w = rail + (std::max(left, rail) - rail) * ease(outliner_t);
    l.right_w = rail + (std::max(right, rail) - rail) * ease(inspector_t);
    const float header = 38.0f * s;
    const float console = std::min(console_size * s, std::max(l.bottom - l.top - l.margin - kMinViewportHeight * s, header));
    l.console_h = header + (std::max(console, header) - header) * ease(console_t);
    return l;
}

} // namespace

// --- Splitters ---------------------------------------------------------------------------

void EditorUi::draw_splitters() {
    const float s = ui_scale_;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Layout l = compute_layout(s, outliner_t_, inspector_t_, console_t_, outliner_size_, inspector_size_, console_size_);
    const float left_edge = l.margin + l.left_w;              // outliner's right edge
    const float right_edge = display.x - l.margin - l.right_w; // inspector's left edge
    const float console_top = l.bottom - l.console_h;

    struct Handle {
        int id;
        bool visible;
        ImVec2 pos;
        ImVec2 size;
        bool vertical; // drag horizontally
    };
    const Handle handles[] = {
        {1, outliner_open_ && outliner_t_ > 0.99f, {left_edge, l.top}, {l.margin, l.bottom - l.top}, true},
        {2, inspector_open_ && inspector_t_ > 0.99f, {right_edge - l.margin, l.top}, {l.margin, l.bottom - l.top}, true},
        {3, show_console && console_t_ > 0.99f, {left_edge + l.margin, console_top - l.margin}, {std::max(right_edge - left_edge - l.margin * 2.0f, 1.0f), l.margin}, false},
    };
    bool released = false;
    for (const Handle& h : handles) {
        if (!h.visible) {
            continue;
        }
        const std::string name = "##splitter" + std::to_string(h.id);
        ImGui::SetNextWindowPos(h.pos);
        ImGui::SetNextWindowSize(h.size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0f, 1.0f});
        ImGui::Begin(name.c_str(), nullptr, kPanelFlags | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos(h.pos);
        ImGui::InvisibleButton("##grab", h.size);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (hovered || active) {
            ImGui::SetMouseCursor(h.vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        }
        float* size = h.id == 1 ? &outliner_size_ : (h.id == 2 ? &inspector_size_ : &console_size_);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemActivated()) {
            // Absolute dragging from where the press started: robust at any frame rate.
            splitter_start_size_ = *size;
            splitter_start_mouse_ = h.vertical ? mouse.x : mouse.y;
        }
        if (active) {
            active_splitter_ = h.id;
            const float moved = ((h.vertical ? mouse.x : mouse.y) - splitter_start_mouse_) / s;
            if (h.id == 1) {
                *size = std::clamp(splitter_start_size_ + moved, 160.0f, 700.0f);
            } else if (h.id == 2) {
                *size = std::clamp(splitter_start_size_ - moved, 260.0f, 800.0f);
            } else {
                *size = std::clamp(splitter_start_size_ - moved, 80.0f, 900.0f);
            }
        }
        if (ImGui::IsItemDeactivated()) {
            released = true;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Double-click restores the default size.
            if (h.id == 1) outliner_size_ = 250.0f;
            if (h.id == 2) inspector_size_ = 340.0f;
            if (h.id == 3) console_size_ = 170.0f;
            released = true;
        }
        // A lime grip line while hovered or dragging.
        if (hovered || active) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float t = 2.0f * s;
            ImVec4 c = col::kLime;
            c.w = active ? 1.0f : 0.7f;
            const ImVec2 center{h.pos.x + h.size.x * 0.5f, h.pos.y + h.size.y * 0.5f};
            if (h.vertical) {
                const float len = std::min(h.size.y - 24.0f * s, 120.0f * s);
                dl->AddRectFilled({center.x - t * 0.5f, center.y - len * 0.5f}, {center.x + t * 0.5f, center.y + len * 0.5f}, theme::u32(c), t);
            } else {
                const float len = std::min(h.size.x - 24.0f * s, 120.0f * s);
                dl->AddRectFilled({center.x - len * 0.5f, center.y - t * 0.5f}, {center.x + len * 0.5f, center.y + t * 0.5f}, theme::u32(c), t);
            }
        }
        ImGui::End();
    }
    if (released) {
        active_splitter_ = 0;
        save_layout();
    }
}

// Panel sizes and open states persist in %LOCALAPPDATA%/HollowEngine/editor_layout.json.
namespace {

std::filesystem::path layout_file() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (base == nullptr || *base == 0) {
        return {};
    }
    return std::filesystem::path(base) / L"HollowEngine" / L"editor_layout.json";
}

} // namespace

void EditorUi::load_layout() {
    const std::filesystem::path path = layout_file();
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        return;
    }
    try {
        const JsonValue json = parse_json(read_text_file(path));
        outliner_size_ = std::clamp(json.at("outliner_width").as_float(outliner_size_), 160.0f, 700.0f);
        inspector_size_ = std::clamp(json.at("inspector_width").as_float(inspector_size_), 260.0f, 800.0f);
        console_size_ = std::clamp(json.at("console_height").as_float(console_size_), 80.0f, 900.0f);
        outliner_open_ = json.at("outliner_open").as_bool(true);
        inspector_open_ = json.at("inspector_open").as_bool(true);
        show_console = json.at("console_open").as_bool(true);
        outliner_t_ = outliner_open_ ? 1.0f : 0.0f;
        inspector_t_ = inspector_open_ ? 1.0f : 0.0f;
        console_t_ = show_console ? 1.0f : 0.0f;
    } catch (const std::exception&) {
        // A broken layout file just means defaults.
    }
}

void EditorUi::save_layout() const {
    const std::filesystem::path path = layout_file();
    if (path.empty()) {
        return;
    }
    try {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        JsonValue json = JsonValue::object();
        json.set("outliner_width", outliner_size_);
        json.set("inspector_width", inspector_size_);
        json.set("console_height", console_size_);
        json.set("outliner_open", outliner_open_);
        json.set("inspector_open", inspector_open_);
        json.set("console_open", show_console);
        write_text_file(path, write_json(json));
    } catch (const std::exception&) {
    }
}

// --- Outliner -----------------------------------------------------------------------------

bool EditorUi::matches_filter(const Scene& scene, ActorId id) const {
    if (outliner_filter_.empty()) {
        return true;
    }
    const Actor* actor = scene.find(id);
    if (actor == nullptr) {
        return false;
    }
    if (lower(actor->name()).find(lower(outliner_filter_)) != std::string::npos) {
        return true;
    }
    for (ActorId child : actor->children()) {
        if (matches_filter(scene, child)) {
            return true;
        }
    }
    return false;
}

void EditorUi::draw_outliner(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const float s = ui_scale_;
    const Layout l = compute_layout(s, outliner_t_, inspector_t_, console_t_, outliner_size_, inspector_size_, console_size_);
    const ImVec2 pos{l.margin, l.top};
    const ImVec2 size{l.left_w, l.bottom - l.top};
    if (!begin_panel("##outliner", pos, size, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    Scene& scene = ctx.engine.scene();
    if (outliner_t_ < 0.5f && !outliner_open_) {
        // Collapsed rail.
        if (theme::icon_button("##expand", Icon::Layers, false, "Show outliner")) outliner_open_ = true;
        if (theme::icon_button("##add_rail", Icon::Plus, false, "Add")) ImGui::OpenPopup("##add_actor");
    } else {
        theme::title("Scene");
        ImGui::SameLine();
        const std::string count = std::to_string(scene.actor_count());
        theme::chip(count.c_str());
        const float button = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - button * 2.0f - 4.0f * s);
        if (theme::icon_button("##add", Icon::Plus, false, "Add actor")) ImGui::OpenPopup("##add_actor");
        ImGui::SameLine(0.0f, 4.0f * s);
        if (theme::icon_button("##collapse", Icon::Chevron, false, "Collapse")) outliner_open_ = false;

        // Search
        ImGui::SetNextItemWidth(-1.0f);
        const ImVec2 search_pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {28.0f * s, 6.0f * s});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight());
        ImGui::InputTextWithHint("##search", "Search actors", &outliner_filter_);
        ImGui::PopStyleVar(2);
        theme::draw_icon(ImGui::GetWindowDrawList(), Icon::Search, {search_pos.x + 14.0f * s, search_pos.y + ImGui::GetItemRectSize().y * 0.5f}, 12.0f * s,
            theme::u32(col::kMuted));
        ImGui::Dummy({0.0f, 2.0f * s});

        ImGui::BeginChild("##tree", {0.0f, 0.0f}, ImGuiChildFlags_None, ImGuiWindowFlags_None);
        for (ActorId root : scene.root_actors()) {
            draw_outliner_node(ctx, commands, root, 0);
        }
        // Dropping onto the empty space below the tree un-parents.
        const ImVec2 remaining = ImGui::GetContentRegionAvail();
        ImGui::Dummy({std::max(remaining.x, 1.0f), std::max(remaining.y, 30.0f * s)});
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HOLLOW_ACTOR")) {
                commands.push_back({EditorCommandType::Reparent, *static_cast<const ActorId*>(payload->Data), kNoActor});
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemClicked()) {
            scene.clear_selection();
        }
        ImGui::EndChild();
    }
    if (ImGui::BeginPopup("##add_actor")) {
        menu_item("Empty Actor", nullptr, commands, EditorCommandType::AddEmpty);
        menu_item("Cube", nullptr, commands, EditorCommandType::AddCube);
        menu_item("Sphere", nullptr, commands, EditorCommandType::AddSphere);
        menu_item("Plane", nullptr, commands, EditorCommandType::AddPlane);
        ImGui::Separator();
        menu_item("Directional Light", nullptr, commands, EditorCommandType::AddDirectionalLight);
        menu_item("Point Light", nullptr, commands, EditorCommandType::AddPointLight);
        menu_item("Spot Light", nullptr, commands, EditorCommandType::AddSpotLight);
        ImGui::Separator();
        menu_item("Terrain", nullptr, commands, EditorCommandType::AddTerrain);
        menu_item("Water", nullptr, commands, EditorCommandType::AddWater);
        menu_item("Particle Emitter", nullptr, commands, EditorCommandType::AddParticles);
        menu_item("Decal", nullptr, commands, EditorCommandType::AddDecal);
        menu_item("Reflection Probe", nullptr, commands, EditorCommandType::AddReflectionProbe);
        menu_item("Irradiance Volume", nullptr, commands, EditorCommandType::AddIrradianceVolume);
        menu_item("Camera", nullptr, commands, EditorCommandType::AddCamera);
        ImGui::EndPopup();
    }
    ImGui::End();
}

void EditorUi::draw_outliner_node(EditorContext& ctx, std::vector<EditorCommand>& commands, ActorId id, int depth) {
    Scene& scene = ctx.engine.scene();
    Actor* actor = scene.find(id);
    if (actor == nullptr || !matches_filter(scene, id)) {
        return;
    }
    const float s = ui_scale_;
    ImGui::PushID(static_cast<int>(id));
    const bool selected = scene.selected_actor_id() == id;
    const bool has_children = !actor->children().empty();
    const ImGuiID open_id = ImGui::GetID("##open");
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool open = storage->GetBool(open_id, true) || !outliner_filter_.empty();

    const float row_h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##row", {width, row_h});
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        scene.select_actor(id);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        commands.push_back({EditorCommandType::FocusSelected});
    }
    // Drag an actor onto another to parent it.
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("HOLLOW_ACTOR", &id, sizeof(ActorId));
        ImGui::Text("Move %s", actor->name().c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HOLLOW_ACTOR")) {
            commands.push_back({EditorCommandType::Reparent, *static_cast<const ActorId*>(payload->Data), id});
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##context")) {
        scene.select_actor(id);
        menu_item("Duplicate", "Ctrl+D", commands, EditorCommandType::Duplicate);
        menu_item("Delete", "Del", commands, EditorCommandType::Delete);
        if (actor->parent() != kNoActor && ImGui::MenuItem("Unparent")) commands.push_back({EditorCommandType::Reparent, id, kNoActor});
        menu_item("Focus", "F", commands, EditorCommandType::FocusSelected);
        ImGui::EndPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hover_t = theme::animate(ImGui::GetID("##hover"), hovered || selected, 16.0f);
    if (selected) {
        dl->AddRectFilled(pos, {pos.x + width, pos.y + row_h}, theme::u32(col::kLimeSoft), 7.0f * s);
        dl->AddRectFilled({pos.x, pos.y + 5.0f * s}, {pos.x + 3.0f * s, pos.y + row_h - 5.0f * s}, theme::u32(col::kLime), 2.0f * s);
    } else if (hover_t > 0.0f) {
        dl->AddRectFilled(pos, {pos.x + width, pos.y + row_h}, theme::u32(col::kRaised, hover_t), 7.0f * s);
    }
    const float indent = 14.0f * s * static_cast<float>(depth);
    float x = pos.x + 8.0f * s + indent;
    const float cy = pos.y + row_h * 0.5f;
    // Expand arrow
    if (has_children) {
        const ImVec2 arrow_min{x - 2.0f * s, pos.y};
        ImGui::SetCursorScreenPos(arrow_min);
        if (ImGui::InvisibleButton("##arrow", {14.0f * s, row_h})) {
            open = !open;
            storage->SetBool(open_id, open);
        }
        theme::draw_icon(dl, open ? Icon::ChevronDown : Icon::Chevron, {x + 4.0f * s, cy}, 10.0f * s, theme::u32(col::kMuted));
    }
    x += 14.0f * s;
    const ImVec4 icon_color = selected ? col::kLime : (actor->active ? ImVec4(0.75f, 0.75f, 0.78f, 1.0f) : col::kDim);
    theme::draw_icon(dl, actor_icon(*actor), {x + 6.0f * s, cy}, 13.0f * s, theme::u32(icon_color));
    x += 18.0f * s;
    const ImVec2 text = ImGui::CalcTextSize(actor->name().c_str());
    dl->AddText({x, cy - text.y * 0.5f}, theme::u32(actor->active ? (selected ? col::kWhite : col::kText) : col::kDim), actor->name().c_str());
    // Visibility toggle on hover (or when hidden).
    if (hovered || !actor->active) {
        const float button = row_h * 0.8f;
        ImGui::SetCursorScreenPos({pos.x + width - button - 4.0f * s, pos.y + (row_h - button) * 0.5f});
        if (theme::icon_button("##active", actor->active ? Icon::Eye : Icon::EyeOff, false, actor->active ? "Hide" : "Show", button)) {
            if (ctx.begin_edit) ctx.begin_edit();
            actor->active = !actor->active;
            if (ctx.end_edit) ctx.end_edit();
        }
    }
    ImGui::SetCursorScreenPos({pos.x, pos.y + row_h + 1.0f * s});
    ImGui::Dummy({0.0f, 0.0f});
    if (open && has_children) {
        const std::vector<ActorId> children = actor->children();
        for (ActorId child : children) {
            draw_outliner_node(ctx, commands, child, depth + 1);
        }
    }
    ImGui::PopID();
}

// --- Inspector --------------------------------------------------------------------------------

void EditorUi::draw_inspector(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const float s = ui_scale_;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Layout l = compute_layout(s, outliner_t_, inspector_t_, console_t_, outliner_size_, inspector_size_, console_size_);
    const ImVec2 pos{display.x - l.margin - l.right_w, l.top};
    const ImVec2 size{l.right_w, l.bottom - l.top};
    if (!begin_panel("##inspector", pos, size, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    if (inspector_t_ < 0.5f && !inspector_open_) {
        if (theme::icon_button("##expand", Icon::Gear, false, "Show inspector")) inspector_open_ = true;
        if (theme::icon_button("##details_rail", Icon::Cube, false, "Details")) {
            inspector_open_ = true;
            inspector_tab_ = 0;
        }
        if (theme::icon_button("##render_rail", Icon::Sparkle, false, "Render settings")) {
            inspector_open_ = true;
            inspector_tab_ = 1;
        }
        if (theme::icon_button("##stats_rail", Icon::Chart, false, "Stats")) {
            inspector_open_ = true;
            inspector_tab_ = 2;
        }
        ImGui::End();
        return;
    }
    static const char* const kTabs[] = {"Details", "Render", "Stats"};
    const float button = ImGui::GetFrameHeight();
    theme::segmented("##tabs", &inspector_tab_, kTabs, 3, ImGui::GetContentRegionAvail().x - button - 6.0f * s);
    ImGui::SameLine(0.0f, 6.0f * s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f * s);
    if (theme::icon_button("##collapse", Icon::Chevron, false, "Collapse")) inspector_open_ = false;
    ImGui::Dummy({0.0f, 2.0f * s});

    ImGui::BeginChild("##inspector_body", {0.0f, 0.0f}, ImGuiChildFlags_None, ImGuiWindowFlags_None);
    switch (inspector_tab_) {
    case 0: draw_details(ctx, commands); break;
    case 1: draw_render_settings(ctx, commands); break;
    default: draw_stats(ctx, commands); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

void EditorUi::draw_details(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const float s = ui_scale_;
    Actor* actor = ctx.engine.scene().selected_actor();
    if (actor == nullptr) {
        ImGui::Dummy({0.0f, 12.0f * s});
        theme::caption("Nothing selected");
        ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
        ImGui::TextWrapped("Click an object in the viewport or the outliner. Right-drag orbits, middle-drag pans, the wheel zooms.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0f, 8.0f * s});
        if (theme::begin_section("camera", "Editor camera", true, nullptr, Icon::Camera, true)) {
            InspectorCallbacks none;
            InspectorVisitor v(none, nullptr, false);
            v.vec3("target", "Target", ctx.camera.target, {0.0f, 0.0f, 0.05f, "%.2f", false});
            v.number("distance", "Distance", ctx.camera.distance, {0.5f, 200.0f, 0.1f, "%.1f m", true, true});
            v.angle("fov", "Field of view", ctx.camera.fov_y_radians, 20.0f, 120.0f);
        }
        theme::end_section();
        if (!ctx.status.empty()) {
            ImGui::Dummy({0.0f, 4.0f * s});
            ImGui::PushStyleColor(ImGuiCol_Text, col::kMuted);
            ImGui::TextWrapped("%s", ctx.status.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    // Header card: icon, name, active toggle, type + id.
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight() * 2.0f + 18.0f * s;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(start, {start.x + width, start.y + height}, theme::u32(col::kRaised, 0.55f), 10.0f * s);
        const float badge = ImGui::GetFrameHeight() * 1.5f;
        const ImVec2 bp{start.x + 10.0f * s, start.y + (height - badge) * 0.5f};
        dl->AddRectFilled(bp, {bp.x + badge, bp.y + badge}, theme::u32(col::kLimeSoft), 9.0f * s);
        theme::draw_icon(dl, actor_icon(*actor), {bp.x + badge * 0.5f, bp.y + badge * 0.5f}, badge * 0.5f, theme::u32(col::kLime));
        const float x = 10.0f * s + badge + 10.0f * s;
        ImGui::SetCursorScreenPos({start.x + x, start.y + 8.0f * s});
        std::string name = actor->name();
        const float toggle_w = ImGui::GetFrameHeight() * 1.6f;
        ImGui::SetNextItemWidth(width - x - toggle_w - 20.0f * s);
        const std::string before_name = name;
        ImGui::PushFont(theme::heading_font(), 0.0f);
        if (ImGui::InputText("##name", &name)) {
            actor->set_name(name);
        }
        ImGui::PopFont();
        if (ImGui::IsItemActivated() && ctx.begin_edit) {
            actor->set_name(before_name);
            ctx.begin_edit();
            actor->set_name(name);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && ctx.end_edit) {
            ctx.end_edit();
        } else if (ImGui::IsItemDeactivated() && ctx.cancel_edit) {
            ctx.cancel_edit();
        }
        ImGui::SameLine(0.0f, 8.0f * s);
        bool active = actor->active;
        if (theme::toggle("##active", &active)) {
            actor->active = !active;
            if (ctx.begin_edit) ctx.begin_edit();
            actor->active = active;
            if (ctx.end_edit) ctx.end_edit();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active");
        ImGui::SetCursorScreenPos({start.x + x, start.y + 10.0f * s + ImGui::GetFrameHeight()});
        theme::chip(actor->display_type().c_str());
        ImGui::SameLine(0.0f, 4.0f * s);
        const std::string id_text = "#" + std::to_string(actor->id());
        theme::chip(id_text.c_str(), col::kDim, col::kWell);
        if (actor->parent() != kNoActor) {
            if (const Actor* parent = ctx.engine.scene().find(actor->parent())) {
                ImGui::SameLine(0.0f, 4.0f * s);
                const std::string parent_text = "in " + parent->name();
                theme::chip(parent_text.c_str(), col::kMuted, col::kWell);
            }
        }
        ImGui::SetCursorScreenPos({start.x, start.y + height + 6.0f * s});
        ImGui::Dummy({0.0f, 0.0f});
    }

    InspectorCallbacks callbacks{ctx.begin_edit, ctx.end_edit, ctx.cancel_edit, nullptr};
    if (theme::begin_section("transform", "Transform", true, nullptr, Icon::Move, true)) {
        InspectorVisitor v(callbacks, nullptr, false);
        Transform& t = actor->transform;
        v.vec3("position", "Position", t.translation, {0.0f, 0.0f, 0.02f, "%.2f", false});
        Vec3 degrees = t.rotation * kRadToDeg;
        const Vec3 before = degrees;
        v.vec3("rotation", "Rotation", degrees, {0.0f, 0.0f, 0.4f, "%.1f", false});
        if (degrees != before) {
            t.rotation = degrees * kDegToRad;
        }
        v.vec3("scale", "Scale", t.scale, {0.001f, 1000.0f, 0.01f, "%.2f", false});
    }
    theme::end_section();

    for (std::size_t i = 0; i < actor->components().size(); ++i) {
        draw_component(ctx, commands, *actor, i);
    }

    ImGui::Dummy({0.0f, 4.0f * s});
    if (theme::pill_button("Add component", Pill::Secondary, {-1.0f, ImGui::GetFrameHeight() + 4.0f * s}, Icon::Plus, true)) {
        ImGui::OpenPopup("##add_component");
    }
    if (ImGui::BeginPopup("##add_component")) {
        auto item = [&](ComponentType type) {
            const bool allowed = component_allows_multiple(type) || !actor->has_component(type);
            if (ImGui::MenuItem(component_type_name(type), nullptr, false, allowed)) {
                commands.push_back({EditorCommandType::AddComponent, actor->id(), kNoActor, static_cast<int>(type)});
            }
        };
        theme::caption("Rendering");
        for (ComponentType type : {ComponentType::StaticMesh, ComponentType::Light, ComponentType::Camera, ComponentType::Decal, ComponentType::ParticleEmitter,
                 ComponentType::Animator}) {
            item(type);
        }
        theme::caption("World");
        for (ComponentType type : {ComponentType::Terrain, ComponentType::Water, ComponentType::ReflectionProbe, ComponentType::IrradianceVolume}) {
            item(type);
        }
        theme::caption("Gameplay");
        for (ComponentType type : {ComponentType::RigidBody, ComponentType::Collider, ComponentType::CharacterController, ComponentType::Script}) {
            item(type);
        }
        ImGui::EndPopup();
    }
}

void EditorUi::draw_component(EditorContext& ctx, std::vector<EditorCommand>& commands, Actor& actor, std::size_t index) {
    Component& component = *actor.components()[index];
    ImGui::PushID(static_cast<int>(index));
    bool remove = false;
    const bool open = theme::begin_section_action(component_type_key(component.type()), component_type_name(component.type()), true, component_icon(component.type()),
        Icon::Trash, "Remove component", &remove);
    if (remove) {
        commands.push_back({EditorCommandType::RemoveComponent, actor.id(), kNoActor, static_cast<int>(index)});
    }
    if (open) {
        InspectorCallbacks callbacks{ctx.begin_edit, ctx.end_edit, ctx.cancel_edit, [this](const char*) -> std::string {
                                         if (window_ == nullptr) return {};
                                         const std::filesystem::path path = window_->open_file_dialog(
                                             L"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2\0All files\0*.*\0", L"Choose a texture");
                                         return path.empty() ? std::string() : to_utf8(path);
                                     }};
        switch (component.type()) {
        case ComponentType::StaticMesh:
            draw_static_mesh(ctx, actor, static_cast<StaticMeshComponent&>(component));
            break;
        case ComponentType::Script:
            draw_script(ctx, static_cast<ScriptComponent&>(component));
            break;
        default: {
            InspectorVisitor v(callbacks, &component.revision, false);
            component.describe(v);
            if (component.type() == ComponentType::Light) {
                auto& light = static_cast<LightComponent&>(component);
                ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
                ImGui::TextWrapped(light.light_kind == LightKind::Point ? "Shines in every direction." : "Shines along the actor's local -Y axis.");
                ImGui::PopStyleColor();
            }
            if (component.type() == ComponentType::Collider) {
                if (theme::pill_button("Fit to mesh", Pill::Secondary)) {
                    if (ctx.begin_edit) ctx.begin_edit();
                    fit_collider_to_mesh(actor, static_cast<ColliderComponent&>(component));
                    if (ctx.end_edit) ctx.end_edit();
                }
            }
            if (component.type() == ComponentType::ReflectionProbe || component.type() == ComponentType::IrradianceVolume) {
                if (ctx.renderer.baking()) {
                    ImGui::ProgressBar(ctx.renderer.bake_progress(), {-1.0f, 0.0f}, "Baking...");
                } else if (theme::pill_button("Bake now", Pill::Primary, {-1.0f, 0.0f}, Icon::Bake, true)) {
                    commands.push_back({EditorCommandType::BakeLighting});
                }
            }
            if (component.type() == ComponentType::Animator) {
                auto& animator = static_cast<AnimatorComponent&>(component);
                if (animator.skeleton && !animator.skeleton->clips.empty()) {
                    theme::caption("Clips");
                    for (const AnimationClip& clip : animator.skeleton->clips) {
                        const bool current = clip.name == animator.active_clip;
                        const std::string label = std::format("{}  ({:.1f}s)", clip.name, clip.duration);
                        if (ImGui::Selectable(label.c_str(), current)) {
                            if (ctx.begin_edit) ctx.begin_edit();
                            animator.clip = clip.name;
                            if (ctx.end_edit) ctx.end_edit();
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
                    ImGui::TextWrapped("Needs a skinned mesh (an imported glTF with a skin and animations) on this actor.");
                    ImGui::PopStyleColor();
                }
            }
            if (component.type() == ComponentType::RigidBody && ctx.engine.playing()) {
                const auto& body = static_cast<RigidBodyComponent&>(component);
                ImGui::TextDisabled("Velocity %.2f %.2f %.2f %s", body.velocity.x, body.velocity.y, body.velocity.z, body.grounded ? "(grounded)" : "");
            }
            if (component.type() == ComponentType::RigidBody && !actor.has_component(ComponentType::Collider)) {
                ImGui::TextColored(col::kWarning, "Needs a Collider to collide.");
            }
            if (component.type() == ComponentType::CharacterController) {
                const auto& character = static_cast<CharacterControllerComponent&>(component);
                if (ctx.engine.playing()) {
                    ImGui::TextDisabled("Velocity %.2f %.2f %.2f %s", character.velocity.x, character.velocity.y, character.velocity.z,
                        character.grounded ? "(grounded)" : "(airborne)");
                }
                if (actor.has_component(ComponentType::RigidBody)) {
                    ImGui::TextColored(col::kWarning, "A character ignores this actor's Rigid Body.");
                }
                ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
                ImGui::TextWrapped("Moved by scripts through its velocity (PlayerController drives it). Feet at the actor origin.");
                ImGui::PopStyleColor();
            }
            break;
        }
        }
    }
    theme::end_section();
    ImGui::PopID();
}

void EditorUi::draw_static_mesh(EditorContext& ctx, Actor& actor, StaticMeshComponent& mesh) {
    InspectorCallbacks callbacks{ctx.begin_edit, ctx.end_edit, ctx.cancel_edit, [this](const char*) -> std::string {
                                     if (window_ == nullptr) return {};
                                     const std::filesystem::path path = window_->open_file_dialog(
                                         L"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2\0All files\0*.*\0", L"Choose a texture");
                                     return path.empty() ? std::string() : to_utf8(path);
                                 }};
    {
        InspectorVisitor v(callbacks, nullptr, false);
        v.boolean("visible", "Visible", mesh.visible);
        v.boolean("cast_shadows", "Cast shadows", mesh.cast_shadows);
    }
    if (mesh.parts.empty()) {
        theme::caption("No mesh parts.");
        return;
    }
    // Mesh info chips.
    std::size_t triangles = 0;
    std::size_t lods = 1;
    for (const MeshPart& part : mesh.parts) {
        if (part.mesh) {
            triangles += part.mesh->triangle_count();
            lods = std::max(lods, part.mesh->lods.size());
        }
    }
    const std::string tri_text = std::format("{} tris", triangles);
    const std::string part_text = std::format("{} part{}", mesh.parts.size(), mesh.parts.size() == 1 ? "" : "s");
    const std::string lod_text = std::format("{} LOD{}", lods, lods == 1 ? "" : "s");
    theme::chip(part_text.c_str());
    ImGui::SameLine(0.0f, 4.0f);
    theme::chip(tri_text.c_str());
    ImGui::SameLine(0.0f, 4.0f);
    theme::chip(lod_text.c_str(), lods > 1 ? col::kLime : col::kMuted, lods > 1 ? col::kLimeSoft : col::kRaised);

    if (material_part_actor_ != actor.id() || material_part_ >= static_cast<int>(mesh.parts.size())) {
        material_part_actor_ = actor.id();
        material_part_ = mesh.parts.size() > 1 ? -1 : 0;
    }
    if (mesh.parts.size() > 1) {
        theme::begin_row("Material of");
        const std::string preview = material_part_ < 0 ? std::string("All parts") : mesh.parts[static_cast<std::size_t>(material_part_)].name;
        if (ImGui::BeginCombo("##part", preview.c_str())) {
            if (ImGui::Selectable("All parts", material_part_ < 0)) material_part_ = -1;
            for (int i = 0; i < static_cast<int>(mesh.parts.size()); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(mesh.parts[static_cast<std::size_t>(i)].name.c_str(), material_part_ == i)) material_part_ = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    // Edit a reference part; with "All parts" only the changed fields are
    // copied to every other part (textures etc. stay per part).
    const int part = material_part_;
    Material& reference = mesh.parts[static_cast<std::size_t>(std::max(part, 0))].material;
    const JsonValue before = properties_to_json(reference);
    InspectorVisitor v(callbacks, nullptr, false);
    describe(reference, v);
    if (part < 0 && v.changed()) {
        const JsonValue diff = json_diff(before, properties_to_json(reference));
        if (diff.size() > 0) {
            for (std::size_t i = 1; i < mesh.parts.size(); ++i) {
                properties_from_json(mesh.parts[i].material, diff);
            }
        }
    }
}

void EditorUi::draw_script(EditorContext& ctx, ScriptComponent& script) {
    const ScriptRegistry& registry = ScriptRegistry::instance();
    theme::begin_row("Script");
    if (ImGui::BeginCombo("##script", script.script_name.empty() ? "(none)" : script.script_name.c_str())) {
        for (const ScriptInfo& info : registry.scripts()) {
            if (ImGui::Selectable(info.name.c_str(), info.name == script.script_name)) {
                if (ctx.begin_edit) ctx.begin_edit();
                registry.assign(script, info.name);
                if (ctx.end_edit) ctx.end_edit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.description.c_str());
        }
        ImGui::EndCombo();
    }
    if (const ScriptInfo* info = registry.find(script.script_name)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col::kMuted);
        ImGui::TextWrapped("%s", info->description.c_str());
        ImGui::PopStyleColor();
    } else if (!script.script_name.empty()) {
        ImGui::TextColored(col::kError, "Unknown script.");
    }
    InspectorCallbacks callbacks{ctx.begin_edit, ctx.end_edit, ctx.cancel_edit, nullptr};
    InspectorVisitor v(callbacks, nullptr, false);
    for (ScriptParam& param : script.params) {
        if (param.is_text) {
            v.text(param.name.c_str(), param.name.c_str(), param.text);
        } else {
            v.number(param.name.c_str(), param.name.c_str(), param.number, {0.0f, 0.0f, 0.05f, "%.2f", false});
        }
    }
}

// --- Render settings -----------------------------------------------------------------

void EditorUi::draw_render_settings(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const float s = ui_scale_;
    RenderPipelineSettings& settings = ctx.engine.render_settings();
    // Quick row: debug view + actions.
    {
        theme::begin_row("Debug view");
        if (ImGui::BeginCombo("##debug", debug_view_name(ctx.debug_view))) {
            for (int i = 0; i < static_cast<int>(DebugView::Count); ++i) {
                const auto view = static_cast<DebugView>(i);
                if (ImGui::Selectable(debug_view_name(view), ctx.debug_view == view)) {
                    ctx.debug_view = view;
                }
            }
            ImGui::EndCombo();
        }
        const float half = (ImGui::GetContentRegionAvail().x - 6.0f * s) * 0.5f;
        if (ctx.renderer.baking()) {
            ImGui::ProgressBar(ctx.renderer.bake_progress(), {-1.0f, ImGui::GetFrameHeight()}, "Baking lighting...");
        } else {
            if (theme::pill_button("Bake lighting", Pill::Primary, {half, 0.0f}, Icon::Bake, true)) {
                commands.push_back({EditorCommandType::BakeLighting});
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-capture every reflection probe and irradiance volume.");
            ImGui::SameLine(0.0f, 6.0f * s);
            if (theme::pill_button("Load HDRI", Pill::Secondary, {half, 0.0f}, Icon::Open, true)) {
                commands.push_back({EditorCommandType::LoadEnvironment});
            }
        }
        if (ctx.renderer.has_environment()) {
            const std::string env = "HDRI: " + to_utf8(path_from_utf8(ctx.renderer.environment_path()).filename());
            ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
            ImGui::TextUnformatted(env.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({0.0f, 4.0f * s});
    }
    InspectorCallbacks callbacks{nullptr, ctx.mark_dirty, nullptr, [this](const char*) -> std::string {
                                     if (window_ == nullptr) return {};
                                     const std::filesystem::path path = window_->open_file_dialog(L"Cube LUT (*.cube)\0*.cube\0All files\0*.*\0", L"Choose a LUT");
                                     return path.empty() ? std::string() : to_utf8(path);
                                 }};
    InspectorVisitor v(callbacks, nullptr, true);
    describe(settings, v);
    draw_physics_settings(ctx);
}

void EditorUi::draw_physics_settings(EditorContext& ctx) {
    const float s = ui_scale_;
    PhysicsSettings& physics = ctx.engine.physics().settings;
    if (!theme::begin_section("physics_settings", "Physics", false, "Gravity and collision layers", Icon::Physics, true)) {
        theme::end_section();
        return;
    }
    InspectorCallbacks callbacks{nullptr, ctx.mark_dirty, nullptr, nullptr};
    {
        InspectorVisitor v(callbacks, nullptr, false);
        v.vec3("gravity", "Gravity", physics.gravity, {-100.0f, 100.0f, 0.05f, "%.2f", false});
    }
    theme::caption("Layers");
    for (int i = 0; i < kPhysicsLayerCount; ++i) {
        ImGui::PushID(i);
        theme::begin_row(std::format("{}", i).c_str());
        if (ImGui::InputText("##name", &physics.layer_names[static_cast<std::size_t>(i)]) && ctx.mark_dirty) {
            ctx.mark_dirty();
        }
        ImGui::PopID();
    }
    theme::caption("Which layers collide");
    // Lower-triangular matrix of toggles (symmetric).
    const float cell = ImGui::GetFrameHeight() * 0.8f;
    const float label_w = 92.0f * s;
    for (int row = 0; row < kPhysicsLayerCount; ++row) {
        ImGui::PushID(100 + row);
        ImGui::AlignTextToFramePadding();
        const std::string& name = physics.layer_names[static_cast<std::size_t>(row)];
        ImGui::TextUnformatted(name.empty() ? std::format("Layer {}", row).c_str() : name.c_str());
        for (int column = 0; column <= row; ++column) {
            ImGui::SameLine(label_w + static_cast<float>(column) * (cell + 3.0f * s));
            ImGui::PushID(column);
            bool on = physics.collides(row, column);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##cell", {cell, cell})) {
                physics.set_collides(row, column, !on);
                on = !on;
                if (ctx.mark_dirty) ctx.mark_dirty();
            }
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, {p.x + cell, p.y + cell}, theme::u32(on ? col::kLime : (hovered ? col::kHover : col::kWell)), 4.0f * s);
            if (hovered) {
                ImGui::SetTooltip("%s  x  %s", physics.layer_names[static_cast<std::size_t>(row)].c_str(), physics.layer_names[static_cast<std::size_t>(column)].c_str());
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::Dummy({0.0f, 2.0f * s});
    ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
    ImGui::TextWrapped("Columns are layers 0-%d, left to right. Lime = collides.", kPhysicsLayerCount - 1);
    ImGui::PopStyleColor();
    theme::end_section();
}

// --- Stats -------------------------------------------------------------------------------

void EditorUi::draw_stats(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    const float s = ui_scale_;
    const RenderStats& r = ctx.renderer.stats();
    // Big numbers.
    {
        const float fps = ctx.frame_ms > 0.0f ? 1000.0f / ctx.frame_ms : 0.0f;
        ImGui::PushFont(theme::heading_font(), ImGui::GetStyle().FontSizeBase * 1.9f);
        ImGui::TextColored(col::kLime, "%.0f", fps);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Dummy({0.0f, 2.0f * s});
        theme::caption("fps");
        ImGui::Text("%.2f ms CPU  |  %.2f ms GPU", ctx.frame_ms, r.gpu_ms);
        ImGui::EndGroup();
        std::vector<float> history(frame_history_.begin(), frame_history_.end());
        if (!history.empty()) {
            const float max_ms = std::max(*std::max_element(history.begin(), history.end()), 16.7f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, col::kWell);
            ImGui::PlotLines("##frames", history.data(), static_cast<int>(history.size()), 0, nullptr, 0.0f, max_ms * 1.1f, {-1.0f, 46.0f * s});
            ImGui::PopStyleColor();
        }
    }
    auto row = [](const char* label, const std::string& value) {
        theme::begin_row(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(value.c_str());
    };
    if (theme::begin_section("renderer", "Renderer", true, nullptr, Icon::Sparkle, true)) {
        row("Resolution", std::format("{} x {}", r.render_width, r.render_height));
        row("Path", r.gpu_driven ? "GPU-driven (MDI)" : "CPU culling");
        row("Draw calls", std::format("{}  ({} batches)", r.draw_calls, r.batches));
        row("Instances", std::format("{} visible / {}", r.visible_instances, r.instances));
        row("Occluded", std::to_string(r.occluded_instances));
        row("Triangles", std::format("{:.2f} M", static_cast<double>(r.triangles) / 1.0e6));
        row("Lights", std::format("{}  ({} shadowed)", r.lights, r.shadowed_lights));
        row("Shadow views", std::to_string(r.shadow_views));
        row("Exposure", std::format("EV100 {:.2f}", r.exposure_ev100));
        row("Mesh pool", std::format("{} meshes, {:.1f} MB", r.gpu_meshes, static_cast<double>(r.mesh_pool_bytes) / (1024.0 * 1024.0)));
        row("Textures", std::format("{} ({:.1f} MB){}", r.textures, static_cast<double>(r.texture_bytes) / (1024.0 * 1024.0),
                            r.pending_textures > 0 ? std::format(", {} loading", r.pending_textures) : std::string()));
        row("Render targets", std::format("{} pooled, {:.1f} MB", r.transient_textures, static_cast<double>(r.transient_bytes) / (1024.0 * 1024.0)));
        row("Particles", std::to_string(r.particles));
    }
    theme::end_section();

    if (theme::begin_section("passes", "GPU passes", true, nullptr, Icon::Chart, true)) {
        const std::vector<RenderPassStat> passes = ctx.renderer.pass_stats();
        double max_ms = 0.1;
        for (const RenderPassStat& pass : passes) {
            max_ms = std::max(max_ms, pass.gpu_ms);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const RenderPassStat& pass : passes) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float h = ImGui::GetTextLineHeight() + 6.0f * s;
            const float bar = static_cast<float>(pass.gpu_ms / max_ms) * width;
            dl->AddRectFilled(pos, {pos.x + width, pos.y + h}, theme::u32(col::kWell), 5.0f * s);
            if (!pass.culled && bar > 0.5f) {
                dl->AddRectFilled(pos, {pos.x + std::max(bar, 4.0f * s), pos.y + h}, theme::u32(col::kLime, 0.35f), 5.0f * s);
            }
            dl->AddText({pos.x + 6.0f * s, pos.y + 3.0f * s}, theme::u32(pass.culled ? col::kDim : col::kText), pass.name.c_str());
            const std::string ms = pass.culled ? std::string("culled") : std::format("{:.3f} ms", pass.gpu_ms);
            const ImVec2 ms_size = ImGui::CalcTextSize(ms.c_str());
            dl->AddText({pos.x + width - ms_size.x - 6.0f * s, pos.y + 3.0f * s}, theme::u32(pass.culled ? col::kDim : col::kMuted), ms.c_str());
            ImGui::Dummy({width, h});
            if (ImGui::IsItemHovered() && (!pass.reads.empty() || !pass.writes.empty())) {
                std::string tip = "reads: ";
                for (const std::string& name : pass.reads) tip += name + " ";
                tip += "\nwrites: ";
                for (const std::string& name : pass.writes) tip += name + " ";
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
    }
    theme::end_section();

    if (theme::begin_section("resources", "Render graph resources", false, nullptr, Icon::Layers, true)) {
        if (ImGui::BeginTable("##resources", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            for (const RenderResourceStat& res : ctx.renderer.resource_stats()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(res.name.c_str());
                ImGui::TableNextColumn();
                if (res.buffer) {
                    ImGui::TextDisabled("%.1f KB", static_cast<double>(res.width) / 1024.0);
                } else {
                    ImGui::TextDisabled("%dx%d %s", res.width, res.height, res.format.c_str());
                }
                ImGui::TableNextColumn();
                if (res.imported) {
                    ImGui::TextColored(col::kMuted, "persistent");
                } else {
                    ImGui::TextColored(col::kLimeDim, "slot %d", res.pool_slot);
                }
            }
            ImGui::EndTable();
        }
    }
    theme::end_section();

    if (theme::begin_section("world", "Scene & physics", false, nullptr, Icon::Physics, true)) {
        const PhysicsStats& p = ctx.engine.physics().stats();
        row("Actors", std::to_string(ctx.engine.scene().actor_count()));
        row("Models loaded", std::to_string(ctx.engine.assets().model_count()));
        row("Bodies", std::format("{} ({} dynamic)", p.bodies, p.dynamic_bodies));
        row("Awake", std::to_string(p.active_bodies));
        row("Characters", std::to_string(p.characters));
        row("Contacts", std::to_string(p.contacts));
    }
    theme::end_section();

    if (theme::begin_section("game_module", "Game code", false, nullptr, Icon::Script, true)) {
        GameModuleHost& module = ctx.engine.game_module();
        row("Module", module.path().empty() ? std::string("none") : to_utf8(module.path().filename()));
        if (module.loaded()) {
            row("Status", module.reload_pending() ? std::string("rebuilt; reloads after Play") : std::format("loaded (reload #{})", module.generation()));
            row("Scripts", std::to_string(module.script_count()));
        } else {
            row("Status", module.last_error().empty() ? std::string("not loaded") : module.last_error());
        }
        if (theme::pill_button("Reload now", Pill::Secondary)) {
            commands.push_back({EditorCommandType::ReloadGameModule});
        }
    }
    theme::end_section();
}

// --- Console ----------------------------------------------------------------------------

void EditorUi::draw_console(EditorContext& ctx) {
    (void)ctx;
    const float s = ui_scale_;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Layout l = compute_layout(s, outliner_t_, inspector_t_, console_t_, outliner_size_, inspector_size_, console_size_);
    const float left = l.margin + l.left_w + l.margin;
    const float right = display.x - l.margin - l.right_w - l.margin;
    const ImVec2 pos{left, l.bottom - l.console_h};
    const ImVec2 size{std::max(right - left, 100.0f), l.console_h};
    if (!begin_panel("##console", pos, size, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    int warnings = 0;
    int errors = 0;
    for (const LogEntry& entry : log_entries()) {
        warnings += entry.level == LogLevel::Warning ? 1 : 0;
        errors += entry.level == LogLevel::Error ? 1 : 0;
    }
    if (theme::icon_button("##toggle_console", show_console ? Icon::ChevronDown : Icon::Chevron, false, show_console ? "Collapse" : "Expand")) {
        show_console = !show_console;
    }
    ImGui::SameLine(0.0f, 6.0f * s);
    theme::title("Console");
    ImGui::SameLine(0.0f, 10.0f * s);
    const std::string warning_text = std::format("{} warning{}", warnings, warnings == 1 ? "" : "s");
    const std::string error_text = std::format("{} error{}", errors, errors == 1 ? "" : "s");
    // Clickable chips filter the log.
    auto filter_chip = [&](const char* id, const std::string& text, bool& enabled, const ImVec4& on_color) {
        ImGui::PushID(id);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        theme::chip(text.c_str(), enabled ? on_color : col::kDim, enabled ? col::kRaised : col::kWell);
        if (ImGui::IsItemClicked()) {
            enabled = !enabled;
        }
        (void)cursor;
        ImGui::PopID();
    };
    filter_chip("info", "info", console_show_info_, col::kText);
    ImGui::SameLine(0.0f, 4.0f * s);
    filter_chip("warn", warning_text, console_show_warnings_, col::kWarning);
    ImGui::SameLine(0.0f, 4.0f * s);
    filter_chip("err", error_text, console_show_errors_, col::kError);
    ImGui::SameLine(ImGui::GetWindowWidth() - 70.0f * s);
    if (theme::pill_button("Clear", Pill::Ghost)) {
        clear_log();
    }
    if (console_t_ > 0.02f) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col::kWell);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f * s, 6.0f * s});
        ImGui::BeginChild("##lines", {0.0f, 0.0f}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
        for (const LogEntry& entry : log_entries()) {
            if ((entry.level == LogLevel::Info && !console_show_info_) || (entry.level == LogLevel::Warning && !console_show_warnings_) ||
                (entry.level == LogLevel::Error && !console_show_errors_)) {
                continue;
            }
            ImVec4 color = col::kText;
            if (entry.level == LogLevel::Warning) color = col::kWarning;
            if (entry.level == LogLevel::Error) color = col::kError;
            ImGui::TextColored(col::kDim, "%7.2f", entry.seconds);
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", entry.message.c_str());
        }
        if (log_revision() != console_revision_) {
            console_revision_ = log_revision();
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

// --- Viewport ----------------------------------------------------------------------------

void EditorUi::draw_viewport(EditorContext& ctx, std::vector<EditorCommand>& commands) {
    (void)commands;
    const float s = ui_scale_;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Layout l = compute_layout(s, outliner_t_, inspector_t_, console_t_, outliner_size_, inspector_size_, console_size_);
    const float left = l.margin + l.left_w + l.margin;
    const float right = display.x - l.margin - l.right_w - l.margin;
    const float top = l.top;
    const float bottom = l.bottom - l.console_h - l.margin;
    viewport_ = {std::floor(left), std::floor(top), std::max(std::floor(right - left), 16.0f), std::max(std::floor(bottom - top), 16.0f)};

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const ImVec2 min{viewport_.x, viewport_.y};
    const ImVec2 max{viewport_.x + viewport_.width, viewport_.y + viewport_.height};
    const float rounding = 12.0f * s;
    const unsigned int texture = ctx.renderer.output_texture();
    bg->AddRectFilled(min, max, theme::u32(col::kBlack), rounding);
    if (texture != 0) {
        bg->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(texture)), min, max, {0.0f, 1.0f}, {1.0f, 0.0f}, IM_COL32_WHITE, rounding);
    }
    if (ctx.engine.playing()) {
        bg->AddRect(min, max, theme::u32(ctx.engine.paused() ? col::kWarning : col::kLime), rounding, 0, 2.0f * s);
    } else {
        bg->AddRect(min, max, theme::u32(col::kBorder), rounding, 0, 1.0f);
    }

    // Floating tool bar (top-left).
    const float pad = 10.0f * s;
    ImGui::SetNextWindowPos({min.x + pad, min.y + pad});
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {5.0f * s, 5.0f * s});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {3.0f * s, 3.0f * s});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
    if (ImGui::Begin("##viewport_tools", nullptr, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        auto tool = [&](const char* id, Icon icon, EditorTool value, const char* tip) {
            if (theme::icon_button(id, icon, ctx.tool == value, tip)) ctx.tool = value;
            ImGui::SameLine();
        };
        tool("##select", Icon::Select, EditorTool::Select, "Select (Q)");
        tool("##move", Icon::Move, EditorTool::Move, "Move (W)");
        tool("##rotate", Icon::Rotate, EditorTool::Rotate, "Rotate (E)");
        tool("##scale", Icon::Scale, EditorTool::Scale, "Scale (R)");
        const ImVec2 sep = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine({sep.x + 2.0f * s, sep.y + 5.0f * s}, {sep.x + 2.0f * s, sep.y + ImGui::GetFrameHeight() - 5.0f * s}, theme::u32(col::kBorder), 1.0f);
        ImGui::Dummy({5.0f * s, 0.0f});
        ImGui::SameLine();
        const bool world = ctx.space == TransformSpace::World;
        if (theme::icon_button("##space", world ? Icon::World : Icon::Local, false, world ? "World space (X)" : "Local space (X)")) {
            ctx.space = world ? TransformSpace::Local : TransformSpace::World;
        }
        ImGui::SameLine();
        if (theme::icon_button("##snap", Icon::Magnet, ctx.snap, "Snap (G). Step sizes in Edit.")) ctx.snap = !ctx.snap;
        ImGui::SameLine();
        if (theme::icon_button("##grid", Icon::Grid, ctx.view.grid, "Grid (F10)")) ctx.view.grid = !ctx.view.grid;
    }
    ImGui::End();

    // View controls (top-right).
    const char* view_name = debug_view_name(ctx.debug_view);
    const float combo_w = 150.0f * s;
    ImGui::SetNextWindowPos({max.x - pad, min.y + pad}, ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("##viewport_view", nullptr, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::BeginCombo("##view_mode", view_name)) {
            for (int i = 0; i < static_cast<int>(DebugView::Count); ++i) {
                const auto view = static_cast<DebugView>(i);
                if (ImGui::Selectable(debug_view_name(view), ctx.debug_view == view)) {
                    ctx.debug_view = view;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (theme::icon_button("##gizmos", Icon::Move, ctx.view.gizmos, "Gizmos (F9)")) ctx.view.gizmos = !ctx.view.gizmos;
        ImGui::SameLine();
        if (theme::icon_button("##icons", Icon::Light, ctx.view.icons, "Light / camera icons")) ctx.view.icons = !ctx.view.icons;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Status line (bottom-left).
    const RenderStats& r = ctx.renderer.stats();
    const std::string status = std::format("{}x{}  |  {} draws  |  {}/{} visible  |  EV {:.1f}", r.render_width, r.render_height, r.draw_calls, r.visible_instances,
        r.instances, r.exposure_ev100);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImVec2 status_size = ImGui::CalcTextSize(status.c_str());
    const ImVec2 sp{min.x + pad, max.y - pad - status_size.y - 6.0f * s};
    fg->AddRectFilled({sp.x - 8.0f * s, sp.y - 3.0f * s}, {sp.x + status_size.x + 8.0f * s, sp.y + status_size.y + 3.0f * s}, theme::u32(col::kPanel, 0.75f),
        (status_size.y + 6.0f * s) * 0.5f);
    fg->AddText(sp, theme::u32(col::kMuted), status.c_str());
    // Lime pills, right-aligned along the bottom edge.
    float pill_right = max.x - pad;
    auto pill = [&](const std::string& text) {
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 p{pill_right - size.x, max.y - pad - size.y - 6.0f * s};
        fg->AddRectFilled({p.x - 8.0f * s, p.y - 3.0f * s}, {p.x + size.x + 8.0f * s, p.y + size.y + 3.0f * s}, theme::u32(col::kLime), (size.y + 6.0f * s) * 0.5f);
        fg->AddText(p, theme::u32(col::kBlack), text.c_str());
        pill_right = p.x - 24.0f * s;
    };
    if (ctx.renderer.baking()) {
        pill(std::format("Baking lighting {:.0f}%", ctx.renderer.bake_progress() * 100.0f));
    }
    if (ctx.game_view) {
        pill("Game camera");
    }
}

// --- Showcase -----------------------------------------------------------------------------

void EditorUi::draw_showcase_window(EditorContext& ctx) {
    const float s = ui_scale_;
    ShowcaseState& showcase = ctx.showcase;
    if (!showcase_directory_synced_) {
        const std::string current = to_utf8(showcase.directory);
        const std::size_t length = std::min(current.size(), sizeof(showcase_directory_buffer_) - 1);
        std::memcpy(showcase_directory_buffer_, current.c_str(), length);
        showcase_directory_buffer_[length] = '\0';
        showcase_directory_synced_ = true;
    }
    ImGui::SetNextWindowSize({360.0f * s, 440.0f * s}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({viewport_.x + 60.0f * s, viewport_.y + 60.0f * s}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Showcase", &show_showcase_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (theme::begin_section("turntable", "Turntable", true, nullptr, Icon::Rotate, true)) {
        InspectorCallbacks none;
        InspectorVisitor v(none, nullptr, false);
        v.boolean("orbit", "Orbit camera (T)", showcase.turntable_enabled);
        float degrees = showcase.turntable_radians_per_second * kRadToDeg;
        v.number("speed", "Speed", degrees, {2.0f, 120.0f, 1.0f, "%.0f deg/s"});
        showcase.turntable_radians_per_second = degrees * kDegToRad;
    }
    theme::end_section();
    if (theme::begin_section("skies", "Skybox cycle", true, nullptr, Icon::Sun, true)) {
        InspectorCallbacks none;
        InspectorVisitor v(none, nullptr, false);
        v.boolean("cycle", "Cycle while orbiting", showcase.skybox_cycle_enabled);
        v.number("hold", "Hold", showcase.skybox_cycle_seconds, {1.0f, 15.0f, 0.1f, "%.1f s"});
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##dir", showcase_directory_buffer_, sizeof(showcase_directory_buffer_), ImGuiInputTextFlags_EnterReturnsTrue)) {
            showcase.directory = path_from_utf8(showcase_directory_buffer_);
            showcase.request_rescan = true;
        }
        if (theme::pill_button("Rescan", Pill::Secondary)) {
            showcase.directory = path_from_utf8(showcase_directory_buffer_);
            showcase.request_rescan = true;
        }
        ImGui::SameLine();
        if (theme::pill_button("Prev", Pill::Secondary)) showcase.request_previous = true;
        ImGui::SameLine();
        if (theme::pill_button("Next", Pill::Secondary)) showcase.request_next = true;
        if (showcase.playlist.empty()) {
            theme::caption("No .hdr files found in this folder.");
        } else if (!showcase.preload_complete()) {
            ImGui::ProgressBar(static_cast<float>(showcase.preload_cursor) / static_cast<float>(showcase.playlist.size()), {-1.0f, 0.0f}, "Baking...");
        }
        const std::string active = ctx.renderer.environment_path();
        for (std::size_t i = 0; i < showcase.playlist.size(); ++i) {
            const std::string name = to_utf8(showcase.playlist[i].filename());
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(name.c_str(), to_utf8(showcase.playlist[i]) == active)) {
                showcase.request_index = i;
            }
            ImGui::PopID();
        }
    }
    theme::end_section();
    ImGui::End();
}

} // namespace Hollow

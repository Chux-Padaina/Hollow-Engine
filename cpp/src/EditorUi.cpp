#include "Forge/EditorUi.hpp"

#include <algorithm>
#include <numbers>

#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>

namespace Forge {
namespace {

void apply_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = {7.0f, 6.0f};
    style.WindowPadding = {10.0f, 9.0f};

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.060f, 0.066f, 0.068f, 0.96f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.050f, 0.056f, 0.058f, 0.92f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.070f, 0.076f, 0.078f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.16f, 0.20f, 0.20f, 0.9f);
    colors[ImGuiCol_Text] = ImVec4(0.86f, 0.90f, 0.88f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.56f, 0.54f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.095f, 0.105f, 0.110f, 0.96f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.23f, 0.25f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.30f, 0.34f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.095f, 0.105f, 0.110f, 0.96f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.23f, 0.25f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.84f, 0.75f, 0.36f, 0.72f);
    colors[ImGuiCol_Header] = ImVec4(0.12f, 0.18f, 0.20f, 0.94f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.28f, 0.34f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.36f, 0.42f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.045f, 0.051f, 0.052f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.045f, 0.051f, 0.052f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.84f, 0.75f, 0.36f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.84f, 0.75f, 0.36f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.92f, 0.46f, 1.0f);
}

bool tool_button(const char* label, Engine& engine, EditorTool tool) {
    const bool active = engine.active_tool() == tool;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.30f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.84f, 0.75f, 0.36f, 1.0f));
    }
    const bool clicked = ImGui::Button(label);
    if (active) {
        ImGui::PopStyleColor(2);
    }
    if (clicked) {
        engine.set_active_tool(tool);
    }
    return clicked;
}

void transform_editor(Transform& transform) {
    ImGui::SeparatorText("Transform");
    ImGui::DragFloat3("Position", &transform.translation.x, 0.05f);
    Vec3 degrees = transform.rotation * (180.0f / std::numbers::pi_v<float>);
    if (ImGui::DragFloat3("Rotation", &degrees.x, 0.4f)) {
        transform.rotation = degrees * (std::numbers::pi_v<float> / 180.0f);
    }
    ImGui::DragFloat3("Scale", &transform.scale.x, 0.02f, 0.01f, 100.0f);
}

} // namespace

void EditorUi::initialize(Win32Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_style();
    ImGui_ImplWin32_Init(window.hwnd());
    ImGui_ImplOpenGL2_Init();
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

EditorCommand EditorUi::draw(Scene& scene, Engine& engine, Camera& camera, bool& snap_to_grid, const std::string& status) {
    EditorCommand command = EditorCommand::None;
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({io.DisplaySize.x, 44.0f});
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Forge Editor");
    ImGui::SameLine(92.0f);
    ImGui::TextDisabled("Tool");
    ImGui::SameLine();
    tool_button("Q Select", engine, EditorTool::Select);
    ImGui::SameLine();
    tool_button("W Move", engine, EditorTool::Move);
    ImGui::SameLine();
    tool_button("E Rotate", engine, EditorTool::Rotate);
    ImGui::SameLine();
    tool_button("R Scale", engine, EditorTool::Scale);
    ImGui::SameLine();
    if (ImGui::Button(engine.transform_space() == TransformSpace::World ? "World" : "Local")) {
        engine.toggle_transform_space();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap_to_grid);
    ImGui::SameLine();
    ImGui::TextDisabled("View: %s", name_of(engine.debug_view()));
    ImGui::SameLine();
    ImGui::TextDisabled("Style: %s", name_of(engine.style()));
    ImGui::End();

    ImGui::SetNextWindowPos({0.0f, 44.0f});
    ImGui::SetNextWindowSize({260.0f, io.DisplaySize.y - 44.0f});
    ImGui::Begin("Outliner", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
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

    ImGui::SeparatorText("Actors");
    for (const auto& actor : scene.actors()) {
        const bool selected = actor->id() == scene.selected_actor_id();
        const char* icon = actor->find_component<LightComponent>() ? "[Light]" : "[Actor]";
        std::string label = std::string(icon) + " " + actor->name();
        if (ImGui::Selectable(label.c_str(), selected)) {
            scene.select_actor(actor->id());
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos({io.DisplaySize.x - 322.0f, 44.0f});
    ImGui::SetNextWindowSize({322.0f, io.DisplaySize.y - 44.0f});
    ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (Actor* selected = scene.selected_actor()) {
        ImGui::Text("Actor: %s", selected->name().c_str());
        ImGui::TextDisabled("Type: %s", selected->display_type().c_str());
        transform_editor(selected->root().local_transform);

        if (auto* light = selected->find_component<LightComponent>()) {
            ImGui::SeparatorText("Light");
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
            ImGui::SeparatorText("Static Mesh");
            ImGui::Text("Mesh parts: %zu", mesh->parts.size());
            if (!mesh->parts.empty()) {
                ImGui::SeparatorText("Material");
                Material material = mesh->parts.front().material;
                bool changed = false;
                changed |= ImGui::ColorEdit3("Base color", &material.base_color.x);
                changed |= ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Roughness", &material.roughness, 0.04f, 1.0f);
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

    ImGui::SeparatorText("Camera");
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

} // namespace Forge

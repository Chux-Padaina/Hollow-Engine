#pragma once

#include "Forge/Engine.hpp"
#include "Forge/Math.hpp"
#include "Forge/Scene.hpp"

#include <Windows.h>

#include <string>

namespace Forge {

struct Camera {
    Vec3 target{0.0f, 0.5f, 0.0f};
    float yaw = 0.72f;
    float pitch = -0.36f;
    float distance = 7.5f;
    float fov_y_radians = 1.05f;

    Vec3 position() const;
    Vec3 forward() const;
    Vec3 right() const;
    Vec3 up() const;
};

struct EditorOverlayState {
    EditorTool active_tool = EditorTool::Move;
    TransformSpace transform_space = TransformSpace::World;
    bool snap_to_grid = false;
    ActorId hovered_actor = 0;
};

class Renderer {
public:
    void initialize(HDC device_context);
    void resize(int width, int height);
    void render(const Scene& scene, const Camera& camera, const Engine& engine);
    void render_editor_overlay(const Scene& scene, const Camera& camera, const EditorOverlayState& overlay);
    Vec2 project(Vec3 world, const Camera& camera) const;

private:
    int width_ = 1280;
    int height_ = 720;
    unsigned int font_base_ = 0;

    void draw_grid() const;
    void draw_actor(const Actor& actor, bool selected) const;
    void draw_mesh_part(const MeshPart& part, const Transform& transform) const;
    void draw_cube(const Material& material) const;
    void draw_plane() const;
    void draw_sphere() const;
    void draw_light_icon(const Actor& actor, const LightComponent& light, bool selected) const;
    void draw_gizmo(const Actor& actor, EditorTool tool, TransformSpace space) const;
    void draw_text(float x, float y, const std::string& text, Vec3 color = {0.86f, 0.9f, 0.88f}) const;
    void draw_rect(float x, float y, float w, float h, Vec3 color, float alpha = 1.0f) const;
    void draw_button(float x, float y, float w, float h, const std::string& label, bool active) const;
};

} // namespace Forge

#pragma once

#include "Forge/Engine.hpp"
#include "Forge/Math.hpp"
#include "Forge/Scene.hpp"

namespace Forge {

struct Camera {
    Vec3 target{0.0f, 0.5f, 0.0f};
    float yaw = 0.72f;
    float pitch = -0.36f;
    float distance = 7.5f;
    float fov_y_radians = 1.05f;

    Vec3 position() const;
};

class Renderer {
public:
    void initialize();
    void resize(int width, int height);
    void render(const Scene& scene, const Camera& camera, const Engine& engine);

private:
    int width_ = 1280;
    int height_ = 720;

    void draw_grid() const;
    void draw_actor(const Actor& actor, bool selected) const;
    void draw_mesh_part(const MeshPart& part, const Transform& transform) const;
    void draw_cube(const Material& material) const;
    void draw_plane() const;
    void draw_sphere() const;
    void draw_light_icon(const Actor& actor, const LightComponent& light, bool selected) const;
    void draw_gizmo(const Actor& actor, EditorTool tool) const;
};

} // namespace Forge

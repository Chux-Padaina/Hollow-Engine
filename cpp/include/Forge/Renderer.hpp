#pragma once

#include "Forge/Engine.hpp"
#include "Forge/Math.hpp"
#include "Forge/Scene.hpp"

#include <Windows.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

struct RenderVertex {
    Vec3 position;
    Vec3 normal;
    Vec3 color;
};

struct GpuMesh {
    unsigned int vertex_array = 0;
    unsigned int vertex_buffer = 0;
    unsigned int index_buffer = 0;
    int index_count = 0;
};

struct ObjectUniforms {
    Vec3 camera_position{};
    Vec3 directional_direction{0.35f, 0.9f, 0.28f};
    Vec3 directional_color{1.0f, 0.98f, 0.90f};
    float directional_intensity = 1.0f;
    Vec3 point_position{0.0f, 2.4f, 2.2f};
    Vec3 point_color{0.35f, 0.75f, 1.0f};
    float point_intensity = 0.0f;
    float point_range = 8.0f;
    int debug_view = 0;
};

class Renderer {
public:
    void initialize(HDC device_context);
    void shutdown();
    void resize(int width, int height);
    void render(const Scene& scene, const Camera& camera, const Engine& engine);
    Vec2 project(Vec3 world, const Camera& camera) const;

private:
    int width_ = 1280;
    int height_ = 720;
    unsigned int object_program_ = 0;
    unsigned int outline_program_ = 0;
    GpuMesh cube_mesh_;
    GpuMesh plane_mesh_;
    GpuMesh sphere_mesh_;
    std::unordered_map<const MeshPart*, GpuMesh> custom_mesh_cache_;

    bool initialize_modern_pipeline();
    ObjectUniforms collect_object_uniforms(const Scene& scene, const Camera& camera, const Engine& engine) const;
    void sync_custom_mesh_cache(const Scene& scene);
    GpuMesh create_gpu_mesh(const std::vector<RenderVertex>& vertices, const std::vector<std::uint32_t>& indices) const;
    GpuMesh create_custom_mesh(const MeshPart& part) const;
    void destroy_gpu_mesh(GpuMesh& mesh) const;
    void draw_gpu_mesh(const GpuMesh& mesh, const Material& material, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms) const;
    GpuMesh create_cube_mesh() const;
    GpuMesh create_plane_mesh() const;
    GpuMesh create_sphere_mesh() const;
    void draw_custom_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms);
    void draw_actor_outline(const Actor& actor, const Mat4& view_projection);
    void draw_mesh_part_outline(const MeshPart& part, const Transform& transform, const Mat4& view_projection);
    void draw_gpu_mesh_outline(const GpuMesh& mesh, const Transform& transform, const Mat4& view_projection, float outline_width) const;
    void draw_grid(const Camera& camera) const;
    void draw_actor(const Actor& actor, bool selected, const Mat4& view_projection, const ObjectUniforms& uniforms);
    void draw_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms);
    void draw_cube(const Material& material) const;
    void draw_plane() const;
    void draw_sphere() const;
    void draw_light_icon(const Actor& actor, const LightComponent& light, bool selected) const;
    void draw_gizmo(const Actor& actor, EditorTool tool, TransformSpace space) const;
};

} // namespace Forge

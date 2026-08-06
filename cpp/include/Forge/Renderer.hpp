#pragma once

#include "Forge/Engine.hpp"
#include "Forge/Math.hpp"
#include "Forge/Scene.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>
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
    Vec2 uv;
    Vec3 tangent;
    float tangent_sign = 1.0f;
};

struct GpuMesh {
    unsigned int vertex_array = 0;
    unsigned int vertex_buffer = 0;
    unsigned int index_buffer = 0;
    int index_count = 0;
};

struct GpuTexture {
    unsigned int id = 0;
    int width = 0;
    int height = 0;
    bool failed = false;
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
    Mat4 light_view_projection = Mat4::identity();
    std::array<Mat4, 6> point_light_view_projections{
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
    };
    bool shadows_enabled = false;
    bool point_shadows_enabled = false;
    bool hdr_output_enabled = false;
    int debug_view = 0;
};

enum class RenderPassKind {
    ShadowMaps,
    SceneColor,
    EditorGrid,
    ActorOutlines,
    OpaqueGeometry,
    Tonemap,
    EditorOverlay,
};

struct RenderFrameContext {
    const Scene& scene;
    const Camera& camera;
    const Engine& engine;
    Mat4 projection;
    Mat4 view;
    Mat4 view_projection;
    ObjectUniforms object_uniforms;
    bool use_hdr = false;
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
    unsigned int depth_program_ = 0;
    unsigned int tonemap_program_ = 0;
    unsigned int shadow_framebuffer_ = 0;
    unsigned int shadow_depth_texture_ = 0;
    int shadow_resolution_ = 2048;
    unsigned int point_shadow_framebuffer_ = 0;
    std::array<unsigned int, 6> point_shadow_depth_textures_{};
    int point_shadow_resolution_ = 1024;
    unsigned int hdr_framebuffer_ = 0;
    unsigned int hdr_color_texture_ = 0;
    unsigned int hdr_depth_texture_ = 0;
    int hdr_width_ = 0;
    int hdr_height_ = 0;
    GpuMesh cube_mesh_;
    GpuMesh plane_mesh_;
    GpuMesh sphere_mesh_;
    std::unordered_map<const MeshPart*, GpuMesh> custom_mesh_cache_;
    mutable std::unordered_map<std::string, GpuTexture> texture_cache_;
    unsigned int white_texture_ = 0;

    bool initialize_modern_pipeline();
    RenderFrameContext begin_frame(const Scene& scene, const Camera& camera, const Engine& engine);
    void execute_shadow_passes(const RenderFrameContext& frame);
    void begin_scene_color_pass(const RenderFrameContext& frame);
    void execute_editor_grid_pass(const RenderFrameContext& frame) const;
    void execute_outline_pass(const RenderFrameContext& frame);
    void execute_opaque_geometry_pass(const RenderFrameContext& frame);
    void end_scene_color_pass(const RenderFrameContext& frame);
    void execute_editor_overlay_pass(const RenderFrameContext& frame);
    void restore_camera_matrices(const RenderFrameContext& frame) const;
    ObjectUniforms collect_object_uniforms(const Scene& scene, const Camera& camera, const Engine& engine) const;
    void sync_custom_mesh_cache(const Scene& scene);
    GpuMesh create_gpu_mesh(const std::vector<RenderVertex>& vertices, const std::vector<std::uint32_t>& indices) const;
    GpuMesh create_custom_mesh(const MeshPart& part) const;
    void destroy_gpu_mesh(GpuMesh& mesh) const;
    GpuTexture create_texture_from_file(const std::string& path) const;
    unsigned int resolve_base_color_texture(const Material& material) const;
    unsigned int resolve_metallic_roughness_texture(const Material& material) const;
    unsigned int resolve_normal_texture(const Material& material) const;
    void destroy_texture(GpuTexture& texture) const;
    void ensure_hdr_framebuffer();
    void destroy_hdr_framebuffer();
    void render_tonemapped_frame(float exposure, int debug_view) const;
    void draw_gpu_mesh(const GpuMesh& mesh, const Material& material, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms);
    void render_shadow_map(const Scene& scene, const ObjectUniforms& uniforms);
    void render_point_shadow_maps(const Scene& scene, const ObjectUniforms& uniforms);
    void draw_depth_actor(const Actor& actor, const Mat4& light_view_projection);
    void draw_depth_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& light_view_projection);
    void draw_depth_gpu_mesh(const GpuMesh& mesh, const Transform& transform, const Mat4& light_view_projection) const;
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

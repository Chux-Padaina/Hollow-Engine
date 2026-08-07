// Renderer module: the engine's OpenGL PBR rendering interface.
//
// Declares the Renderer class along with the data it consumes (Camera,
// GpuLight/ObjectUniforms) and the render-graph introspection types
// (RenderTargetDesc/RenderTarget/RenderPassDescriptor) used by the editor's
// debug views. Renderer owns all GPU-side state (programs, textures, render
// targets, baked IBL data) and is driven once per frame by App via
// Renderer::render(Scene, Camera, Engine); Scene supplies the actors/lights
// to draw and Engine supplies frame-global settings (e.g. RenderPipelineSettings).
// See Renderer.cpp for the pass pipeline implementation.
#pragma once

#include "Hollow/Engine.hpp"
#include "Hollow/Math.hpp"
#include "Hollow/Scene.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Hollow {

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

// Maximum number of lights the object shader can shade in a single draw call.
// Must match MAX_LIGHTS in the object fragment shader source in Renderer.cpp.
constexpr int kMaxLights = 16;

// Number of cascades in the directional light's cascaded shadow map (CSM).
// Must match the number of uCascadeShadowMapN/uCascadeViewProjectionN
// uniforms declared in the object fragment shader source in Renderer.cpp,
// and the number of components used in its uCascadeSplitDepths vec4
// (there's no dynamically-indexable sampler array in GLSL 120, so each
// cascade gets its own named sampler uniform and an if/else selection
// chain instead of a loop over an array).
constexpr int kCascadeCount = 4;

// A single light entry uploaded to the object shader's `uLights[]` array.
// `direction` always stores the direction light travels (the light's local
// -Y axis rotated into world space), matching the convention used for
// directional light rotation everywhere else in the engine:
//   - Directional: only `direction` matters; the vector toward the light
//     used for shading is -direction.
//   - Point: only `position`/`range` matter.
//   - Spot: `position` is the cone origin, `direction` is the cone axis the
//     light points down, and `innerCos`/`outerCos` control the cone falloff.
struct GpuLight {
    int type = 0; // 0 = directional, 1 = point, 2 = spot
    Vec3 position{};
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 0.0f;
    float range = 10.0f;
    float inner_cos = 0.95f;
    float outer_cos = 0.85f;
    bool casts_shadow = false;
};

struct ObjectUniforms {
    Vec3 camera_position{};
    // Used by the fragment shader to compute a camera-forward view-space
    // depth for cascade selection (see cascade_split_depths below).
    Vec3 camera_forward{0.0f, 0.0f, -1.0f};
    // Legacy single-light fields kept for shadow-map camera placement and
    // point-shadow cubemap sampling; the first enabled directional/point/
    // spot light still owns the only shadow maps for now.
    Vec3 directional_direction{0.35f, 0.9f, 0.28f};
    Vec3 directional_color{1.0f, 0.98f, 0.90f};
    float directional_intensity = 1.0f;
    Vec3 point_position{0.0f, 2.4f, 2.2f};
    Vec3 point_color{0.35f, 0.75f, 1.0f};
    float point_intensity = 0.0f;
    float point_range = 8.0f;
    std::array<Mat4, 6> point_light_view_projections{
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
    };
    // Cascaded shadow maps for the first enabled directional light.
    // cascade_split_depths[i] is the camera-forward view-space distance
    // where cascade i ends (and i+1 begins); see Renderer::collect_object_uniforms.
    std::array<Mat4, kCascadeCount> cascade_view_projections{
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
    };
    std::array<float, kCascadeCount> cascade_split_depths{};
    // Shadow map for the first enabled spot light.
    Mat4 spot_light_view_projection = Mat4::identity();
    bool spot_shadows_enabled = false;
    bool shadows_enabled = false;
    bool point_shadows_enabled = false;
    bool hdr_output_enabled = false;
    bool ibl_enabled = false;
    bool ao_enabled = false;
    int debug_view = 0;

    // Full multi-light list shaded every draw call.
    std::array<GpuLight, kMaxLights> lights{};
    int light_count = 0;
};

enum class RenderPassKind {
    ShadowMaps,
    SceneColor,
    AmbientOcclusion,
    EditorGrid,
    ActorOutlines,
    OpaqueGeometry,
    TransparentGeometry,
    Bloom,
    Tonemap,
    Fxaa,
    EditorOverlay,
};

// --- Named render target registry -----------------------------------------
//
// Render targets are GPU texture+framebuffer resources looked up by name
// instead of being individual hardcoded Renderer member variables. A target
// is created lazily the first time it's requested (acquire_render_target)
// and recreated automatically if its resolved size changes, which is how
// viewport-sized targets track window resizes. This is the foundation later
// milestones (bloom, SSAO, IBL, deferred buffers, shadow atlases) attach new
// targets to instead of adding more one-off framebuffer members.
//
// The six-face point-light shadow cube is not in this registry yet: it
// shares a single framebuffer across six texture attachments swapped per
// face during rendering, which doesn't fit the "one target = one
// framebuffer" model here. It will be folded in when shadow atlasing
// (roadmap milestone 5) gives multi-face/multi-light shadows a proper home.

enum class TargetSize {
    Viewport, // tracks the renderer's current viewport size every frame
    Fixed,    // a fixed pixel size set once at creation (e.g. a shadow map)
};

enum class TargetColorFormat {
    None,
    Rgba8,
    Rgba16f,
};

enum class TargetDepthFormat {
    None,
    Depth24,
};

struct RenderTargetDesc {
    std::string name;
    TargetSize size = TargetSize::Viewport;
    int fixed_width = 0;
    int fixed_height = 0;
    TargetColorFormat color_format = TargetColorFormat::None;
    TargetDepthFormat depth_format = TargetDepthFormat::None;
    bool linear_filter = true;
    bool clamp_to_edge = true;
};

struct RenderTarget {
    RenderTargetDesc desc;
    unsigned int framebuffer = 0;
    unsigned int color_texture = 0;
    unsigned int depth_texture = 0;
    int width = 0;
    int height = 0;
};

// A snapshot of a render target's resolved state, safe to hand to the editor
// UI for a debug "Render Graph" view without exposing GL handles.
struct RenderTargetInfo {
    std::string name;
    int width = 0;
    int height = 0;
    bool has_color = false;
    bool has_depth = false;
};

// Declares one pass's name and the named render targets it reads from
// (samples as a texture) and writes to (binds as a framebuffer target).
// "Backbuffer" refers to the default window framebuffer. This is metadata
// for introspection/debugging today; `Renderer::render` still executes
// passes in the fixed order below rather than deriving order from these
// declarations. Reads/writes reflect the HDR-enabled configuration — with
// HDR off, the opaque/grid/outline/overlay passes write straight to
// "Backbuffer" instead of "SceneColor" and there is no tonemap pass.
struct RenderPassDescriptor {
    RenderPassKind kind;
    std::string name;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
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

    // Introspection for the editor's debug "Render Graph" panel. Safe to
    // call from EditorUi after Renderer::render has run for the frame.
    std::vector<RenderPassDescriptor> describe_passes() const;
    std::vector<RenderTargetInfo> render_target_info() const;

    // Loads a Radiance (.hdr) equirectangular environment map and bakes it
    // into an environment cubemap, a convolved diffuse irradiance cubemap,
    // and a roughness-prefiltered specular mip chain for image-based
    // lighting. Throws std::runtime_error if the file can't be read/parsed
    // or a bake pass fails; the previously-loaded environment (if any) is
    // left in place if this throws before the new one is ready. Safe to
    // call again later to swap environments at runtime.
    void load_environment(const std::string& hdr_path);
    bool has_environment() const { return ibl_ready_; }
    const std::string& environment_path() const { return ibl_environment_path_; }

private:
    int width_ = 1280;
    int height_ = 720;
    unsigned int object_program_ = 0;
    unsigned int outline_program_ = 0;
    unsigned int depth_program_ = 0;
    unsigned int tonemap_program_ = 0;
    unsigned int bloom_threshold_program_ = 0;
    unsigned int bloom_downsample_program_ = 0;
    unsigned int bloom_upsample_program_ = 0;
    unsigned int normal_depth_program_ = 0;
    unsigned int ssao_program_ = 0;
    unsigned int ssao_blur_program_ = 0;
    unsigned int fxaa_program_ = 0;
    std::unordered_map<std::string, RenderTarget> render_targets_;
    unsigned int point_shadow_framebuffer_ = 0;
    std::array<unsigned int, 6> point_shadow_depth_textures_{};
    int point_shadow_resolution_ = 1024;
    unsigned int skybox_program_ = 0;
    bool ibl_ready_ = false;
    std::string ibl_environment_path_;
    unsigned int ibl_env_cubemap_ = 0;
    unsigned int ibl_irradiance_cubemap_ = 0;
    unsigned int ibl_prefiltered_cubemap_ = 0;
    int ibl_prefiltered_mip_count_ = 0;
    unsigned int ibl_brdf_lut_ = 0;
    GpuMesh cube_mesh_;
    GpuMesh plane_mesh_;
    GpuMesh sphere_mesh_;
    std::unordered_map<const MeshPart*, GpuMesh> custom_mesh_cache_;
    mutable std::unordered_map<std::string, GpuTexture> texture_cache_;
    unsigned int white_texture_ = 0;

    struct LightUniformLocations {
        int type = -1;
        int position = -1;
        int direction = -1;
        int color = -1;
        int intensity = -1;
        int range = -1;
        int inner_cos = -1;
        int outer_cos = -1;
        int casts_shadow = -1;
    };
    std::array<LightUniformLocations, kMaxLights> light_uniform_locations_{};
    int light_count_uniform_location_ = -1;

    bool initialize_modern_pipeline();
    void cache_light_uniform_locations();
    RenderFrameContext begin_frame(const Scene& scene, const Camera& camera, const Engine& engine);
    void execute_shadow_passes(const RenderFrameContext& frame);
    void begin_scene_color_pass(const RenderFrameContext& frame);
    void execute_ambient_occlusion_pass(const RenderFrameContext& frame);
    void render_normal_depth_prepass(const Scene& scene, const Mat4& view_projection, const Camera& camera);
    void draw_normal_depth_actor(const Actor& actor, const Mat4& view_projection);
    void draw_normal_depth_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection);
    void draw_normal_depth_gpu_mesh(const GpuMesh& mesh, const Transform& transform, const Mat4& view_projection) const;
    void execute_editor_grid_pass(const RenderFrameContext& frame) const;
    void execute_outline_pass(const RenderFrameContext& frame);
    void execute_opaque_geometry_pass(const RenderFrameContext& frame);
    void execute_transparent_geometry_pass(const RenderFrameContext& frame);
    void execute_bloom_pass(const RenderFrameContext& frame);
    void end_scene_color_pass(const RenderFrameContext& frame);
    void execute_fxaa_pass(const RenderFrameContext& frame);
    void execute_editor_overlay_pass(const RenderFrameContext& frame);
    void draw_fullscreen_quad() const;
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
    unsigned int resolve_emissive_texture(const Material& material) const;
    unsigned int resolve_occlusion_texture(const Material& material) const;
    void destroy_texture(GpuTexture& texture) const;
    RenderTarget& acquire_render_target(const RenderTargetDesc& desc);
    void create_render_target_resources(RenderTarget& target) const;
    void destroy_render_target(RenderTarget& target) const;
    void destroy_all_render_targets();
    RenderTarget* find_render_target(const std::string& name);
    const RenderTarget* find_render_target(const std::string& name) const;
    void render_tonemapped_frame(const RenderPipelineSettings& settings, int debug_view) const;
    // IBL: bakes an environment/irradiance/prefiltered-specular cubemap set
    // from an already-loaded equirectangular HDR image, and lazily bakes the
    // (environment-independent) split-sum BRDF LUT the first time it's
    // needed. See Renderer::load_environment for the public entry point.
    void bake_ibl_environment(const struct HdrImage& hdr);
    void ensure_brdf_lut_baked();
    void destroy_ibl_resources();
    unsigned int create_cubemap(int width, int height, int mip_levels, bool mipmap_filtering) const;
    void draw_ibl_cube_geometry() const;
    void draw_skybox(const RenderFrameContext& frame) const;
    void draw_gpu_mesh(const GpuMesh& mesh, const Material& material, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms);
    void render_cascaded_shadow_maps(const Scene& scene, const ObjectUniforms& uniforms);
    void render_spot_shadow_map(const Scene& scene, const ObjectUniforms& uniforms);
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
    void draw_gpu_mesh_outline(const GpuMesh& mesh, const Transform& transform, const Mat4& view_projection, float outline_width, bool radial_extrude) const;
    void draw_grid(const Camera& camera) const;
    // `blend_only` selects which mesh parts to draw: false (opaque pass) draws
    // every part whose material is not AlphaMode::Blend and also draws the
    // actor's light icon if any; true (transparent pass) draws only
    // AlphaMode::Blend parts and skips the light icon, since the opaque pass
    // already drew it for this actor.
    void draw_actor(const Actor& actor, bool selected, const Mat4& view_projection, const ObjectUniforms& uniforms, bool blend_only = false);
    void draw_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms);
    void draw_cube(const Material& material) const;
    void draw_plane() const;
    void draw_sphere() const;
    void draw_light_icon(const Actor& actor, const LightComponent& light, bool selected) const;
    void draw_gizmo(const Actor& actor, EditorTool tool, TransformSpace space) const;
};

} // namespace Hollow

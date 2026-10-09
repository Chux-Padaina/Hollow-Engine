// Renderer::Impl: all renderer state, shared by the Renderer*.cpp stage
// files. Private to src/Render.
//
//   Renderer.cpp            initialization, frame orchestration, public API
//   RendererScene.cpp       scene extraction: mesh pool, instances, batches,
//                           materials, lights, probes, decals, terrain
//   RendererGeometry.cpp    GPU culling, Hi-Z, batch drawing
//   RendererShadows.cpp     cascades + shadow atlas
//   RendererLighting.cpp    clusters, GTAO, contact shadows, SSR, fog
//   RendererEnvironment.cpp sky (atmosphere LUTs), HDRI, IBL filtering
//   RendererBake.cpp        reflection probe + irradiance volume baking
//   RendererPost.cpp        TAA, sharpening, DOF, motion blur, bloom,
//                           exposure, grading LUT, tonemap, FXAA
//   RendererEffects.cpp     skinning, grass, GPU particles, the transparent
//                           stage (water, refraction, sorted/OIT blending)
//   RendererOverlay.cpp     editor overlays (grid, gizmos, icons, outline)
#pragma once

#include "Hollow/Render/Renderer.hpp"

#include "Render/Gfx.hpp"
#include "Render/GpuTypes.hpp"
#include "Render/RenderGraph.hpp"
#include "Render/ShaderLibrary.hpp"
#include "Render/TextureManager.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hollow {

struct HdrImage;

constexpr int kCascadeCount = 4;
constexpr int kEnvironmentSize = 128;     // prefiltered cubemap face size
constexpr int kMaxProbes = 31;            // + layer 0 = global environment
constexpr int kDecalLayerSize = 512;
constexpr int kMaxDecalLayers = 16;
constexpr int kClusterSlices = 24;
constexpr std::size_t kClusterItemsPerCluster = 96;

// --- Mesh pool ---------------------------------------------------------------------

// Free-list allocator over element ranges.
class RangeAllocator {
public:
    struct Range {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };
    void reset(std::uint32_t capacity);
    bool allocate(std::uint32_t count, Range& out);
    void free(Range range);
    void grow(std::uint32_t new_capacity);
    std::uint32_t capacity() const { return capacity_; }
    std::uint32_t used() const { return used_; }

private:
    std::vector<Range> free_;
    std::uint32_t capacity_ = 0;
    std::uint32_t used_ = 0;
};

// Every mesh lives in one shared vertex + index buffer so whole batches
// can be drawn with a single multi-draw-indirect call.
struct MeshPool {
    gfx::Buffer vertices; // Vertex (60 bytes)
    gfx::Buffer indices;  // uint32
    gfx::Buffer infos;    // gpu::MeshInfo per mesh slot
    gfx::Buffer draw_ids; // 0..N-1, per-instance attribute
    std::size_t draw_id_capacity = 0;
    GLuint vao = 0;
    RangeAllocator vertex_alloc;
    RangeAllocator index_alloc;
    std::vector<gpu::MeshInfo> info_shadow; // CPU copy of the info buffer
    std::vector<std::uint32_t> free_slots;
    bool infos_dirty = false;
};

struct PooledMesh {
    std::uint32_t slot = 0;
    RangeAllocator::Range vertex_range;
    RangeAllocator::Range index_range;
    std::uint64_t last_used = 0;
    std::size_t triangles_lod0 = 0;
};

// --- Frame data ---------------------------------------------------------------------

enum class BatchCategory : std::uint8_t {
    Opaque,
    Masked,
    Refractive, // transmission: after the opaque color copy, no prepass
    Transparent,
};

struct TextureSet {
    std::array<GLuint, gpu::binding::kMaterialTextureCount> ids{};
    bool operator==(const TextureSet&) const = default;
};

struct Batch {
    BatchCategory category = BatchCategory::Opaque;
    bool double_sided = false;
    bool water = false;
    std::uint32_t texture_set = 0;
    std::uint32_t first = 0; // instance slot
    std::uint32_t count = 0;
};

// One CPU-side record per instance slot (parallel to the instance buffer).
struct InstanceInfo {
    std::uint64_t key = 0;     // stable across frames: actor id + part index
    const MeshAsset* mesh = nullptr;
    std::uint32_t mesh_slot = 0;
    ActorId actor = kNoActor;
    float view_depth = 0.0f;
    std::uint32_t lod_count = 1;
};

struct ShadowViewInfo {
    Mat4 view_projection;
    Frustum frustum;
    int atlas_x = 0;
    int atlas_y = 0;
    int size = 0;
    std::uint32_t command_offset = 0;
    bool cascade = false;
    int cascade_index = -1;
};

struct FrameState {
    const Scene* scene = nullptr;
    const Camera* camera = nullptr;
    const RenderPipelineSettings* settings = nullptr;
    const RenderView* view = nullptr;
    float delta = 0.0f;

    int render_width = 1;
    int render_height = 1;
    int output_width = 1;
    int output_height = 1;
    float render_scale = 1.0f;
    bool taa = false;
    bool gpu_driven = true;
    bool occlusion = true;
    Vec2 jitter{};      // NDC
    Vec2 prev_jitter{};

    Mat4 view_matrix;
    Mat4 projection;       // GPU (reversed-Z, jittered)
    Mat4 unjittered_projection;
    Mat4 view_projection;  // jittered
    Mat4 unjittered_view_projection;
    Mat4 cull_view_projection; // conventional depth, for CPU-style planes
    Frustum frustum;
    Vec3 camera_position;
    Vec3 camera_forward;
    float near_plane = 0.05f;
    float fov_y = 1.0f;

    // Sun (first directional shadow caster).
    int sun_light_index = -1;
    Vec3 sun_direction{0.3f, 0.8f, 0.2f}; // toward the sun
    Vec3 sun_illuminance{};
    float sun_angular_radius = 0.0f;

    int directional_count = 0;
    int light_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t command_stride = 0; // commands per view (= instance count)
    std::uint32_t view_count = 0;
    std::uint32_t main_phase1_offset = 0;
    std::uint32_t main_phase2_offset = 0;
    std::uint32_t main_union_offset = 0;
    std::vector<ShadowViewInfo> shadow_views; // cascades first, then atlas tiles
    bool cascades = false;
    int feature_bits = 0;
    float exposure = 1.0f;
    float ev100 = 13.0f;
    int debug_view = 0;
    bool editor = true;
    bool has_refractive = false;
    bool has_transparent = false;
    bool has_water = false;
    bool has_particles = false;
    bool any_skinned = false;
};

// --- Environment ----------------------------------------------------------------------

struct EnvironmentEntry {
    std::string path;
    gfx::Texture background; // R11G11B10F cubemap with mips (background + filter source)
    std::uint64_t last_used = 0;
};

struct ProbeRecord {
    ActorId actor = kNoActor;
    int layer = 0;
    std::uint32_t revision = 0xFFFFFFFFu;
    Vec3 baked_position{};
    bool baked = false;
};

struct VolumeRecord {
    ActorId actor = kNoActor;
    int z_offset = 0;
    int nx = 0;
    int ny = 0;
    int nz = 0;
    std::uint32_t revision = 0xFFFFFFFFu;
    Vec3 baked_position{};
    bool baked = false;
};

struct BakeJob {
    enum class Kind {
        Probe,
        Volume,
    } kind = Kind::Probe;
    ActorId actor = kNoActor;
    int next_item = 0; // probe face or volume probe index
    int bounce = 0;
};

struct TerrainCache {
    std::uint32_t revision = 0xFFFFFFFFu;
    std::vector<MeshHandle> chunks;
    std::vector<Vec3> chunk_offsets;
    Material material;
    gfx::Buffer grass;       // grass blade instances
    std::uint32_t grass_count = 0;
    float size = 0.0f;
    std::uint64_t last_used = 0;
};

struct ParticleSystemState {
    gfx::Buffer particles;
    int capacity = 0;
    float emit_accumulator = 0.0f;
    std::uint32_t emit_cursor = 0;
    std::uint64_t last_used = 0;
    Mat4 previous_world = Mat4::identity();
};

struct GrassDraw {
    const TerrainCache* cache = nullptr;
    Mat4 model;
    Vec3 color;
    float distance = 45.0f;
    float wind = 0.6f;
    std::uint32_t offset = 0; // region in the visible list
};

struct ParticleDraw {
    ParticleSystemState* state = nullptr;
    const ParticleEmitterComponent* emitter = nullptr;
    Mat4 spawn = Mat4::identity(); // world-space systems spawn through the actor transform
    Mat4 model = Mat4::identity(); // local-space systems draw through it
    std::uint32_t emit_start = 0;
    std::uint32_t emit_count = 0;
    std::uint32_t seed = 0;
};

struct OverlayVertex {
    Vec3 position;
    std::uint32_t color; // RGBA8
};

struct OverlayLine {
    Vec3 a;
    Vec3 b;
    std::uint32_t color;
    float width; // pixels
    bool depth_test;
};

// --- Impl -------------------------------------------------------------------------------

struct Renderer::Impl {
    // Core
    bool ready = false;
    std::string error;
    int output_width = 1280;
    int output_height = 720;
    std::uint64_t frame_index = 0;
    double time = 0.0;
    RenderStats stats;
    ShaderLibrary shaders;
    gfx::FramebufferCache framebuffers;
    gfx::GpuProfiler profiler;
    std::unique_ptr<rg::RenderGraph> graph;
    TextureManager textures;
    FrameState frame;
    std::vector<rg::PassInfo> last_pass_info;
    std::vector<rg::ResourceInfo> last_resource_info;
    float max_anisotropy = 1.0f;
    bool has_s3tc = true;

    // Uniform buffers
    gfx::Buffer frame_ubo;
    gfx::Buffer shadow_ubo;
    gfx::Buffer pass_ubo; // small per-pass blocks (ring)
    std::size_t pass_ubo_offset = 0;
    gpu::FrameUniforms frame_uniforms{};
    gpu::ShadowUniforms shadow_uniforms{};
    // Previous frame (motion vectors, history reprojection).
    Mat4 previous_view_projection = Mat4::identity();
    Vec4 previous_camera{};
    float previous_exposure = 1.0f;
    bool has_previous = false;
    Vec3 fog_wind_offset{};

    // Scene buffers
    MeshPool pool;
    std::unordered_map<std::uint64_t, PooledMesh> pooled_meshes; // by MeshAsset id
    gfx::Buffer instance_buffer;
    gfx::Buffer material_buffer;
    gfx::Buffer light_buffer;
    gfx::Buffer shadow_view_buffer;
    gfx::Buffer probe_buffer;
    gfx::Buffer volume_buffer;
    gfx::Buffer decal_buffer;
    gfx::Buffer cluster_grid;
    gfx::Buffer cluster_items;
    gfx::Buffer command_buffer;
    std::array<gfx::Buffer, 2> visibility;
    int visibility_index = 0;
    gfx::Buffer instance_lods;
    gfx::Buffer cull_stats;                    // visible-instance counter (main view)
    std::array<gfx::Buffer, 3> cull_readback;  // read 2 frames later: no stall
    int last_visible_instances = 0;
    gfx::Buffer joint_buffer;      // skinning palettes
    gfx::Buffer skin_buffer;       // per-vertex joints/weights (pool order)
    std::vector<gpu::Instance> instances;
    std::vector<InstanceInfo> instance_info;
    std::vector<gpu::Material> materials;
    std::unordered_map<const Material*, std::uint32_t> material_lookup;
    std::vector<TextureSet> texture_sets;
    std::vector<Batch> batches;
    std::vector<gpu::Light> lights;
    std::vector<gpu::ShadowView> shadow_view_data;
    std::vector<gpu::Probe> probes;
    std::vector<gpu::Volume> volumes;
    std::vector<gpu::Decal> decals;
    std::unordered_map<std::uint64_t, std::uint32_t> previous_slots; // instance key -> slot
    std::unordered_map<std::uint64_t, Mat4> previous_models;
    std::vector<gpu::DrawCommand> cpu_commands; // CPU culling path

    // Skinning: per skinned instance key, two vertex regions (ping-pong).
    struct SkinnedRegion {
        std::array<RangeAllocator::Range, 2> regions;
        int current = 0;
        std::uint64_t last_used = 0;
        std::uint64_t mesh_id = 0;
    };
    std::unordered_map<std::uint64_t, SkinnedRegion> skinned_regions;
    std::unordered_map<std::uint64_t, std::uint32_t> skin_offsets; // mesh id -> offset in skin_buffer
    std::uint32_t skin_buffer_used = 0;
    struct SkinDispatch {
        std::uint32_t source_vertex = 0;
        std::uint32_t dest_vertex = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t skin_offset = 0;
        std::uint32_t joint_offset = 0;
    };
    std::vector<SkinDispatch> skin_dispatches;
    std::vector<Mat4> joint_palette;

    // Persistent render resources
    gfx::Texture output;          // RGBA8 final image
    gfx::Texture cascades;        // D32F array
    gfx::Texture atlas;           // D32F
    std::array<gfx::Texture, 2> taa_history; // RGBA16F, output size
    int history_index = 0;
    bool history_valid = false;
    gfx::Texture dfg_lut;         // RG: split-sum scale/bias, B: sheen
    gfx::Texture probe_array;     // cubemap array (layer 0 = global env)
    std::array<gfx::Texture, 3> volume_sh; // irradiance volume atlas (L1 SH)
    gfx::Texture transmittance_lut;
    gfx::Texture multiscatter_lut;
    gfx::Texture decal_array;     // RGBA16F 2D array
    std::array<gfx::Texture, 2> fog_history; // froxel history
    int fog_index = 0;
    bool fog_history_valid = false;
    gfx::Texture scene_color_history; // last frame's lit scene (SSR source)
    gfx::Texture grading_lut;     // 3D, 32^3
    gfx::Texture user_lut_texture; // .cube LUT
    gfx::Texture noise;           // 64x64 RGBA8 blue-ish noise
    gfx::Texture terrain_layers;  // procedural albedo array
    gfx::Texture terrain_normals;
    gfx::Texture lens_dirt;
    gfx::Buffer exposure_buffer;  // GPU exposure state
    std::array<gfx::Buffer, 3> exposure_readback;
    int exposure_frame = 0;
    float auto_ev100 = 13.0f;
    bool exposure_initialized = false;
    std::uint64_t grading_hash = 0;
    std::string loaded_lut_path;
    std::vector<float> user_lut; // .cube data (size^3 * 3)
    int user_lut_size = 0;

    // Environment state
    std::vector<std::unique_ptr<EnvironmentEntry>> environments;
    int active_environment = -1;
    std::string environment_path;
    std::uint64_t sky_hash = 0;     // atmosphere params last captured
    std::uint64_t env_layer_hash = 0;
    std::array<Vec4, 9> sky_sh{};
    gfx::Texture sky_capture;       // cubemap the atmosphere renders into
    gfx::Texture sky_view_lut;      // per-frame sky radiance around the camera
    bool sky_dirty = true;
    std::uint64_t atmosphere_lut_hash = 0;
    std::uint64_t last_sky_capture_frame = 0;

    // Baking
    std::vector<ProbeRecord> probe_records;
    std::vector<VolumeRecord> volume_records;
    std::vector<BakeJob> bake_queue;
    int bake_total = 0;
    int bake_done = 0;
    bool bake_requested = false;
    gfx::Texture capture_color;  // cubemap RGBA16F (probe capture)
    gfx::Texture capture_depth;  // D32F 2D (per face)
    int volume_atlas_depth = 0;
    gfx::Texture volume_capture;       // small cubemap per volume probe
    gfx::Texture volume_capture_depth;
    gfx::Buffer capture_ubo;           // FrameUniforms for the capture camera
    gfx::Buffer capture_commands;      // every instance, LOD 0
    GLuint capture_fbo = 0;

    // Decal layers by texture path
    std::unordered_map<std::string, int> decal_layers;
    int decal_layer_count = 0;

    // Terrain, particles
    std::unordered_map<ActorId, TerrainCache> terrains;
    std::unordered_map<ActorId, ParticleSystemState> particle_systems;
    MeshHandle water_grid;
    std::vector<GrassDraw> grass_draws;
    std::vector<ParticleDraw> particle_draws;
    gfx::Buffer grass_visible; // blade indices, per terrain region
    gfx::Buffer grass_args;    // DrawArraysIndirect per terrain
    GLuint empty_vao = 0;      // attribute-less draws (grass, particles)
    std::uint64_t terrain_texture_hash = 0;

    // Editor overlay
    std::vector<OverlayLine> overlay_lines;
    std::vector<OverlayVertex> overlay_triangles;
    gfx::Buffer overlay_vertices;
    GLuint overlay_vao = 0;
    MeshHandle icon_sphere;
    MeshHandle icon_cube;
    MeshHandle icon_cone;

    // --- Renderer.cpp --------------------------------------------------------
    bool initialize();
    void shutdown();
    void render(const Scene& scene, const Camera& camera, const RenderPipelineSettings& settings, const RenderView& view, float delta);
    void setup_frame(const Scene& scene, const Camera& camera, const RenderPipelineSettings& settings, const RenderView& view, float delta);
    void upload_frame_uniforms();
    void compute_features();
    void build_graph();
    // Writes `size` bytes into the pass UBO ring and binds it at UBO_PASS.
    void bind_pass_uniforms(const void* data, std::size_t size);
    template <typename T>
    void bind_pass_uniforms(const T& block) {
        bind_pass_uniforms(&block, sizeof(T));
    }
    void bind_scene_buffers();
    void bind_global_textures(GLuint scene_color, GLuint scene_depth, GLuint ao, GLuint ssr, GLuint contact, GLuint fog);

    // --- RendererScene.cpp ----------------------------------------------------
    void init_mesh_pool();
    void destroy_mesh_pool();
    PooledMesh& ensure_mesh(const MeshAsset& mesh);
    void evict_unused_meshes();
    void upload_mesh_infos();
    void ensure_draw_ids(std::size_t count);
    void extract_scene();
    void extract_lights();
    void extract_probes_and_volumes();
    void extract_decals();
    std::uint32_t material_index(const Material& material, std::uint32_t extra_flags, TextureSet& set);
    std::uint32_t texture_set_index(const TextureSet& set);
    TerrainCache& terrain_for(const Actor& actor, const TerrainComponent& terrain);
    void build_terrain(TerrainCache& cache, const TerrainComponent& terrain);
    void upload_scene_buffers();
    bool allocate_vertices(std::uint32_t count, RangeAllocator::Range& out);

    // --- RendererGeometry.cpp ------------------------------------------------
    void add_cull_pass(const char* name, std::uint32_t command_offset, std::uint32_t mode, std::uint32_t flags, const Mat4& cull_view_projection,
        const Frustum& frustum, Vec3 lod_reference, float projection_scale, rg::TextureHandle hiz, rg::BufferHandle commands, rg::BufferHandle visibility_now,
        rg::BufferHandle visibility_prev);
    rg::TextureHandle add_hiz_pass(const char* name, rg::TextureHandle depth);
    void cpu_cull(std::uint32_t command_offset, const Frustum& frustum, bool shadow_casters);
    // Draws the batches of the given categories with commands at
    // `command_offset`. `program_for` picks the program per batch.
    // `commands_override` draws from another indirect buffer (probe captures).
    void draw_batches(std::uint32_t command_offset, std::initializer_list<BatchCategory> categories, const std::function<ProgramId(const Batch&)>& program_for, bool bind_textures,
        GLuint commands_override = 0);
    void bind_texture_set(std::uint32_t set);
    ProgramId mesh_program(const char* pass_define, bool masked, std::initializer_list<const char*> extra = {});

    // --- RendererShadows.cpp ---------------------------------------------------
    void setup_shadows();
    void setup_cascades();
    void allocate_atlas();
    void add_shadow_passes(rg::BufferHandle commands);

    // --- RendererLighting.cpp ---------------------------------------------------
    // Returns the cluster grid + item buffers (read them in later passes).
    std::pair<rg::BufferHandle, rg::BufferHandle> add_cluster_pass();
    rg::TextureHandle add_contact_shadow_pass(rg::TextureHandle depth);
    rg::TextureHandle add_gtao_passes(rg::TextureHandle depth, rg::TextureHandle normals);
    rg::TextureHandle add_ssr_passes(rg::TextureHandle depth, rg::TextureHandle normals);
    rg::TextureHandle add_fog_passes(rg::BufferHandle cluster_grid_handle, rg::BufferHandle cluster_items_handle);

    // --- RendererPost.cpp ----------------------------------------------------------
    void update_exposure();
    void update_grading_lut();
    void ensure_lens_dirt();
    // Exposure, TAA/upscaling, DOF, motion blur, bloom, tonemap/grade, FXAA
    // into `output_handle`.
    rg::TextureHandle add_post_passes(rg::TextureHandle color, rg::TextureHandle depth, rg::TextureHandle velocity, rg::TextureHandle output_handle);

    // --- RendererEnvironment.cpp --------------------------------------------------
    void init_environment();
    void destroy_environment();
    void bake_dfg_lut();
    std::size_t ensure_environment_cached(const std::string& path);
    std::unique_ptr<EnvironmentEntry> bake_hdri(const HdrImage& hdr, const std::string& path);
    void update_environment(); // fills layer 0 + SH for the current sky mode
    void filter_cubemap_into(GLuint source_cubemap, int source_size, int layer, float rotation = 0.0f, float scale = 1.0f);
    std::array<Vec4, 9> project_sh(GLuint cubemap, int size, float rotation);
    void ensure_terrain_textures(const TerrainComponent* terrain);
    void update_atmosphere_luts();
    void capture_sky(GLuint cubemap, int size);

    // --- RendererBake.cpp ----------------------------------------------------------
    // Time-sliced reflection probe + irradiance volume baking (after the frame).
    void process_bakes();
    void ensure_volume_atlas();
    // Renders the scene (opaque + environment background) into one face of
    // `cubemap` from `position`.
    void render_capture_face(GLuint cubemap, GLuint depth, int size, int face, Vec3 position, bool use_volumes);

    // --- RendererEffects.cpp ------------------------------------------------------
    // Skinned instance: own posed vertex regions + a skinning dispatch.
    void setup_skinned_instance(std::uint64_t key, const MeshAsset& mesh, const PooledMesh& pooled, const AnimatorComponent& animator, gpu::Instance& instance);
    void add_skinning_pass();
    void extract_effects(const std::vector<const Actor*>& actors);
    void prepare_skinning(); // uploads the joint palette
    // Grass culling (returns the visible list + indirect args; invalid
    // handles when there is no grass).
    std::pair<rg::BufferHandle, rg::BufferHandle> add_grass_cull_pass();
    void draw_grass(bool prepass);
    // Opaque copy, refractive (water/glass), transparent (sorted or OIT)
    // and particles, drawn into `color`.
    void add_transparency_passes(rg::TextureHandle color, rg::TextureHandle depth, rg::TextureHandle fog, rg::BufferHandle cluster_grid,
        rg::BufferHandle cluster_items);

    // --- RendererOverlay.cpp ---------------------------------------------------------
    void init_overlay();
    void destroy_overlay();
    void collect_overlay();
    void draw_overlay(GLuint depth_texture);
    void add_overlay_passes(rg::TextureHandle depth, rg::TextureHandle output_handle);
};

} // namespace Hollow

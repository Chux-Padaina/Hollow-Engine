// CPU mirrors of the GPU data layouts declared in engine/shaders
// (bindings.glsl, frame.glsl, scene_data.glsl, light_types.glsl). std140 /
// std430 rules are satisfied by only using vec4/mat4-sized members (and
// 4-component groups of scalars). Keep both sides in sync.
#pragma once

#include "Hollow/Core/Math.hpp"

#include <array>
#include <cstdint>

namespace Hollow::gpu {

namespace binding {
// Uniform blocks
constexpr unsigned kFrame = 0;
constexpr unsigned kShadow = 1;
constexpr unsigned kPass = 2;
// Storage blocks
constexpr unsigned kInstances = 0;
constexpr unsigned kMaterials = 1;
constexpr unsigned kLights = 2;
constexpr unsigned kClusterGrid = 3;
constexpr unsigned kClusterItems = 4;
constexpr unsigned kDecals = 5;
constexpr unsigned kProbes = 6;
constexpr unsigned kVolumes = 7;
constexpr unsigned kShadowViews = 8;
constexpr unsigned kMeshes = 9;
constexpr unsigned kCommands = 10;
constexpr unsigned kVisibility = 11;
constexpr unsigned kPrevVisibility = 12;
constexpr unsigned kVertices = 13;
constexpr unsigned kScratch0 = 14;
constexpr unsigned kScratch1 = 15;
// Textures
constexpr unsigned kBaseColor = 0;
constexpr unsigned kMetalRough = 1;
constexpr unsigned kNormal = 2;
constexpr unsigned kEmissive = 3;
constexpr unsigned kOcclusion = 4;
constexpr unsigned kHeight = 5;
constexpr unsigned kClearcoat = 6;
constexpr unsigned kSheen = 7;
constexpr unsigned kTransmission = 8;
constexpr unsigned kBrdfLut = 9;
constexpr unsigned kEnvSpecular = 10;
constexpr unsigned kProbesTex = 11;
constexpr unsigned kVolumeSh0 = 12;
constexpr unsigned kVolumeSh1 = 13;
constexpr unsigned kVolumeSh2 = 14;
constexpr unsigned kCascades = 15;
constexpr unsigned kCascadesRaw = 16;
constexpr unsigned kAtlas = 17;
constexpr unsigned kAtlasRaw = 18;
constexpr unsigned kAo = 19;
constexpr unsigned kSsr = 20;
constexpr unsigned kContact = 21;
constexpr unsigned kFog = 22;
constexpr unsigned kDecalsTex = 23;
constexpr unsigned kTransmittance = 24;
constexpr unsigned kSceneColor = 25;
constexpr unsigned kSceneDepth = 26;
constexpr unsigned kSkyView = 27;
constexpr unsigned kTerrainLayers = 28;
constexpr unsigned kTerrainNormals = 29;
constexpr unsigned kNoise = 30;
constexpr unsigned kExtra = 31;
constexpr unsigned kMaterialTextureCount = 9;
} // namespace binding

// frame.glsl feature bits
namespace feature {
constexpr int kIbl = 1;
constexpr int kProbes = 2;
constexpr int kVolumes = 4;
constexpr int kSsr = 8;
constexpr int kAo = 16;
constexpr int kContactShadows = 32;
constexpr int kFog = 64;
constexpr int kShadows = 128;
constexpr int kCascades = 256;
constexpr int kSpecularAa = 512;
constexpr int kPcss = 1024;
constexpr int kDecals = 2048;
constexpr int kVolumetricFog = 4096;
constexpr int kEditor = 8192;
constexpr int kRefraction = 16384;
} // namespace feature

struct FrameUniforms {
    Mat4 view;
    Mat4 projection;
    Mat4 view_projection;
    Mat4 inv_view;
    Mat4 inv_projection;
    Mat4 inv_view_projection;
    Mat4 unjittered_view_projection;
    Mat4 prev_view_projection;
    Mat4 prev_inv_view_projection;
    Vec4 camera_position;
    Vec4 camera_forward;
    Vec4 prev_camera_position;
    Vec4 render_size;
    Vec4 output_size;
    Vec4 jitter;
    Vec4 exposure;
    std::int32_t flags[4];
    Vec4 time;
    Vec4 cluster;
    Vec4 cluster_z;
    Vec4 sun_direction;
    Vec4 sun_illuminance;
    Vec4 ambient;
    Vec4 sky_color;
    Vec4 counts;
    Vec4 fog0;
    Vec4 fog1;
    Vec4 fog2;
    Vec4 fog_wind;
    Vec4 gi;
    Vec4 atmosphere0;
    Vec4 atmosphere1;
    Vec4 ground_albedo;
    Vec4 selection;
    std::array<Vec4, 9> sky_sh;
};

struct ShadowUniforms {
    std::array<Mat4, 4> cascade_view_projection;
    Vec4 cascade_splits;
    std::array<Vec4, 4> cascade_scales;
    Vec4 shadow_params;
    Vec4 atlas_params;
};

// instance flags (scene_data.glsl)
namespace instance_flag {
constexpr std::uint32_t kCastShadows = 1;
constexpr std::uint32_t kSelected = 2;
constexpr std::uint32_t kSkinned = 4;
constexpr std::uint32_t kDoubleSided = 8;
constexpr std::uint32_t kOccluder = 16;
constexpr std::uint32_t kTerrain = 32;
constexpr std::uint32_t kNoMotion = 64;
constexpr std::uint32_t kEditorOnly = 128;
} // namespace instance_flag

struct Instance {
    Mat4 model;
    Mat4 prev_model;
    Vec4 normal0;
    Vec4 normal1;
    Vec4 normal2;
    Vec4 bounds;
    std::uint32_t material = 0;
    std::uint32_t flags = 0;
    std::uint32_t mesh = 0;
    std::uint32_t actor = 0;
    std::int32_t base_vertex = 0;
    std::int32_t prev_base_vertex = 0;
    std::uint32_t prev_slot = 0xFFFFFFFFu;
    std::uint32_t batch = 0;
};
static_assert(sizeof(Instance) == 224);

struct MeshInfo {
    std::uint32_t lod_first[4]{};
    std::uint32_t lod_count[4]{};
    float lod_error[4]{};
    Vec4 local_bounds;
    std::int32_t base_vertex = 0;
    std::uint32_t lod_levels = 1;
    std::uint32_t vertex_count = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(MeshInfo) == 80);

namespace material_flag {
constexpr std::uint32_t kAlphaMask = 1;
constexpr std::uint32_t kAlphaBlend = 2;
constexpr std::uint32_t kDoubleSided = 4;
constexpr std::uint32_t kUnlit = 8;
constexpr std::uint32_t kClearcoat = 16;
constexpr std::uint32_t kSheen = 32;
constexpr std::uint32_t kTransmission = 64;
constexpr std::uint32_t kAnisotropy = 128;
constexpr std::uint32_t kSubsurface = 256;
constexpr std::uint32_t kParallax = 512;
constexpr std::uint32_t kTerrain = 1024;
constexpr std::uint32_t kWater = 2048;
} // namespace material_flag

struct Material {
    Vec4 base_color;
    Vec4 emissive;
    Vec4 pbr;
    Vec4 alpha_clearcoat;
    Vec4 sheen;
    Vec4 transmission;
    Vec4 attenuation;
    Vec4 anisotropy;
    Vec4 subsurface;
    Vec4 uv;
    std::uint32_t flags = 0;
    std::uint32_t textures = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};
static_assert(sizeof(Material) == 176);

struct DrawCommand {
    std::uint32_t count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t base_vertex = 0;
    std::uint32_t base_instance = 0;
};
static_assert(sizeof(DrawCommand) == 20);

struct Light {
    Vec4 position_range;
    Vec4 direction_type;
    Vec4 color;
    Vec4 spot_shadow;
};

struct ShadowView {
    Mat4 view_projection;
    Vec4 atlas_rect;
    Vec4 params;
};

struct Probe {
    Vec4 position;
    Vec4 box_min;
    Vec4 box_max;
    Vec4 params;
};

struct Volume {
    Vec4 box_min;
    Vec4 box_max;
    Vec4 resolution;
    Vec4 params;
};

struct Decal {
    Mat4 world_to_decal;
    Vec4 color;
    Vec4 params;
    Vec4 params2;
    Vec4 bounds;
};

// cull.comp CullBlock
struct CullUniforms {
    std::array<Vec4, 6> planes;
    Mat4 view_projection;
    Vec4 camera;
    std::uint32_t params[4]; // instance count, command offset, mode, flags
    Vec4 hiz_size;
    std::uint32_t params2[4];
};

namespace cull_flag {
constexpr std::uint32_t kShadowCasters = 1;
constexpr std::uint32_t kLod = 2;
constexpr std::uint32_t kOcclusion = 4;
constexpr std::uint32_t kFrustum = 8;
constexpr std::uint32_t kMainView = 16;
} // namespace cull_flag

} // namespace Hollow::gpu

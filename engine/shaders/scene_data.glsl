// Per-instance, per-mesh and per-material data in shader storage buffers.
// Mirrored by GpuInstance / GpuMesh / GpuMaterial in src/Render/GpuTypes.hpp.
#pragma once
#include "common.glsl"

// Instance flags
#define INSTANCE_CAST_SHADOWS 1u
#define INSTANCE_SELECTED 2u
#define INSTANCE_SKINNED 4u
#define INSTANCE_DOUBLE_SIDED 8u
#define INSTANCE_OCCLUDER 16u
#define INSTANCE_TERRAIN 32u
#define INSTANCE_NO_MOTION 64u
#define INSTANCE_EDITOR_ONLY 128u

struct Instance {
    mat4 model;
    mat4 prev_model;
    vec4 normal0; // normal matrix columns (inverse-transpose of model)
    vec4 normal1;
    vec4 normal2;
    vec4 bounds;  // world bounding sphere: xyz center, w radius
    uint material;
    uint flags;
    uint mesh;
    uint actor;
    int base_vertex;      // current vertex region (skinned instances own one)
    int prev_base_vertex; // previous frame's skinned vertices (or == base)
    uint prev_slot;       // this instance's slot last frame (0xFFFFFFFF = new)
    uint batch;
};

// Up to 4 LOD index ranges per mesh.
struct MeshInfo {
    uvec4 lod_first;  // first index per LOD
    uvec4 lod_count;  // index count per LOD
    vec4 lod_error;   // screen-size thresholds (fraction of screen height)
    vec4 local_bounds; // sphere in mesh space
    int base_vertex;
    uint lod_levels;
    uint vertex_count;
    uint pad;
};

// Material flags
#define MATERIAL_ALPHA_MASK 1u
#define MATERIAL_ALPHA_BLEND 2u
#define MATERIAL_DOUBLE_SIDED 4u
#define MATERIAL_UNLIT 8u
#define MATERIAL_CLEARCOAT 16u
#define MATERIAL_SHEEN 32u
#define MATERIAL_TRANSMISSION 64u
#define MATERIAL_ANISOTROPY 128u
#define MATERIAL_SUBSURFACE 256u
#define MATERIAL_PARALLAX 512u
#define MATERIAL_TERRAIN 1024u
#define MATERIAL_WATER 2048u

// Texture mask bits (TEX_* binding order)
#define HAS_BASE_COLOR 1u
#define HAS_METAL_ROUGH 2u
#define HAS_NORMAL 4u
#define HAS_EMISSIVE 8u
#define HAS_OCCLUSION 16u
#define HAS_HEIGHT 32u
#define HAS_CLEARCOAT 64u
#define HAS_SHEEN 128u
#define HAS_TRANSMISSION 256u

struct Material {
    vec4 base_color;      // rgb, alpha
    vec4 emissive;        // rgb (already * strength), w: exposure-relative (1) or nits (0)
    vec4 pbr;             // metallic, roughness, normal scale, occlusion strength
    vec4 alpha_clearcoat; // alpha cutoff, clearcoat, clearcoat roughness, specular factor
    vec4 sheen;           // rgb sheen color, sheen roughness
    vec4 transmission;    // transmission, thickness, ior, dispersion
    vec4 attenuation;     // rgb attenuation color, attenuation distance
    vec4 anisotropy;      // strength, rotation (rad), subsurface strength, parallax scale
    vec4 subsurface;      // rgb scatter color, radius
    vec4 uv;              // uv scale xy, uv offset zw
    uint flags;
    uint textures;
    uint pad0;
    uint pad1;
};

// glDrawElementsIndirect command layout.
struct DrawCommand {
    uint count;
    uint instance_count;
    uint first_index;
    int base_vertex;
    uint base_instance;
};

layout(std430, binding = SSBO_INSTANCES) readonly buffer InstanceBuffer {
    Instance instances[];
};

layout(std430, binding = SSBO_MATERIALS) readonly buffer MaterialBuffer {
    Material materials[];
};

layout(std430, binding = SSBO_MESHES) readonly buffer MeshBuffer {
    MeshInfo meshes[];
};

mat3 instance_normal_matrix(Instance inst) {
    return mat3(inst.normal0.xyz, inst.normal1.xyz, inst.normal2.xyz);
}

// Binding points shared by every shader. Mirrored in src/Render/GpuTypes.hpp
// (namespace binding) -- keep the two in sync.
#pragma once

// Uniform blocks
#define UBO_FRAME 0
#define UBO_SHADOW 1
#define UBO_PASS 2

// Shader storage blocks
#define SSBO_INSTANCES 0
#define SSBO_MATERIALS 1
#define SSBO_LIGHTS 2
#define SSBO_CLUSTER_GRID 3
#define SSBO_CLUSTER_ITEMS 4
#define SSBO_DECALS 5
#define SSBO_PROBES 6
#define SSBO_VOLUMES 7
#define SSBO_SHADOW_VIEWS 8
#define SSBO_MESHES 9
#define SSBO_COMMANDS 10
#define SSBO_VISIBILITY 11
#define SSBO_PREV_VISIBILITY 12
#define SSBO_VERTICES 13
#define SSBO_SCRATCH0 14
#define SSBO_SCRATCH1 15

// Material textures (per batch)
#define TEX_BASE_COLOR 0
#define TEX_METAL_ROUGH 1
#define TEX_NORMAL 2
#define TEX_EMISSIVE 3
#define TEX_OCCLUSION 4
#define TEX_HEIGHT 5
#define TEX_CLEARCOAT 6
#define TEX_SHEEN 7
#define TEX_TRANSMISSION 8

// Frame-global textures
#define TEX_BRDF_LUT 9
#define TEX_ENV_SPECULAR 10
#define TEX_PROBES 11
#define TEX_VOLUME_SH0 12
#define TEX_VOLUME_SH1 13
#define TEX_VOLUME_SH2 14
#define TEX_CASCADES 15
#define TEX_CASCADES_RAW 16
#define TEX_ATLAS 17
#define TEX_ATLAS_RAW 18
#define TEX_AO 19
#define TEX_SSR 20
#define TEX_CONTACT 21
#define TEX_FOG 22
#define TEX_DECALS 23
#define TEX_TRANSMITTANCE 24
#define TEX_SCENE_COLOR 25
#define TEX_SCENE_DEPTH 26
#define TEX_SKY_VIEW 27
#define TEX_TERRAIN_LAYERS 28
#define TEX_TERRAIN_NORMALS 29
#define TEX_NOISE 30
#define TEX_EXTRA 31

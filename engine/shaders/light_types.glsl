// Light, shadow view, probe, volume and decal records (std430).
// Mirrored in src/Render/GpuTypes.hpp.
#pragma once
#include "frame.glsl"

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

struct Light {
    vec4 position_range;   // xyz, w attenuation radius
    vec4 direction_type;   // xyz direction light travels, w type
    vec4 color;            // rgb = color * luminous intensity (cd) or illuminance (lux); w source radius (m) / angular radius (rad)
    vec4 spot_shadow;      // x cone scale, y cone offset, z first shadow view (-1 none), w volumetric multiplier
};

struct ShadowView {
    mat4 view_projection;
    vec4 atlas_rect;  // uv offset xy, uv scale zw
    vec4 params;      // near, far, tan(half fov) (0 = ortho), bias scale
};

struct Probe {
    vec4 position;    // capture center, w: cubemap layer
    vec4 box_min;     // w: blend distance
    vec4 box_max;     // w: intensity
    vec4 params;      // x box projection (0/1), y priority, zw unused
};

struct Volume {
    vec4 box_min;     // w: atlas z offset (slices)
    vec4 box_max;     // w: intensity
    vec4 resolution;  // probe counts xyz, w: atlas depth
    vec4 params;      // x normal bias, y blend distance, zw unused
};

struct Decal {
    mat4 world_to_decal; // unit cube [-0.5, 0.5]^3
    vec4 color;          // rgb tint, opacity
    vec4 params;         // x albedo layer, y normal layer (-1 none), z normal strength, w roughness (-1 keep)
    vec4 params2;        // x angle fade, y metallic (-1 keep), zw unused
    vec4 bounds;         // world bounding sphere (cluster culling)
};


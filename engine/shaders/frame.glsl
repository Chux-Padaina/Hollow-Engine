// Per-frame constants (UBO_FRAME). Mirrored by FrameUniforms in
// src/Render/GpuTypes.hpp.
#pragma once
#include "common.glsl"

// u_flags.y feature bits
#define FEATURE_IBL 1
#define FEATURE_PROBES 2
#define FEATURE_VOLUMES 4
#define FEATURE_SSR 8
#define FEATURE_AO 16
#define FEATURE_CONTACT_SHADOWS 32
#define FEATURE_FOG 64
#define FEATURE_SHADOWS 128
#define FEATURE_CASCADES 256
#define FEATURE_SPECULAR_AA 512
#define FEATURE_PCSS 1024
#define FEATURE_DECALS 2048
#define FEATURE_VOLUMETRIC_FOG 4096
#define FEATURE_EDITOR 8192
#define FEATURE_REFRACTION 16384

// Debug views (DebugView enum order)
#define DEBUG_FINAL 0
#define DEBUG_ALBEDO 1
#define DEBUG_NORMALS 2
#define DEBUG_DIRECT 3
#define DEBUG_ROUGHNESS 4
#define DEBUG_METALLIC 5
#define DEBUG_AO 6
#define DEBUG_EMISSIVE 7
#define DEBUG_INDIRECT 8
#define DEBUG_REFLECTIONS 9
#define DEBUG_VELOCITY 10
#define DEBUG_LIGHT_COMPLEXITY 11
#define DEBUG_CASCADES 12
#define DEBUG_LODS 13
#define DEBUG_INSTANCES 14

layout(std140, binding = UBO_FRAME) uniform FrameBlock {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_view_projection;
    mat4 u_inv_view;
    mat4 u_inv_projection;
    mat4 u_inv_view_projection;
    mat4 u_unjittered_view_projection;
    mat4 u_prev_view_projection;
    mat4 u_prev_inv_view_projection;
    vec4 u_camera_position; // w: near plane
    vec4 u_camera_forward;  // w: time (s)
    vec4 u_prev_camera_position;
    vec4 u_render_size;     // w, h, 1/w, 1/h
    vec4 u_output_size;
    vec4 u_jitter;          // xy current NDC jitter, zw previous
    vec4 u_exposure;        // x exposure, y EV100, z previous exposure, w 1/exposure
    ivec4 u_flags;          // x debug view, y features, z light count, w frame index
    vec4 u_time;            // x delta time, y time, z sin(time), w render scale
    vec4 u_cluster;         // tiles x, tiles y, slices, tile size (px)
    vec4 u_cluster_z;       // slice = log2(viewZ) * x + y; z near, w far
    vec4 u_sun_direction;   // xyz toward the sun, w shadowed sun light index (-1 none)
    vec4 u_sun_illuminance; // rgb lux, w angular radius (rad)
    vec4 u_ambient;         // x env luminance scale, y prefiltered mips, z sky mode, w env rotation
    vec4 u_sky_color;       // rgb luminance (Color mode)
    vec4 u_counts;          // probes, volumes, decals, lights
    vec4 u_fog0;            // density, height falloff, base height, anisotropy
    vec4 u_fog1;            // albedo rgb, max distance
    vec4 u_fog2;            // ambient, noise, froxel depth exponent, unused
    vec4 u_fog_wind;        // xyz accumulated wind offset
    vec4 u_gi;              // ao intensity, specular occlusion, ssr max roughness, ao radius
    vec4 u_atmosphere0;     // planet radius km, top radius km, haze, air density
    vec4 u_atmosphere1;     // ozone, sun disk, background intensity, unused
    vec4 u_ground_albedo;
    vec4 u_selection;       // editor: selected actor id, hovered id, outline px, unused
    vec4 u_sky_sh[9];       // global ambient: L2 SH of the sky/HDRI radiance (luminance units)
};

bool has_feature(int bit) { return (u_flags.y & bit) != 0; }

// Reversed-Z, infinite far: depth = near / viewZ (viewZ > 0 forward).
float linear_depth(float depth) {
    return u_camera_position.w / max(depth, 1e-7);
}

vec3 world_from_depth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = u_inv_view_projection * clip;
    return world.xyz / world.w;
}

vec3 view_from_depth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = u_inv_projection * clip;
    return view.xyz / view.w;
}

vec2 project_to_uv(vec3 world) {
    vec4 clip = u_unjittered_view_projection * vec4(world, 1.0);
    return clip.xy / clip.w * 0.5 + 0.5;
}

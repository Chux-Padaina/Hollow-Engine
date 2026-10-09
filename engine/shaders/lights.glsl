// Lights, clustered light lists and shadow sampling (cascades for the sun,
// an atlas for spot/point lights; PCF or PCSS filtering).
#pragma once
#include "frame.glsl"

#include "light_types.glsl"

layout(std430, binding = SSBO_LIGHTS) readonly buffer LightBuffer {
    Light lights[];
};

layout(std430, binding = SSBO_SHADOW_VIEWS) readonly buffer ShadowViewBuffer {
    ShadowView shadow_views[];
};

layout(std430, binding = SSBO_PROBES) readonly buffer ProbeBuffer {
    Probe probes[];
};

layout(std430, binding = SSBO_VOLUMES) readonly buffer VolumeBuffer {
    Volume volumes[];
};

layout(std430, binding = SSBO_DECALS) readonly buffer DecalBuffer {
    Decal decals[];
};

// Per cluster: x first item, y lights, z decals, w probes (items stored in
// that order).
layout(std430, binding = SSBO_CLUSTER_GRID) readonly buffer ClusterGrid {
    uvec4 clusters[];
};

layout(std430, binding = SSBO_CLUSTER_ITEMS) readonly buffer ClusterItems {
    uint cluster_item_count;
    uint cluster_items[];
};

layout(std140, binding = UBO_SHADOW) uniform ShadowBlock {
    mat4 u_cascade_view_projection[4];
    vec4 u_cascade_splits;     // view distance where each cascade ends
    vec4 u_cascade_scales[4];  // x world size of the cascade, y depth range (m), z texel size (m)
    vec4 u_shadow_params;      // depth bias, normal bias, softness, cascade blend fraction
    vec4 u_atlas_params;       // atlas size, 1/size, contact shadow length, unused
};

layout(binding = TEX_CASCADES) uniform sampler2DArrayShadow t_cascades;
layout(binding = TEX_CASCADES_RAW) uniform sampler2DArray t_cascades_raw;
layout(binding = TEX_ATLAS) uniform sampler2DShadow t_atlas;
layout(binding = TEX_ATLAS_RAW) uniform sampler2D t_atlas_raw;

uint cluster_index(vec2 pixel, float view_z) {
    uvec2 tile = uvec2(pixel / u_cluster.w);
    tile = min(tile, uvec2(u_cluster.xy) - 1u);
    float slice_f = log2(max(view_z, 1e-4)) * u_cluster_z.x + u_cluster_z.y;
    uint slice = uint(clamp(slice_f, 0.0, u_cluster.z - 1.0));
    return tile.x + tile.y * uint(u_cluster.x) + slice * uint(u_cluster.x) * uint(u_cluster.y);
}

// Smooth distance window: physically 1/d^2 that reaches 0 at the radius.
float distance_attenuation(float distance_sq, float range) {
    float factor = distance_sq / max(range * range, 1e-4);
    float smooth_factor = saturate(1.0 - factor * factor);
    return smooth_factor * smooth_factor / max(distance_sq, 1e-4);
}

float spot_attenuation(Light light, vec3 l) {
    float cd = dot(light.direction_type.xyz, -l);
    float a = saturate(cd * light.spot_shadow.x + light.spot_shadow.y);
    return a * a;
}

// --- Shadow filtering ----------------------------------------------------------

const vec2 POISSON16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

mat2 shadow_rotation(vec2 pixel) {
    float angle = interleaved_gradient_noise(pixel, float(u_flags.w)) * TWO_PI;
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, s, -s, c);
}

// Cascades: depth is linear (orthographic), reversed (1 = near).
float sample_cascade_pcf(int cascade, vec3 coord, float radius_uv, mat2 rot) {
    float sum = 0.0;
    for (int i = 0; i < 12; ++i) {
        vec2 offset = rot * POISSON16[i] * radius_uv;
        sum += texture(t_cascades, vec4(coord.xy + offset, float(cascade), coord.z));
    }
    return sum / 12.0;
}

float sample_cascade(int cascade, vec3 world, vec3 normal, vec2 pixel, float light_angle) {
    vec4 scales = u_cascade_scales[cascade];
    float texel = scales.z;
    vec3 offset_pos = world + normal * texel * 1.5 * u_shadow_params.y;
    vec4 clip = u_cascade_view_projection[cascade] * vec4(offset_pos, 1.0);
    vec3 coord = clip.xyz / clip.w;
    coord.xy = coord.xy * 0.5 + 0.5;
    if (any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0)))) {
        return 1.0;
    }
    // Reversed depth: a small bias toward the light (larger depth).
    coord.z += u_shadow_params.x * 0.00015 * (1.0 + float(cascade));
    mat2 rot = shadow_rotation(pixel);
    float texel_uv = 1.0 / float(textureSize(t_cascades_raw, 0).x);
    float radius_uv = texel_uv * 1.5;
    if (has_feature(FEATURE_PCSS) && light_angle > 0.0) {
        // Blocker search over the light's footprint.
        float search_uv = clamp(light_angle * u_shadow_params.z * scales.y * 0.5 / scales.x, texel_uv, 0.05);
        float blocker_sum = 0.0;
        float blockers = 0.0;
        for (int i = 0; i < 16; ++i) {
            float d = texture(t_cascades_raw, vec3(coord.xy + rot * POISSON16[i] * search_uv, float(cascade))).r;
            if (d > coord.z) {
                blocker_sum += d;
                blockers += 1.0;
            }
        }
        if (blockers < 1.0) {
            return 1.0;
        }
        float blocker = blocker_sum / blockers;
        float distance_m = (blocker - coord.z) * scales.y;
        float penumbra_m = distance_m * tan(light_angle * u_shadow_params.z);
        radius_uv = clamp(penumbra_m / scales.x, texel_uv, 0.04);
    }
    return sample_cascade_pcf(cascade, coord, radius_uv, rot);
}

float sun_shadow(vec3 world, vec3 normal, vec2 pixel, float view_z) {
    if (!has_feature(FEATURE_CASCADES)) {
        return 1.0;
    }
    int cascade = 0;
    for (; cascade < 4; ++cascade) {
        if (view_z < u_cascade_splits[cascade]) {
            break;
        }
    }
    if (cascade >= 4) {
        return 1.0;
    }
    float light_angle = u_sun_illuminance.w;
    float shadow = sample_cascade(cascade, world, normal, pixel, light_angle);
    // Blend into the next cascade near the boundary.
    float split = u_cascade_splits[cascade];
    float previous = cascade == 0 ? 0.0 : u_cascade_splits[cascade - 1];
    float band = (split - previous) * u_shadow_params.w;
    float t = (view_z - (split - band)) / max(band, 1e-4);
    if (cascade < 3 && t > 0.0) {
        float next = sample_cascade(cascade + 1, world, normal, pixel, light_angle);
        shadow = mix(shadow, next, saturate(t));
    } else if (cascade == 3 && t > 0.0) {
        shadow = mix(shadow, 1.0, saturate(t));
    }
    return shadow;
}

// Probe captures: the capture camera's depth says nothing about the main
// view's cascades, so pick the first cascade that contains the point.
float sun_shadow_world(vec3 world, vec3 normal, vec2 pixel) {
    if (!has_feature(FEATURE_CASCADES)) {
        return 1.0;
    }
    for (int c = 0; c < 4; ++c) {
        vec4 clip = u_cascade_view_projection[c] * vec4(world, 1.0);
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (all(greaterThan(uv, vec2(0.02))) && all(lessThan(uv, vec2(0.98)))) {
            return sample_cascade(c, world, normal, pixel, u_sun_illuminance.w);
        }
    }
    return 1.0;
}

int cascade_for(float view_z) {
    for (int c = 0; c < 4; ++c) {
        if (view_z < u_cascade_splits[c]) {
            return c;
        }
    }
    return 4;
}

// Spot/point lights: perspective reversed-Z views packed in the atlas.
float view_linear_depth(ShadowView view, float d) {
    float n = view.params.x;
    float f = view.params.y;
    return n * f / max(d * (f - n) + n, 1e-6);
}

float sample_atlas_view(ShadowView view, vec3 world, vec3 normal, vec2 pixel, float light_radius, float receiver_distance) {
    float tan_half = max(view.params.z, 1e-3);
    // Normal offset scaled with the texel's world size at this distance.
    float tile_texels = view.atlas_rect.z * u_atlas_params.x;
    float texel_world = 2.0 * receiver_distance * tan_half / max(tile_texels, 1.0);
    vec3 offset_pos = world + normal * texel_world * 1.5 * u_shadow_params.y;
    vec4 clip = view.view_projection * vec4(offset_pos, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = clip.xyz / clip.w;
    coord.xy = coord.xy * 0.5 + 0.5;
    coord.z += u_shadow_params.x * 0.00002;
    vec2 tile_min = view.atlas_rect.xy + vec2(1.5 * u_atlas_params.y);
    vec2 tile_max = view.atlas_rect.xy + view.atlas_rect.zw - vec2(1.5 * u_atlas_params.y);
    vec2 uv = view.atlas_rect.xy + coord.xy * view.atlas_rect.zw;
    mat2 rot = shadow_rotation(pixel);
    float radius_uv = 1.5 * u_atlas_params.y;
    if (has_feature(FEATURE_PCSS) && light_radius > 0.0) {
        float receiver = view_linear_depth(view, coord.z);
        float search_world = light_radius * u_shadow_params.z;
        float search_uv = clamp(search_world / (2.0 * receiver * tan_half) * view.atlas_rect.z * 0.5, u_atlas_params.y, view.atlas_rect.z * 0.1);
        float blocker_sum = 0.0;
        float blockers = 0.0;
        for (int i = 0; i < 16; ++i) {
            vec2 suv = clamp(uv + rot * POISSON16[i] * search_uv, tile_min, tile_max);
            float d = texture(t_atlas_raw, suv).r;
            if (d > coord.z) {
                blocker_sum += view_linear_depth(view, d);
                blockers += 1.0;
            }
        }
        if (blockers < 1.0) {
            return 1.0;
        }
        float blocker = blocker_sum / blockers;
        float penumbra_world = search_world * (receiver - blocker) / max(blocker, 1e-3);
        radius_uv = clamp(penumbra_world / (2.0 * receiver * tan_half) * view.atlas_rect.z, u_atlas_params.y, view.atlas_rect.z * 0.08);
    }
    float sum = 0.0;
    for (int i = 0; i < 12; ++i) {
        vec2 suv = clamp(uv + rot * POISSON16[i] * radius_uv, tile_min, tile_max);
        sum += texture(t_atlas, vec3(suv, coord.z));
    }
    return sum / 12.0;
}

int cube_face(vec3 v) {
    vec3 a = abs(v);
    if (a.x >= a.y && a.x >= a.z) {
        return v.x > 0.0 ? 0 : 1;
    }
    if (a.y >= a.z) {
        return v.y > 0.0 ? 2 : 3;
    }
    return v.z > 0.0 ? 4 : 5;
}

float local_light_shadow(Light light, vec3 world, vec3 normal, vec2 pixel) {
    int first = int(light.spot_shadow.z);
    if (first < 0 || !has_feature(FEATURE_SHADOWS)) {
        return 1.0;
    }
    vec3 to_frag = world - light.position_range.xyz;
    float distance = length(to_frag);
    int index = first;
    if (int(light.direction_type.w) == LIGHT_POINT) {
        index += cube_face(to_frag);
    }
    return sample_atlas_view(shadow_views[index], world, normal, pixel, light.color.w, distance);
}

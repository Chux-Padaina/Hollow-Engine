// Applying fog: the froxel volume (in-scattering rgb, transmittance a) when
// volumetric fog is on, otherwise analytic exponential height fog.
#pragma once
#include "frame.glsl"

layout(binding = TEX_FOG) uniform sampler3D t_fog;

// Froxel z distribution: slice = (z / far)^(1/exponent).
vec3 froxel_uvw(vec2 uv, float view_z) {
    float w = pow(saturate(view_z / u_fog1.w), 1.0 / u_fog2.z);
    return vec3(uv, w);
}

float height_fog_optical_depth(vec3 a, vec3 b) {
    float density = u_fog0.x;
    float falloff = max(u_fog0.y, 1e-4);
    float ha = a.y - u_fog0.z;
    float hb = b.y - u_fog0.z;
    float dist = length(b - a);
    float dh = hb - ha;
    float integral = abs(dh) > 1e-3
        ? (exp(-falloff * ha) - exp(-falloff * hb)) / (falloff * dh)
        : exp(-falloff * ha);
    return density * dist * integral;
}

vec4 apply_fog(vec3 color, vec3 world, vec2 uv, float view_z) {
    if (!has_feature(FEATURE_FOG)) {
        return vec4(color, 1.0);
    }
    if (has_feature(FEATURE_VOLUMETRIC_FOG)) {
        vec4 f = textureLod(t_fog, froxel_uvw(uv, view_z), 0.0);
        return vec4(color * f.a + f.rgb, f.a);
    }
    float depth = height_fog_optical_depth(u_camera_position.xyz, world);
    float transmittance = exp(-depth);
    vec3 scatter_color = u_fog1.rgb * (u_sky_sh[0].rgb * 0.282095 * u_ambient.x * u_fog2.x + u_sun_illuminance.rgb * 0.05 / PI) * u_exposure.x;
    return vec4(color * transmittance + scatter_color * (1.0 - transmittance), transmittance);
}

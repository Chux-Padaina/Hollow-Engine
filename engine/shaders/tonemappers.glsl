// Display transforms: ACES (Hill's fit of the RRT+ODT), AgX (Sobotka;
// minimal polynomial approximation by Wrensch) with optional "punchy" look,
// and the Khronos PBR Neutral tonemapper. All take scene-linear and return
// display-linear [0,1] (encode to sRGB afterwards).
#pragma once
#include "common.glsl"

vec3 tonemap_aces(vec3 v) {
    const mat3 input_mat = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 output_mat = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    v = input_mat * v;
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return saturate(output_mat * (a / b));
}

vec3 agx_contrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 tonemap_agx(vec3 v, bool punchy) {
    const mat3 agx_mat = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 agx_inv = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    v = agx_mat * max(v, vec3(1e-10));
    v = clamp(log2(v), min_ev, max_ev);
    v = (v - min_ev) / (max_ev - min_ev);
    v = agx_contrast(v);
    if (punchy) {
        float luma = dot(v, vec3(0.2126, 0.7152, 0.0722));
        v = pow(max(v, vec3(0.0)), vec3(1.35));
        v = luma + 1.4 * (v - luma);
    }
    v = agx_inv * v;
    return saturate(pow(max(v, vec3(0.0)), vec3(2.2)));
}

vec3 tonemap_neutral(vec3 color) {
    const float start = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start) {
        return saturate(color);
    }
    const float d = 1.0 - start;
    float new_peak = 1.0 - d * d / (peak + d - start);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return saturate(mix(color, vec3(new_peak), g));
}

// Log encoding used by the grading LUT: covers 0.18 * 2^[-8, 8].
vec3 lut_encode(vec3 c) {
    return saturate((log2(max(c, vec3(1e-6)) / 0.18) + 8.0) / 16.0);
}

vec3 lut_decode(vec3 x) {
    return 0.18 * exp2(x * 16.0 - 8.0);
}

// Gerstner wave sum shared by water.vert and water.frag.
#pragma once

const int WATER_WAVES = 4;

vec3 gerstner(vec3 world, Material m, float fade, out vec3 normal) {
    float height = m.anisotropy.x * fade;
    float wind = m.anisotropy.y;
    float steepness = m.anisotropy.z;
    float wavelength = max(m.anisotropy.w, 0.1);
    float speed = m.alpha_clearcoat.y;
    float t = u_time.y;
    // Four waves around the wind direction with decreasing size.
    const float angle_offset[WATER_WAVES] = float[](0.0, 0.55, -0.4, 1.1);
    const float length_scale[WATER_WAVES] = float[](1.0, 0.61, 0.37, 0.23);
    vec3 p = world;
    vec3 n = vec3(0.0, 1.0, 0.0);
    for (int i = 0; i < WATER_WAVES; ++i) {
        float a = wind + angle_offset[i];
        vec2 dir = vec2(cos(a), sin(a));
        float L = wavelength * length_scale[i];
        float k = TWO_PI / L;
        float amp = height * length_scale[i] * 0.5;
        float c = sqrt(9.81 / k) * speed;
        float q = steepness / max(k * amp * float(WATER_WAVES), 1e-4);
        q = min(q, 1.0);
        float phase = k * dot(dir, world.xz) - c * k * t;
        float s = sin(phase);
        float co = cos(phase);
        p.xz += q * amp * dir * co;
        p.y += amp * s;
        float wa = k * amp;
        n.x -= dir.x * wa * co;
        n.z -= dir.y * wa * co;
        n.y -= q * wa * s;
    }
    normal = normalize(n);
    return p;
}

// Small ripples (analytic derivatives of a few short sine waves), for
// the per-pixel normal. Fades out with distance to avoid aliasing.
vec3 water_ripples(vec3 world, vec3 n, float wind, float distance_fade) {
    float t = u_time.y;
    vec2 slope = vec2(0.0);
    for (int i = 0; i < 6; ++i) {
        float a = wind + float(i) * 1.13 - 2.4;
        vec2 dir = vec2(cos(a), sin(a));
        float L = 0.35 + float(i) * 0.21;
        float k = TWO_PI / L;
        float amp = 0.012 * L;
        float phase = k * dot(dir, world.xz) - sqrt(9.81 * k) * t + float(i) * 1.7;
        slope += dir * (k * amp * cos(phase));
    }
    slope *= distance_fade;
    return normalize(n + vec3(-slope.x, 0.0, -slope.y));
}

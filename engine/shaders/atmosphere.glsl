// Physically based atmosphere (Hillaire 2020, "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique"): Rayleigh + Mie +
// ozone, a transmittance LUT, a multiple-scattering LUT and a per-frame
// sky-view LUT. Distances in kilometers; radiance is per unit of sun
// illuminance (multiply by the sun's lux).
#pragma once
#include "frame.glsl"

struct Atmosphere {
    float bottom;
    float top;
    vec3 rayleigh_scattering;
    float rayleigh_exp;
    float mie_scattering;
    float mie_extinction;
    float mie_exp;
    float mie_g;
    vec3 ozone_absorption;
    vec3 ground_albedo;
};

Atmosphere get_atmosphere() {
    Atmosphere a;
    a.bottom = u_atmosphere0.x;
    a.top = u_atmosphere0.y;
    a.rayleigh_scattering = vec3(5.802, 13.558, 33.1) * 1e-3 * u_atmosphere0.w;
    a.rayleigh_exp = -1.0 / 8.0;
    a.mie_scattering = 3.996e-3 * u_atmosphere0.z;
    a.mie_extinction = 4.40e-3 * u_atmosphere0.z;
    a.mie_exp = -1.0 / 1.2;
    a.mie_g = 0.8;
    a.ozone_absorption = vec3(0.650, 1.881, 0.085) * 1e-3 * u_atmosphere1.x;
    a.ground_albedo = u_ground_albedo.rgb;
    return a;
}

struct Medium {
    vec3 scattering;
    vec3 extinction;
    vec3 rayleigh;
    float mie;
};

Medium sample_medium(Atmosphere a, vec3 p) {
    float h = max(length(p) - a.bottom, 0.0);
    float rd = exp(a.rayleigh_exp * h);
    float md = exp(a.mie_exp * h);
    float od = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    Medium m;
    m.rayleigh = a.rayleigh_scattering * rd;
    m.mie = a.mie_scattering * md;
    m.scattering = m.rayleigh + vec3(m.mie);
    m.extinction = m.rayleigh + vec3(a.mie_extinction * md) + a.ozone_absorption * od;
    return m;
}

float ray_sphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) {
        return -1.0;
    }
    float s = sqrt(disc);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t0 >= 0.0) {
        return t0;
    }
    return t1 >= 0.0 ? t1 : -1.0;
}

float rayleigh_phase(float c) {
    return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

float cornette_shanks_phase(float g, float c) {
    float g2 = g * g;
    float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(1.0 + g2 - 2.0 * g * c, 1.5);
}

// Transmittance LUT parametrization (Bruneton).
vec2 transmittance_uv(Atmosphere a, float r, float mu) {
    float H = sqrt(a.top * a.top - a.bottom * a.bottom);
    float rho = sqrt(max(r * r - a.bottom * a.bottom, 0.0));
    float disc = r * r * (mu * mu - 1.0) + a.top * a.top;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float d_min = a.top - r;
    float d_max = rho + H;
    return vec2((d - d_min) / (d_max - d_min), rho / H);
}

void transmittance_params(Atmosphere a, vec2 uv, out float r, out float mu) {
    float H = sqrt(a.top * a.top - a.bottom * a.bottom);
    float rho = H * uv.y;
    r = sqrt(rho * rho + a.bottom * a.bottom);
    float d_min = a.top - r;
    float d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

layout(binding = TEX_TRANSMITTANCE) uniform sampler2D t_transmittance;

vec3 transmittance_to_top(Atmosphere a, float r, float mu) {
    return textureLod(t_transmittance, transmittance_uv(a, r, mu), 0.0).rgb;
}

// Camera position in the planet frame (km). The world origin sits 200 m
// above the ground so scenes near y = 0 aren't at the planet surface.
vec3 atmosphere_position(vec3 world) {
    return vec3(0.0, u_atmosphere0.x + 0.2 + max(world.y, 0.0) * 0.001, 0.0);
}

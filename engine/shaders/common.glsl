// Constants and small helpers shared by every shader.
#pragma once
#include "bindings.glsl"

const float PI = 3.14159265358979;
const float TWO_PI = 6.28318530717959;
const float INV_PI = 0.31830988618379;
const float EPSILON = 1e-5;

float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, vec2(0.0), vec2(1.0)); }
vec3 saturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
float sq(float x) { return x * x; }
float max3(vec3 v) { return max(v.x, max(v.y, v.z)); }
float min3(vec3 v) { return min(v.x, min(v.y, v.z)); }

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Octahedral normal encoding (unit vector <-> [-1,1]^2).
vec2 oct_wrap(vec2 v) {
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 encode_normal(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : oct_wrap(n.xy);
    return n.xy;
}

vec3 decode_normal(vec2 e) {
    vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// Hashes / noise
uint hash_u32(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash_float(uint x) { return float(hash_u32(x)) / 4294967296.0; }

float interleaved_gradient_noise(vec2 pixel, float frame) {
    pixel += 5.588238 * mod(frame, 64.0);
    return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

vec2 hammersley(uint i, uint n) {
    uint bits = bitfieldReverse(i);
    return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

// Builds an orthonormal basis around n (Frisvad / Duff et al.).
void basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// Kelvin -> linear RGB (approximation, normalized to luminance 1).
vec3 kelvin_to_rgb(float kelvin) {
    float t = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    vec3 c;
    c.r = t <= 66.0 ? 1.0 : saturate(1.29293618606 * pow(t - 60.0, -0.1332047592));
    c.g = t <= 66.0 ? saturate(0.39008157876 * log(t) - 0.63184144378) : saturate(1.12989086089 * pow(t - 60.0, -0.0755148492));
    c.b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : saturate(0.54320678911 * log(t - 10.0) - 1.19625408914));
    c = srgb_to_linear(c);
    return c / max(luminance(c), 1e-4);
}

// Spherical coordinates for an equirectangular map.
vec2 direction_to_equirect(vec3 d) {
    return vec2(atan(d.z, d.x) * (0.5 * INV_PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) * INV_PI);
}

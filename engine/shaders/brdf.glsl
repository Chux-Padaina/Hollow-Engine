// BRDF terms: GGX specular (isotropic + anisotropic), height-correlated
// Smith visibility, multi-scattering energy compensation, Burley diffuse,
// Charlie sheen and the clearcoat lobe.
#pragma once
#include "common.glsl"

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * f * f + 1e-7);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float lv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float ll = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, 1e-5);
}

float D_GGX_Anisotropic(float NoH, float ToH, float BoH, float at, float ab) {
    float a2 = at * ab;
    vec3 d = vec3(ab * ToH, at * BoH, a2 * NoH);
    float d2 = dot(d, d);
    float b2 = a2 / max(d2, 1e-7);
    return a2 * b2 * b2 * INV_PI;
}

float V_SmithGGXCorrelated_Anisotropic(float at, float ab, float ToV, float BoV, float ToL, float BoL, float NoV, float NoL) {
    float lv = NoL * length(vec3(at * ToV, ab * BoV, NoV));
    float ll = NoV * length(vec3(at * ToL, ab * BoL, NoL));
    return 0.5 / max(lv + ll, 1e-5);
}

vec3 F_Schlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

float F_Schlick(float f0, float f90, float VoH) {
    return f0 + (f90 - f0) * pow(1.0 - VoH, 5.0);
}

float Fd_Burley(float NoV, float NoL, float LoH, float roughness) {
    float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
    float light = F_Schlick(1.0, f90, NoL);
    float view = F_Schlick(1.0, f90, NoV);
    return light * view * INV_PI;
}

// Sheen (Estevez & Kulla "Charlie" distribution + Neubelt visibility).
float D_Charlie(float roughness, float NoH) {
    float inv_alpha = 1.0 / max(roughness, 0.01);
    float cos2h = NoH * NoH;
    float sin2h = max(1.0 - cos2h, 0.0078125);
    return (2.0 + inv_alpha) * pow(sin2h, inv_alpha * 0.5) / (2.0 * PI);
}

float V_Neubelt(float NoV, float NoL) {
    return 1.0 / (4.0 * (NoL + NoV - NoL * NoV) + 1e-5);
}

// Clearcoat: fixed IOR 1.5 (f0 = 0.04) and a cheaper visibility term.
float V_Kelemen(float LoH) {
    return 0.25 / max(LoH * LoH, 1e-4);
}

// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019): widens
// roughness where the normal changes quickly between pixels.
#ifdef STAGE_FRAGMENT
float specular_aa_roughness(vec3 n, float roughness) {
    vec3 du = dFdx(n);
    vec3 dv = dFdy(n);
    float variance = 0.25 * (dot(du, du) + dot(dv, dv));
    float kernel = min(2.0 * variance, 0.18);
    float a2 = roughness * roughness * roughness * roughness;
    return sqrt(sqrt(saturate(a2 + kernel)));
}
#endif

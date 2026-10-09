// Direct lighting for one light (all BRDF lobes) and the full per-pixel
// lighting loop (sun + clustered lights + indirect).
#pragma once
#include "ibl.glsl"
#include "brdf.glsl"

layout(binding = TEX_CONTACT) uniform sampler2D t_contact;

struct LightingResult {
    vec3 direct;
    vec3 indirect_diffuse;
    vec3 indirect_specular;
    float light_count;
};

// Radiance reflected toward the viewer from one light; `illuminance` is the
// light's illuminance at the surface (lux, perpendicular), shadow included.
vec3 shade_light(Surface s, vec3 l, vec3 illuminance) {
    vec3 n = s.normal;
    vec3 v = s.view;
    vec3 h = normalize(l + v);
    float NoL_raw = dot(n, l);
    float NoL = saturate(NoL_raw);
    float NoH = saturate(dot(n, h));
    float LoH = saturate(dot(l, h));
    float NoV = s.NoV;

    vec3 result = vec3(0.0);
    if (NoL > 0.0) {
        float D;
        float V;
        if (s.anisotropy != 0.0) {
            float at = max(s.alpha * (1.0 + s.anisotropy), 0.001);
            float ab = max(s.alpha * (1.0 - s.anisotropy), 0.001);
            vec3 t = s.anisotropic_t;
            vec3 b = s.anisotropic_b;
            D = D_GGX_Anisotropic(NoH, dot(t, h), dot(b, h), at, ab);
            V = V_SmithGGXCorrelated_Anisotropic(at, ab, dot(t, v), dot(b, v), dot(t, l), dot(b, l), NoV, NoL);
        } else {
            D = D_GGX(NoH, s.alpha);
            V = V_SmithGGXCorrelated(NoV, NoL, s.alpha);
        }
        vec3 F = F_Schlick(s.f0, LoH);
        vec3 specular = D * V * F * s.energy_compensation;
        vec3 diffuse = s.albedo * Fd_Burley(NoV, NoL, LoH, s.roughness);
        // Energy taken by the specular lobe isn't available to diffuse.
        diffuse *= (1.0 - F) * (1.0 - s.transmission);
        vec3 lobe = diffuse + specular;

        if (s.sheen_roughness > 0.0) {
            float sheen = D_Charlie(s.sheen_roughness, NoH) * V_Neubelt(NoV, NoL);
            float sheen_scaling = 1.0 - max3(s.sheen_color) * 0.157; // approximate albedo scaling
            lobe = lobe * sheen_scaling + s.sheen_color * sheen;
        }
        if (s.clearcoat > 0.0) {
            vec3 cn = s.clearcoat_normal;
            float cNoH = saturate(dot(cn, h));
            float cNoL = saturate(dot(cn, l));
            float ca = s.clearcoat_roughness * s.clearcoat_roughness;
            float Fc = F_Schlick(0.04, 1.0, LoH) * s.clearcoat;
            float coat = D_GGX(cNoH, ca) * V_Kelemen(LoH) * Fc;
            lobe = lobe * (1.0 - Fc) + vec3(coat) * (cNoL / max(NoL, 1e-4));
        }
        result = lobe * NoL;
    }
    // Subsurface approximation: wrapped diffuse + forward scattering
    // through thin parts.
    if (s.subsurface > 0.0) {
        float wrap = 0.5;
        float wrapped = saturate((NoL_raw + wrap) / ((1.0 + wrap) * (1.0 + wrap)));
        float back = pow(saturate(dot(v, -l)), 4.0) * 0.5;
        result += s.subsurface_color * s.base_color * s.subsurface * (wrapped * 0.5 + back) * INV_PI * (1.0 - NoL);
    }
    return result * illuminance;
}

float contact_shadow(vec2 screen_uv) {
    if (!has_feature(FEATURE_CONTACT_SHADOWS)) {
        return 1.0;
    }
    return textureLod(t_contact, screen_uv, 0.0).r;
}

LightingResult evaluate_lighting(inout Surface s, vec2 pixel, vec2 screen_uv, float view_z, bool screen_space, bool all_lights) {
    LightingResult r;
    r.direct = vec3(0.0);
    r.indirect_diffuse = vec3(0.0);
    r.indirect_specular = vec3(0.0);
    r.light_count = 0.0;

    vec4 dfg = textureLod(t_dfg, vec2(s.NoV, s.roughness), 0.0);
    s.dfg = dfg.xy;
    s.energy_compensation = 1.0 + s.f0 * (1.0 / max(dfg.x + dfg.y, 1e-3) - 1.0);

    // Directional lights come first in the light buffer.
    int directional = int(u_counts.w);
    for (int i = 0; i < directional; ++i) {
        Light light = lights[i];
        vec3 l = -light.direction_type.xyz;
        float shadow = 1.0;
        if (float(i) == u_sun_direction.w) {
            shadow = all_lights ? sun_shadow_world(s.position, s.geometric_normal, pixel) : sun_shadow(s.position, s.geometric_normal, pixel, view_z);
            if (screen_space) {
                shadow *= contact_shadow(screen_uv);
            }
        }
        r.direct += shade_light(s, l, light.color.rgb * shadow);
    }

    uint cluster = 0u;
    if (all_lights) {
        int total = u_flags.z;
        for (int i = directional; i < total; ++i) {
            Light light = lights[i];
            vec3 to_light = light.position_range.xyz - s.position;
            float d2 = dot(to_light, to_light);
            float range = light.position_range.w;
            if (d2 > range * range) {
                continue;
            }
            vec3 l = to_light * inversesqrt(max(d2, 1e-8));
            float atten = distance_attenuation(d2, range);
            if (int(light.direction_type.w) == LIGHT_SPOT) {
                atten *= spot_attenuation(light, l);
            }
            if (atten <= 0.0) {
                continue;
            }
            float shadow = local_light_shadow(light, s.position, s.geometric_normal, pixel);
            r.direct += shade_light(s, l, light.color.rgb * atten * shadow);
        }
    } else {
        cluster = cluster_index(pixel, view_z);
        uvec4 c = clusters[cluster];
        r.light_count = float(c.y);
        for (uint i = 0u; i < c.y; ++i) {
            Light light = lights[cluster_items[c.x + i]];
            vec3 to_light = light.position_range.xyz - s.position;
            float d2 = dot(to_light, to_light);
            float range = light.position_range.w;
            if (d2 > range * range) {
                continue;
            }
            vec3 l = to_light * inversesqrt(max(d2, 1e-8));
            float atten = distance_attenuation(d2, range);
            if (int(light.direction_type.w) == LIGHT_SPOT) {
                atten *= spot_attenuation(light, l);
            }
            if (atten <= 0.0) {
                continue;
            }
            float shadow = local_light_shadow(light, s.position, s.geometric_normal, pixel);
            r.direct += shade_light(s, l, light.color.rgb * atten * shadow);
        }
    }

    if (!has_feature(FEATURE_IBL)) {
        return r;
    }
    float ao = s.occlusion;
    if (screen_space && has_feature(FEATURE_AO)) {
        float ssao = textureLod(t_ao, screen_uv, 0.0).r;
        ao = min(ao, mix(1.0, ssao, u_gi.x));
    }
    vec3 diffuse_light = diffuse_environment(s.position, s.normal);
    r.indirect_diffuse = s.albedo * diffuse_light * ao * (1.0 - s.transmission);

    vec3 reflected = reflect(-s.view, s.normal);
    // Rough surfaces reflect closer to the normal (dominant direction).
    reflected = normalize(mix(s.normal, reflected, (1.0 - s.alpha) * (sqrt(1.0 - s.alpha) + s.alpha)));
    vec3 prefiltered = specular_environment(s.position, reflected, s.roughness, all_lights ? 0u : cluster);
    if (screen_space && has_feature(FEATURE_SSR)) {
        vec4 ssr = textureLod(t_ssr, screen_uv, 0.0);
        prefiltered = mix(prefiltered, ssr.rgb * u_exposure.w, ssr.a);
    }
    float so = mix(1.0, specular_occlusion(s.NoV, ao, s.roughness), u_gi.y);
    vec3 specular_color = (s.f0 * s.dfg.x + s.dfg.y) * s.energy_compensation;
    r.indirect_specular = prefiltered * specular_color * so;
    if (s.sheen_roughness > 0.0) {
        float sheen_dg = textureLod(t_dfg, vec2(s.NoV, s.sheen_roughness), 0.0).z;
        r.indirect_specular += s.sheen_color * sheen_dg * specular_environment(s.position, s.normal, s.sheen_roughness, cluster) * ao;
    }
    if (s.clearcoat > 0.0) {
        float cNoV = saturate(dot(s.clearcoat_normal, s.view));
        float Fc = F_Schlick(0.04, 1.0, cNoV) * s.clearcoat;
        vec3 cr = reflect(-s.view, s.clearcoat_normal);
        r.indirect_diffuse *= 1.0 - Fc;
        r.indirect_specular = r.indirect_specular * (1.0 - Fc) + specular_environment(s.position, cr, s.clearcoat_roughness, cluster) * Fc * so;
    }
    return r;
}

vec3 heatmap(float t) {
    t = saturate(t);
    return saturate(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

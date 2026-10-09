// Indirect lighting: the global environment (sky or HDRI, layer 0 of the
// probe cubemap array + an L2 SH for diffuse), local box-projected
// reflection probes, baked irradiance volumes (L1 SH), screen-space
// reflections and ambient/specular occlusion.
#pragma once
#include "lights.glsl"
#include "surface.glsl"

layout(binding = TEX_BRDF_LUT) uniform sampler2D t_dfg;
layout(binding = TEX_PROBES) uniform samplerCubeArray t_probes;
layout(binding = TEX_VOLUME_SH0) uniform sampler3D t_volume_sh0;
layout(binding = TEX_VOLUME_SH1) uniform sampler3D t_volume_sh1;
layout(binding = TEX_VOLUME_SH2) uniform sampler3D t_volume_sh2;
layout(binding = TEX_AO) uniform sampler2D t_ao;
layout(binding = TEX_SSR) uniform sampler2D t_ssr;

// L2 SH (cosine-convolved, /pi) of the sky: diffuse radiance for albedo 1.
vec3 sky_diffuse(vec3 n) {
    vec3 r = u_sky_sh[0].rgb * 0.282095;
    r += u_sky_sh[1].rgb * 0.488603 * n.y;
    r += u_sky_sh[2].rgb * 0.488603 * n.z;
    r += u_sky_sh[3].rgb * 0.488603 * n.x;
    r += u_sky_sh[4].rgb * 1.092548 * n.x * n.y;
    r += u_sky_sh[5].rgb * 1.092548 * n.y * n.z;
    r += u_sky_sh[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0);
    r += u_sky_sh[7].rgb * 1.092548 * n.x * n.z;
    r += u_sky_sh[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y);
    return max(r, vec3(0.0));
}

// Irradiance volumes: diffuse radiance a + b.n per channel. Returns the
// blend weight used (0 outside every volume).
float volume_diffuse(vec3 position, vec3 n, out vec3 result) {
    result = vec3(0.0);
    float total = 0.0;
    int count = int(u_counts.y);
    for (int i = 0; i < count && total < 1.0; ++i) {
        Volume vol = volumes[i];
        vec3 size = vol.box_max.xyz - vol.box_min.xyz;
        vec3 p = position + n * vol.params.x;
        vec3 local = (p - vol.box_min.xyz) / size;
        if (any(lessThan(local, vec3(0.0))) || any(greaterThan(local, vec3(1.0)))) {
            continue;
        }
        vec3 edge = min(local, 1.0 - local) * size;
        float w = saturate(min3(edge) / max(vol.params.y, 0.01));
        w = min(w, 1.0 - total);
        // Probe-centered texel coordinates inside this volume's slab of the atlas.
        vec3 res = vol.resolution.xyz;
        vec3 texel = local * (res - 1.0) + 0.5;
        vec3 atlas_size = vec3(textureSize(t_volume_sh0, 0));
        vec3 uvw = vec3(texel.xy / atlas_size.xy, (texel.z + vol.box_min.w) / atlas_size.z);
        vec4 s0 = texture(t_volume_sh0, uvw);
        vec4 s1 = texture(t_volume_sh1, uvw);
        vec4 s2 = texture(t_volume_sh2, uvw);
        vec3 a = s0.rgb;
        vec3 br = vec3(s0.a, s1.x, s1.y);
        vec3 bg = vec3(s1.z, s1.w, s2.x);
        vec3 bb = s2.yzw;
        vec3 irr = a + vec3(dot(br, n), dot(bg, n), dot(bb, n));
        result += max(irr, vec3(0.0)) * vol.box_max.w * w;
        total += w;
    }
    return total;
}

vec3 box_project(vec3 position, vec3 r, Probe probe) {
    if (probe.params.x < 0.5) {
        return r;
    }
    vec3 first = (probe.box_max.xyz - position) / r;
    vec3 second = (probe.box_min.xyz - position) / r;
    vec3 furthest = max(first, second);
    float distance = min3(furthest);
    vec3 hit = position + r * distance;
    return normalize(hit - probe.position.xyz);
}

float probe_weight(vec3 position, Probe probe) {
    vec3 d = min(position - probe.box_min.xyz, probe.box_max.xyz - position);
    float inside = min3(d);
    if (inside < 0.0) {
        return 0.0;
    }
    return saturate(inside / max(probe.box_min.w, 0.01));
}

vec3 sample_environment(vec3 r, float lod, float layer) {
    return textureLod(t_probes, vec4(r, layer), lod).rgb;
}

// Prefiltered specular radiance along r for this roughness, blending the
// cluster's reflection probes over the global environment.
vec3 specular_environment(vec3 position, vec3 r, float roughness, uint cluster) {
    float lod = roughness * (u_ambient.y - 1.0);
    vec3 result = vec3(0.0);
    float total = 0.0;
    if (has_feature(FEATURE_PROBES)) {
        uvec4 c = clusters[cluster];
        uint first = c.x + c.y + c.z;
        for (uint i = 0u; i < c.w && total < 1.0; ++i) {
            Probe probe = probes[cluster_items[first + i]];
            float w = min(probe_weight(position, probe), 1.0 - total);
            if (w <= 0.0) {
                continue;
            }
            vec3 dir = box_project(position, r, probe);
            result += sample_environment(dir, lod, probe.position.w) * probe.box_max.w * w;
            total += w;
        }
    }
    if (total < 1.0) {
        result += sample_environment(r, lod, 0.0) * u_ambient.x * (1.0 - total);
    }
    return result;
}

float specular_occlusion(float NoV, float ao, float roughness) {
    return saturate(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

vec3 diffuse_environment(vec3 position, vec3 n) {
    vec3 sky = sky_diffuse(n) * u_ambient.x;
    if (!has_feature(FEATURE_VOLUMES)) {
        return sky;
    }
    vec3 baked;
    float w = volume_diffuse(position, n, baked);
    return baked + sky * (1.0 - w);
}

// Material evaluation: samples a material's textures (with optional
// parallax occlusion mapping) and fills a Surface.
#pragma once
#include "frame.glsl"
#include "surface.glsl"
#include "brdf.glsl"

layout(binding = TEX_BASE_COLOR) uniform sampler2D t_base_color;
layout(binding = TEX_METAL_ROUGH) uniform sampler2D t_metal_rough;
layout(binding = TEX_NORMAL) uniform sampler2D t_normal;
layout(binding = TEX_EMISSIVE) uniform sampler2D t_emissive;
layout(binding = TEX_OCCLUSION) uniform sampler2D t_occlusion;
layout(binding = TEX_HEIGHT) uniform sampler2D t_height;
layout(binding = TEX_CLEARCOAT) uniform sampler2D t_clearcoat;
layout(binding = TEX_SHEEN) uniform sampler2D t_sheen;
layout(binding = TEX_TRANSMISSION) uniform sampler2D t_transmission;
layout(binding = TEX_TERRAIN_LAYERS) uniform sampler2DArray t_terrain_layers;
layout(binding = TEX_TERRAIN_NORMALS) uniform sampler2DArray t_terrain_normals;

bool has_texture(Material m, uint bit) { return (m.textures & bit) != 0u; }

// Parallax occlusion mapping: steps through the height field along the
// tangent-space view ray.
vec2 parallax_uv(Material m, vec2 uv, vec3 view_ts) {
    float scale = m.anisotropy.w;
    if (scale <= 0.0 || !has_texture(m, HAS_HEIGHT)) {
        return uv;
    }
    float steps = mix(32.0, 8.0, saturate(abs(view_ts.z)));
    float layer = 1.0 / steps;
    vec2 delta = view_ts.xy / max(view_ts.z, 0.1) * scale / steps;
    vec2 current = uv;
    float depth = 0.0;
    float height = 1.0 - texture(t_height, current).r;
    for (int i = 0; i < 32 && depth < height; ++i) {
        current -= delta;
        height = 1.0 - texture(t_height, current).r;
        depth += layer;
    }
    vec2 previous = current + delta;
    float after = height - depth;
    float before = (1.0 - texture(t_height, previous).r) - depth + layer;
    float w = after / (after - before + 1e-5);
    return mix(current, previous, w);
}

// Terrain: up to 4 layers blended by height/slope (uv in meters).
void terrain_layers(vec3 world, vec3 n, out vec3 albedo, out vec3 normal_ts, out float roughness) {
    float slope = 1.0 - saturate(n.y);
    float h = world.y;
    vec4 w;
    w.x = saturate(1.0 - slope * 3.0) * saturate(1.0 - (h - 6.0) * 0.2);        // grass
    w.y = saturate(slope * 3.0 - 0.6);                                           // rock
    w.z = saturate((h - 6.0) * 0.2) * saturate(1.0 - slope * 2.5);             // high ground
    w.w = saturate(1.0 - (h + 0.5) * 2.0);                                       // low sand/mud
    w = max(w, vec4(0.0));
    w /= max(dot(w, vec4(1.0)), 1e-4);
    vec2 uv = world.xz * 0.25;
    albedo = vec3(0.0);
    normal_ts = vec3(0.0);
    roughness = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (w[i] <= 0.001) {
            continue;
        }
        vec4 a = texture(t_terrain_layers, vec3(uv, float(i)));
        vec4 nr = texture(t_terrain_normals, vec3(uv, float(i)));
        albedo += a.rgb * w[i];
        normal_ts += (nr.xyz * 2.0 - 1.0) * w[i];
        roughness += a.a * w[i];
    }
    normal_ts = normalize(normal_ts + vec3(0.0, 0.0, 1e-3));
}

Surface evaluate_surface(Material m, vec3 world, vec3 vertex_normal, vec4 vertex_tangent, vec2 uv_in, vec3 vertex_color, bool front_facing) {
    Surface s;
    s.flags = m.flags;
    s.position = world;
    s.view = normalize(u_camera_position.xyz - world);

    vec3 n = normalize(vertex_normal);
    if (!front_facing) {
        n = -n;
    }
    s.geometric_normal = n;
    vec3 t = vertex_tangent.xyz - n * dot(n, vertex_tangent.xyz);
    t = length(t) > 1e-5 ? normalize(t) : vec3(1.0, 0.0, 0.0);
    vec3 b = cross(n, t) * (vertex_tangent.w < 0.0 ? -1.0 : 1.0);
    mat3 tbn = mat3(t, b, n);

    vec2 uv = uv_in * m.uv.xy + m.uv.zw;
    if ((m.flags & MATERIAL_PARALLAX) != 0u) {
        vec3 view_ts = transpose(tbn) * s.view;
        uv = parallax_uv(m, uv, view_ts);
    }

    vec4 base = m.base_color;
    base.rgb *= vertex_color;
    if (has_texture(m, HAS_BASE_COLOR)) {
        base *= texture(t_base_color, uv);
    }
    float metallic = m.pbr.x;
    float roughness = m.pbr.y;
    if (has_texture(m, HAS_METAL_ROUGH)) {
        vec4 mr = texture(t_metal_rough, uv);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    vec3 normal_ts = vec3(0.0, 0.0, 1.0);
    if (has_texture(m, HAS_NORMAL)) {
        // Z is reconstructed: BC5-compressed normal maps only keep XY.
        vec2 xy = texture(t_normal, uv).xy * 2.0 - 1.0;
        float z = sqrt(saturate(1.0 - dot(xy, xy)));
        normal_ts = normalize(vec3(xy * m.pbr.z, z));
    }
    if ((m.flags & MATERIAL_TERRAIN) != 0u) {
        vec3 layer_albedo;
        float layer_roughness;
        terrain_layers(world, n, layer_albedo, normal_ts, layer_roughness);
        base.rgb *= layer_albedo;
        roughness = layer_roughness;
    }
    s.normal = normalize(tbn * normal_ts);
    s.tangent = t;
    s.bitangent = b;

    float occlusion = 1.0;
    if (has_texture(m, HAS_OCCLUSION)) {
        occlusion = mix(1.0, texture(t_occlusion, uv).r, m.pbr.w);
    }
    vec3 emissive = m.emissive.rgb;
    if (has_texture(m, HAS_EMISSIVE)) {
        emissive *= texture(t_emissive, uv).rgb;
    }

    s.base_color = base.rgb;
    s.opacity = base.a;
    s.metallic = saturate(metallic);
    s.roughness = clamp(roughness, 0.03, 1.0);
    if (has_feature(FEATURE_SPECULAR_AA)) {
        s.roughness = specular_aa_roughness(s.normal, s.roughness);
    }
    s.alpha = s.roughness * s.roughness;
    s.occlusion = occlusion;
    // Exposure-relative emissive (w = 1) keeps glTF emissive factors
    // looking the same at any exposure; w = 0 means physical nits.
    s.emissive = emissive * (m.emissive.w > 0.5 ? u_exposure.w : 1.0);
    float specular = m.alpha_clearcoat.w;
    float ior = m.transmission.z;
    float f0_dielectric = sq((ior - 1.0) / (ior + 1.0)) * specular;
    s.f0 = mix(vec3(f0_dielectric), s.base_color, s.metallic);
    s.albedo = s.base_color * (1.0 - s.metallic);
    s.NoV = max(dot(s.normal, s.view), 1e-4);

    // Extensions
    s.clearcoat = 0.0;
    s.clearcoat_roughness = 0.0;
    s.clearcoat_normal = s.geometric_normal;
    if ((m.flags & MATERIAL_CLEARCOAT) != 0u) {
        s.clearcoat = m.alpha_clearcoat.y;
        s.clearcoat_roughness = m.alpha_clearcoat.z;
        if (has_texture(m, HAS_CLEARCOAT)) {
            vec2 cc = texture(t_clearcoat, uv).rg;
            s.clearcoat *= cc.r;
            s.clearcoat_roughness *= cc.g;
        }
        s.clearcoat_roughness = clamp(s.clearcoat_roughness, 0.03, 1.0);
    }
    s.sheen_color = vec3(0.0);
    s.sheen_roughness = 0.0;
    if ((m.flags & MATERIAL_SHEEN) != 0u) {
        s.sheen_color = m.sheen.rgb;
        s.sheen_roughness = m.sheen.w;
        if (has_texture(m, HAS_SHEEN)) {
            vec4 sh = texture(t_sheen, uv);
            s.sheen_color *= sh.rgb;
            s.sheen_roughness *= sh.a;
        }
        s.sheen_roughness = clamp(s.sheen_roughness, 0.05, 1.0);
    }
    s.transmission = 0.0;
    s.thickness = m.transmission.y;
    s.ior = ior;
    s.attenuation_color = m.attenuation.rgb;
    s.attenuation_distance = m.attenuation.w;
    if ((m.flags & MATERIAL_TRANSMISSION) != 0u) {
        s.transmission = m.transmission.x;
        if (has_texture(m, HAS_TRANSMISSION)) {
            vec2 tt = texture(t_transmission, uv).rg;
            s.transmission *= tt.r;
        }
    }
    s.anisotropy = 0.0;
    s.anisotropic_t = t;
    s.anisotropic_b = b;
    if ((m.flags & MATERIAL_ANISOTROPY) != 0u) {
        s.anisotropy = m.anisotropy.x;
        float c = cos(m.anisotropy.y);
        float sn = sin(m.anisotropy.y);
        vec3 at = t * c + b * sn;
        at = normalize(at - s.normal * dot(s.normal, at));
        s.anisotropic_t = at;
        s.anisotropic_b = cross(s.normal, at);
    }
    s.subsurface = 0.0;
    s.subsurface_color = vec3(1.0);
    if ((m.flags & MATERIAL_SUBSURFACE) != 0u) {
        s.subsurface = m.anisotropy.z;
        s.subsurface_color = m.subsurface.rgb;
    }
    s.dfg = vec2(1.0, 0.0);
    s.energy_compensation = vec3(1.0);
    return s;
}

// Alpha only (prepass/shadow passes).
float material_alpha(Material m, vec2 uv_in, vec3 vertex_color) {
    float alpha = m.base_color.a;
    vec2 uv = uv_in * m.uv.xy + m.uv.zw;
    if (has_texture(m, HAS_BASE_COLOR)) {
        alpha *= texture(t_base_color, uv).a;
    }
    return alpha;
}

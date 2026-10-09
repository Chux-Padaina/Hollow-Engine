// Forward shading for opaque (depth-equal after the prepass), transparent
// (sorted or weighted-blended OIT, with screen-space refraction) and probe
// capture passes. Outputs pre-exposed HDR radiance.
#include "lighting.glsl"
#include "material.glsl"
#include "fog.glsl"

in VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
} v_in;
flat in uint v_instance;

layout(binding = TEX_SCENE_COLOR) uniform sampler2D t_scene_color;
layout(binding = TEX_SCENE_DEPTH) uniform sampler2D t_scene_depth;
layout(binding = TEX_DECALS) uniform sampler2DArray t_decals;

#if defined(PASS_TRANSPARENT) && defined(WBOIT)
layout(location = 0) out vec4 o_accum;
layout(location = 1) out float o_reveal;
#else
layout(location = 0) out vec4 o_color;
#endif

void apply_decals(inout Surface s, uint cluster) {
    uvec4 c = clusters[cluster];
    uint first = c.x + c.y;
    for (uint i = 0u; i < c.z; ++i) {
        Decal d = decals[cluster_items[first + i]];
        vec3 local = (d.world_to_decal * vec4(s.position, 1.0)).xyz;
        if (any(greaterThan(abs(local), vec3(0.5)))) {
            continue;
        }
        vec3 axis = normalize(transpose(mat3(d.world_to_decal))[1]);
        float facing = dot(s.geometric_normal, axis);
        float angle_fade = saturate((facing - d.params2.x) / max(1.0 - d.params2.x, 0.01));
        float edge_fade = saturate((0.5 - abs(local.y)) * 8.0);
        vec2 uv = vec2(local.x + 0.5, 0.5 - local.z);
        vec4 albedo = texture(t_decals, vec3(uv, d.params.x));
        albedo.rgb = srgb_to_linear(albedo.rgb);
        float a = albedo.a * d.color.a * angle_fade * edge_fade;
        if (a <= 0.001) {
            continue;
        }
        s.base_color = mix(s.base_color, albedo.rgb * d.color.rgb, a);
        if (d.params.w >= 0.0) {
            s.roughness = mix(s.roughness, d.params.w, a);
        }
        if (d.params2.y >= 0.0) {
            s.metallic = mix(s.metallic, d.params2.y, a);
        }
        if (d.params.y >= 0.0) {
            vec3 nt = texture(t_decals, vec3(uv, d.params.y)).xyz * 2.0 - 1.0;
            nt.xy *= d.params.z;
            vec3 t = normalize(transpose(mat3(d.world_to_decal))[0]);
            vec3 b = cross(s.normal, t);
            t = cross(b, s.normal);
            s.normal = normalize(mix(s.normal, normalize(mat3(t, b, s.normal) * nt), a));
        }
    }
    s.albedo = s.base_color * (1.0 - s.metallic);
    s.f0 = mix(vec3(sq((s.ior - 1.0) / (s.ior + 1.0))), s.base_color, s.metallic);
    s.alpha = s.roughness * s.roughness;
    s.NoV = max(dot(s.normal, s.view), 1e-4);
}

void main() {
    Instance inst = instances[v_instance];
    Material m = materials[inst.material];
    Surface s = evaluate_surface(m, v_in.world_position, v_in.normal, v_in.tangent, v_in.uv, v_in.color, gl_FrontFacing);
#if defined(ALPHA_MASK)
    if (s.opacity < m.alpha_clearcoat.x) {
        discard;
    }
#endif

    vec2 pixel = gl_FragCoord.xy;
    vec2 screen_uv = pixel * u_render_size.zw;
    float view_z = dot(s.position - u_camera_position.xyz, u_camera_forward.xyz);
#if defined(PASS_CAPTURE)
    bool screen_space = false;
    bool all_lights = true;
#else
    bool screen_space = true;
    bool all_lights = false;
    if (has_feature(FEATURE_DECALS)) {
        apply_decals(s, cluster_index(pixel, view_z));
    }
#endif
#if defined(PASS_TRANSPARENT)
    screen_space = false; // AO/SSR/contact textures describe the opaque surface behind
#endif

    vec3 color;
    if ((m.flags & MATERIAL_UNLIT) != 0u) {
        color = s.base_color + s.emissive;
    } else {
        LightingResult lit = evaluate_lighting(s, pixel, screen_uv, view_z, screen_space, all_lights);
        color = lit.direct + lit.indirect_diffuse + lit.indirect_specular + s.emissive;

#if defined(PASS_TRANSPARENT)
        if (s.transmission > 0.0 && has_feature(FEATURE_REFRACTION)) {
            // Screen-space refraction: march the refracted ray a thickness
            // into the volume and sample the (blurred) opaque scene there.
            vec3 refr = refract(-s.view, s.normal, 1.0 / max(s.ior, 1.0));
            float thickness = max(s.thickness, 0.05);
            vec3 exit_point = s.position + refr * thickness;
            vec2 ruv = project_to_uv(exit_point);
            ruv = clamp(ruv, vec2(0.0), vec2(1.0));
            float lod = s.roughness * 6.0;
            vec3 behind = textureLod(t_scene_color, ruv, lod).rgb * u_exposure.w;
            if (s.attenuation_distance > 0.0) {
                vec3 sigma = -log(max(s.attenuation_color, vec3(1e-4))) / s.attenuation_distance;
                behind *= exp(-sigma * thickness);
            }
            vec3 F = F_Schlick(s.f0, s.NoV);
            vec3 transmitted = behind * s.base_color * (1.0 - F);
            color += transmitted * s.transmission;
            s.opacity = 1.0; // refraction replaces blending
        }
#endif

        // Debug views
        int debug = u_flags.x;
        if (debug != DEBUG_FINAL) {
            switch (debug) {
            case DEBUG_ALBEDO: color = s.base_color; break;
            case DEBUG_NORMALS: color = s.normal * 0.5 + 0.5; break;
            case DEBUG_DIRECT: color = lit.direct; break;
            case DEBUG_ROUGHNESS: color = vec3(s.roughness); break;
            case DEBUG_METALLIC: color = vec3(s.metallic); break;
            case DEBUG_AO: color = vec3(s.occlusion * (screen_space && has_feature(FEATURE_AO) ? textureLod(t_ao, screen_uv, 0.0).r : 1.0)); break;
            case DEBUG_EMISSIVE: color = s.emissive; break;
            case DEBUG_INDIRECT: color = lit.indirect_diffuse + lit.indirect_specular; break;
            case DEBUG_REFLECTIONS: color = lit.indirect_specular; break;
            case DEBUG_LIGHT_COMPLEXITY: color = heatmap(lit.light_count / 16.0); break;
            case DEBUG_CASCADES: {
                int c = cascade_for(view_z);
                vec3 tints[5] = vec3[](vec3(1, 0.3, 0.3), vec3(0.3, 1, 0.3), vec3(0.3, 0.3, 1), vec3(1, 1, 0.3), vec3(0.6));
                color = s.base_color * tints[c];
                break;
            }
            case DEBUG_LODS: {
                vec3 tints[4] = vec3[](vec3(0.3, 1, 0.3), vec3(1, 1, 0.3), vec3(1, 0.6, 0.2), vec3(1, 0.25, 0.25));
                color = tints[min(int(inst.batch >> 24u), 3)] * (0.4 + 0.6 * saturate(dot(s.normal, vec3(0.3, 0.9, 0.2))));
                break;
            }
            case DEBUG_INSTANCES: {
                uint h = hash_u32(v_instance * 2654435761u + inst.mesh);
                color = vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
                break;
            }
            default: break;
            }
            // Debug colors are shown unexposed.
            color *= u_exposure.w;
            if (debug == DEBUG_DIRECT || debug == DEBUG_INDIRECT || debug == DEBUG_REFLECTIONS || debug == DEBUG_EMISSIVE) {
                color *= u_exposure.x;
            }
        }
    }

    color *= u_exposure.x;
#if !defined(PASS_CAPTURE)
    if (u_flags.x == DEBUG_FINAL) {
        color = apply_fog(color, s.position, screen_uv, view_z).rgb;
    }
#endif

#if defined(PASS_TRANSPARENT) && defined(WBOIT)
    // McGuire & Bavoil 2013 weighted blended OIT.
    float alpha = s.opacity;
    float z = view_z;
    float weight = clamp(alpha * max(1e-2, 3e3 * pow(1.0 - min(z / 200.0, 1.0), 3.0)), 1e-2, 3e3);
    o_accum = vec4(color * alpha, alpha) * weight;
    o_reveal = alpha;
#elif defined(PASS_TRANSPARENT)
    o_color = vec4(color * s.opacity, s.opacity); // premultiplied
#else
    o_color = vec4(color, 1.0);
#endif
}

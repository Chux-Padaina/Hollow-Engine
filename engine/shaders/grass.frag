// vertex: grass.vert
// Grass blades: prepass (normals + motion) or forward shading with a
// translucent (subsurface) term and root darkening.
#include "lighting.glsl"
#include "fog.glsl"

in GrassData {
    vec3 world_position;
    vec3 normal;
    vec2 uv;
    float tint;
#if defined(PASS_PREPASS)
    vec4 current_clip;
    vec4 previous_clip;
#endif
} v_in;

uniform vec3 u_color;

#if defined(PASS_PREPASS)
layout(location = 0) out vec4 o_normal_roughness;
layout(location = 1) out vec2 o_velocity;
#else
layout(location = 0) out vec4 o_color;
#endif

void main() {
    vec3 n = normalize(v_in.normal) * (gl_FrontFacing ? 1.0 : -1.0);
#if defined(PASS_PREPASS)
    o_normal_roughness = vec4(encode_normal(n) * 0.5 + 0.5, 0.7, 0.0);
    o_velocity = v_in.current_clip.xy / v_in.current_clip.w - v_in.previous_clip.xy / v_in.previous_clip.w;
#else
    float h = v_in.uv.y;
    vec3 tip = u_color * mix(vec3(0.9, 1.05, 0.8), vec3(1.25, 1.15, 0.7), v_in.tint);
    vec3 albedo = mix(u_color * 0.35, tip, smoothstep(0.0, 0.9, h));
    Surface s = make_surface(v_in.world_position, n, n, albedo, 0.65, 0.0);
    s.occlusion = mix(0.35, 1.0, h);
    s.subsurface = 0.8;
    s.subsurface_color = vec3(1.1, 1.2, 0.7);
    vec2 pixel = gl_FragCoord.xy;
    vec2 screen_uv = pixel * u_render_size.zw;
    float view_z = dot(s.position - u_camera_position.xyz, u_camera_forward.xyz);
    LightingResult lit = evaluate_lighting(s, pixel, screen_uv, view_z, true, false);
    vec3 color = lit.direct + lit.indirect_diffuse + lit.indirect_specular;
    int debug = u_flags.x;
    if (debug != DEBUG_FINAL) {
        switch (debug) {
        case DEBUG_ALBEDO: color = albedo; break;
        case DEBUG_NORMALS: color = n * 0.5 + 0.5; break;
        case DEBUG_DIRECT: color = lit.direct * u_exposure.x; break;
        case DEBUG_INDIRECT: color = (lit.indirect_diffuse + lit.indirect_specular) * u_exposure.x; break;
        case DEBUG_REFLECTIONS: color = lit.indirect_specular * u_exposure.x; break;
        case DEBUG_ROUGHNESS: color = vec3(s.roughness); break;
        case DEBUG_METALLIC: color = vec3(0.0); break;
        case DEBUG_AO: color = vec3(s.occlusion); break;
        case DEBUG_EMISSIVE: color = vec3(0.0); break;
        default: color = albedo; break;
        }
        color *= u_exposure.w; // shown unexposed, like forward.frag
    }
    color *= u_exposure.x;
    if (debug == DEBUG_FINAL) {
        color = apply_fog(color, s.position, screen_uv, view_z).rgb;
    }
    o_color = vec4(color, 1.0);
#endif
}

// Depth prepass: writes depth, the shading normal (octahedral) + roughness,
// and screen-space motion vectors.
#include "material.glsl"

in VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
    vec4 current_clip;
    vec4 previous_clip;
} v_in;
flat in uint v_instance;

layout(location = 0) out vec4 o_normal_roughness; // RGB10A2: oct normal xy, roughness, unused
layout(location = 1) out vec2 o_velocity;         // NDC delta (current - previous), unjittered

void main() {
    Instance inst = instances[v_instance];
    Material m = materials[inst.material];
#if defined(ALPHA_MASK)
    if (material_alpha(m, v_in.uv, v_in.color) < m.alpha_clearcoat.x) {
        discard;
    }
#endif
    Surface s = evaluate_surface(m, v_in.world_position, v_in.normal, v_in.tangent, v_in.uv, v_in.color, gl_FrontFacing);
    o_normal_roughness = vec4(encode_normal(s.normal) * 0.5 + 0.5, s.roughness, (m.flags & MATERIAL_WATER) != 0u ? 1.0 : 0.0);
    vec2 current = v_in.current_clip.xy / v_in.current_clip.w;
    vec2 previous = v_in.previous_clip.xy / v_in.previous_clip.w;
    o_velocity = current - previous;
}

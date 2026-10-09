// Editor selection mask: the selected actor's view distance (0 elsewhere).
// The outline composite compares it with the scene depth to dim the parts
// of the outline that are hidden behind other geometry.
#include "material.glsl"

in VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
} v_in;
flat in uint v_instance;

layout(location = 0) out float o_mask;

void main() {
#if defined(ALPHA_MASK)
    Material m = materials[instances[v_instance].material];
    if (material_alpha(m, v_in.uv, v_in.color) < m.alpha_clearcoat.x) {
        discard;
    }
#endif
    o_mask = max(dot(v_in.world_position - u_camera_position.xyz, u_camera_forward.xyz), 1e-3);
}

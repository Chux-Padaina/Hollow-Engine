// Shadow map pass: depth only, with alpha testing for masked materials.
#include "material.glsl"

in VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
} v_in;
flat in uint v_instance;

void main() {
#if defined(ALPHA_MASK)
    Material m = materials[instances[v_instance].material];
    if (material_alpha(m, v_in.uv, v_in.color) < m.alpha_clearcoat.x) {
        discard;
    }
#endif
}

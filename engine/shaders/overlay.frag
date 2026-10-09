// Editor overlay fragments: hidden parts (behind scene geometry) are drawn
// faded instead of disappearing.
// vertex: overlay.vert
#include "frame.glsl"

in vec4 v_color;
in float v_depth;
flat in float v_flags;
layout(binding = 0) uniform sampler2D t_scene_depth; // render res, reversed-Z
layout(location = 0) out vec4 o_color;

void main() {
    vec4 color = v_color;
    if (v_flags > 0.5) {
        vec2 uv = gl_FragCoord.xy * u_output_size.zw;
        float d = textureLod(t_scene_depth, uv, 0.0).r;
        float scene = d > 0.0 ? linear_depth(d) : 1e9;
        if (v_depth > scene * 1.002 + 0.02) {
            color.a *= 0.28;
        }
    }
    o_color = color;
}

// Weighted blended OIT resolve (McGuire & Bavoil 2013), blended over the
// scene with SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
#include "frame.glsl"

layout(binding = 0) uniform sampler2D t_accum;
layout(binding = 1) uniform sampler2D t_reveal;

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float reveal = texelFetch(t_reveal, p, 0).r;
    if (reveal >= 0.9999) {
        discard;
    }
    vec4 accum = texelFetch(t_accum, p, 0);
    if (isinf(max(max(abs(accum.r), abs(accum.g)), abs(accum.b)))) {
        accum.rgb = vec3(accum.a);
    }
    o_color = vec4(accum.rgb / max(accum.a, 1e-5), 1.0 - reveal);
}

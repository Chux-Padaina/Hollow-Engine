// Selection outline from the selection mask (view distance of the selected
// actor, 0 elsewhere): neon lime where the selection is visible, faded
// where it's hidden behind other geometry.
#include "frame.glsl"

in vec2 v_uv;
layout(binding = 0) uniform sampler2D t_mask;        // output res
layout(binding = 1) uniform sampler2D t_scene_depth; // render res
layout(location = 0) out vec4 o_color;
uniform float u_width;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(t_mask, 0);
    float center = texelFetch(t_mask, p, 0).r;
    float nearest = 1e9;
    int r = int(ceil(u_width));
    float coverage = 0.0;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y > r * r) {
                continue;
            }
            float m = texelFetch(t_mask, clamp(p + ivec2(x, y), ivec2(0), size - 1), 0).r;
            if (m > 0.0) {
                coverage = 1.0;
                nearest = min(nearest, m);
            }
        }
    }
    if (center > 0.0) {
        // Inside: a faint lime tint where the selection is hidden.
        float d = textureLod(t_scene_depth, v_uv, 0.0).r;
        float scene = d > 0.0 ? linear_depth(d) : 1e9;
        if (center > scene * 1.002 + 0.02) {
            o_color = vec4(0.831, 1.0, 0.247, 0.10);
            return;
        }
        discard;
    }
    if (coverage <= 0.0) {
        discard;
    }
    float d = textureLod(t_scene_depth, v_uv, 0.0).r;
    float scene = d > 0.0 ? linear_depth(d) : 1e9;
    bool hidden = nearest > scene * 1.002 + 0.02;
    o_color = vec4(0.831, 1.0, 0.247, hidden ? 0.35 : 1.0);
}

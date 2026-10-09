// FXAA (Lottes), compact quality variant: luma edge detection, edge
// direction from the 3x3 neighborhood, and a short search along the edge.
#include "common.glsl"

in vec2 v_uv;
layout(binding = 0) uniform sampler2D t_input; // display-encoded LDR
layout(location = 0) out vec4 o_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 texel = 1.0 / vec2(textureSize(t_input, 0));
    vec3 rgbM = textureLod(t_input, v_uv, 0.0).rgb;
    float lM = luma(rgbM);
    float lN = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(0, 1)).rgb);
    float lS = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(0, -1)).rgb);
    float lE = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(1, 0)).rgb);
    float lW = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(-1, 0)).rgb);
    float lmin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lmax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range = lmax - lmin;
    if (range < max(0.0312, lmax * 0.125)) {
        o_color = vec4(rgbM, 1.0);
        return;
    }
    float lNW = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(-1, 1)).rgb);
    float lNE = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(1, 1)).rgb);
    float lSW = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(-1, -1)).rgb);
    float lSE = luma(textureLodOffset(t_input, v_uv, 0.0, ivec2(1, -1)).rgb);
    float horizontal = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    float vertical = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    bool is_horizontal = horizontal >= vertical;
    float l1 = is_horizontal ? lS : lW;
    float l2 = is_horizontal ? lN : lE;
    float g1 = abs(l1 - lM);
    float g2 = abs(l2 - lM);
    bool steepest_first = g1 >= g2;
    float step_len = is_horizontal ? texel.y : texel.x;
    float local_avg;
    float gradient = 0.25 * max(g1, g2);
    if (steepest_first) {
        step_len = -step_len;
        local_avg = 0.5 * (l1 + lM);
    } else {
        local_avg = 0.5 * (l2 + lM);
    }
    vec2 cur = v_uv;
    if (is_horizontal) cur.y += step_len * 0.5; else cur.x += step_len * 0.5;
    vec2 offset = is_horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uv1 = cur - offset;
    vec2 uv2 = cur + offset;
    float e1 = luma(textureLod(t_input, uv1, 0.0).rgb) - local_avg;
    float e2 = luma(textureLod(t_input, uv2, 0.0).rgb) - local_avg;
    bool done1 = abs(e1) >= gradient;
    bool done2 = abs(e2) >= gradient;
    const float steps[8] = float[](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 8 && !(done1 && done2); ++i) {
        if (!done1) { uv1 -= offset * steps[i]; e1 = luma(textureLod(t_input, uv1, 0.0).rgb) - local_avg; done1 = abs(e1) >= gradient; }
        if (!done2) { uv2 += offset * steps[i]; e2 = luma(textureLod(t_input, uv2, 0.0).rgb) - local_avg; done2 = abs(e2) >= gradient; }
    }
    float d1 = is_horizontal ? v_uv.x - uv1.x : v_uv.y - uv1.y;
    float d2 = is_horizontal ? uv2.x - v_uv.x : uv2.y - v_uv.y;
    bool direction1 = d1 < d2;
    float dmin = min(d1, d2);
    float edge_length = d1 + d2;
    float pixel_offset = -dmin / edge_length + 0.5;
    bool center_smaller = lM < local_avg;
    bool correct = ((direction1 ? e1 : e2) < 0.0) != center_smaller;
    float final_offset = correct ? pixel_offset : 0.0;
    // Subpixel aliasing.
    float l_avg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = saturate(abs(l_avg - lM) / range);
    sub = sq((-2.0 * sub + 3.0) * sub * sub) * 0.75;
    final_offset = max(final_offset, sub);
    vec2 final_uv = v_uv;
    if (is_horizontal) final_uv.y += final_offset * step_len; else final_uv.x += final_offset * step_len;
    o_color = vec4(textureLod(t_input, final_uv, 0.0).rgb, 1.0);
}

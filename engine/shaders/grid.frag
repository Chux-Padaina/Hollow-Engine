// Infinite editor grid on y = 0: 1 m minor and 10 m major lines,
// antialiased with screen-space derivatives, fading with distance; X and Z
// axes in red and blue. Hidden behind scene geometry.
#include "frame.glsl"

in vec2 v_uv;
layout(binding = 0) uniform sampler2D t_scene_depth;
layout(location = 0) out vec4 o_color;

float grid_line(vec2 coord, float scale) {
    vec2 c = coord / scale;
    vec2 d = fwidth(c);
    vec2 g = abs(fract(c - 0.5) - 0.5) / max(d, vec2(1e-5));
    float line = min(g.x, g.y);
    return 1.0 - min(line, 1.0);
}

void main() {
    vec2 ndc = v_uv * 2.0 - 1.0;
    // Unjittered inverse: build rays from the unjittered view-projection.
    mat4 inv = inverse(u_unjittered_view_projection);
    vec4 near_p = inv * vec4(ndc, 1.0, 1.0);
    vec4 far_p = inv * vec4(ndc, 0.0001, 1.0);
    vec3 ro = near_p.xyz / near_p.w;
    vec3 rd = normalize(far_p.xyz / far_p.w - ro);
    if (abs(rd.y) < 1e-5) {
        discard;
    }
    float t = -ro.y / rd.y;
    if (t <= 0.0) {
        discard;
    }
    vec3 hit = ro + rd * t;
    float view_z = dot(hit - u_camera_position.xyz, u_camera_forward.xyz);
    float d = textureLod(t_scene_depth, v_uv, 0.0).r;
    // Generous tolerance: the depth buffer is TAA-jittered (and may be at a
    // lower render scale), so surfaces lying on y = 0 must not flicker.
    if (d > 0.0 && linear_depth(d) < view_z * 0.97 - 0.02) {
        discard;
    }
    float dist = length(hit.xz - u_camera_position.xz);
    float height = abs(u_camera_position.y);
    float fade = 1.0 - smoothstep(height * 6.0 + 20.0, height * 12.0 + 80.0, dist);
    float minor = grid_line(hit.xz, 1.0) * 0.32;
    float major = grid_line(hit.xz, 10.0) * 0.55;
    vec3 color = vec3(0.62, 0.62, 0.66);
    float alpha = max(minor, major);
    // Axes
    vec2 width = fwidth(hit.xz) * 1.4;
    float axis_x = 1.0 - smoothstep(0.0, width.y, abs(hit.z)); // the X axis lies on z = 0
    float axis_z = 1.0 - smoothstep(0.0, width.x, abs(hit.x));
    if (axis_x > 0.0) {
        color = mix(color, vec3(0.93, 0.33, 0.36), axis_x);
        alpha = max(alpha, axis_x * 0.9);
    }
    if (axis_z > 0.0) {
        color = mix(color, vec3(0.33, 0.58, 0.98), axis_z);
        alpha = max(alpha, axis_z * 0.9);
    }
    alpha *= fade;
    if (alpha < 0.003) {
        discard;
    }
    o_color = vec4(color, alpha);
}

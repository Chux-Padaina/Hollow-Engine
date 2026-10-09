// Editor overlay geometry. Lines are expanded to screen-space quads of a
// given pixel width (wide GL lines aren't available in core profiles);
// solids pass through.
#include "frame.glsl"

layout(location = 0) in vec3 a_position;  // line: endpoint A, solid: position
layout(location = 1) in vec3 a_other;     // line: endpoint B
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_params;    // x side (-1/1), y endpoint (0 = A, 1 = B), z width px, w flags (1 = depth fade)

out vec4 v_color;
out float v_depth;
flat out float v_flags;

uniform int u_solid;

void main() {
    v_color = a_color;
    v_flags = a_params.w;
    if (u_solid != 0) {
        vec4 clip = u_unjittered_view_projection * vec4(a_position, 1.0);
        v_depth = clip.w;
        gl_Position = clip;
        return;
    }
    vec4 ca = u_unjittered_view_projection * vec4(a_position, 1.0);
    vec4 cb = u_unjittered_view_projection * vec4(a_other, 1.0);
    vec2 sa = ca.xy / ca.w * u_output_size.xy * 0.5;
    vec2 sb = cb.xy / cb.w * u_output_size.xy * 0.5;
    vec2 dir = sb - sa;
    float len = length(dir);
    dir = len > 1e-4 ? dir / len : vec2(1.0, 0.0);
    vec2 normal = vec2(-dir.y, dir.x);
    vec4 clip = a_params.y < 0.5 ? ca : cb;
    vec2 offset = normal * a_params.x * a_params.z * 0.5;
    // Extend slightly along the line so joints overlap.
    offset += dir * (a_params.y < 0.5 ? -1.0 : 1.0) * a_params.z * 0.25;
    clip.xy += offset / (u_output_size.xy * 0.5) * clip.w;
    v_depth = clip.w;
    gl_Position = clip;
}

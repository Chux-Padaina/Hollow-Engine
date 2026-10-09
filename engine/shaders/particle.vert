// Camera-facing particle quads (6 vertices per particle slot; dead slots
// collapse to nothing).
#include "frame.glsl"
#include "common.glsl"
#include "particle_common.glsl"

uniform mat4 u_model; // identity for world-space systems
uniform vec2 u_size;  // start, end
uniform vec3 u_color_start;
uniform vec3 u_color_end;
uniform vec2 u_alpha; // start, end

out vec4 v_color;
out vec2 v_uv;
out float v_view_z;
out vec3 v_world;

void main() {
    uint index = uint(gl_VertexID) / 6u;
    int corner = gl_VertexID % 6;
    Particle p = particles[index];
    if (p.position_age.w >= p.velocity_life.w) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_color = vec4(0.0);
        v_uv = vec2(0.0);
        v_view_z = 1.0;
        v_world = vec3(0.0);
        return;
    }
    float t = saturate(p.position_age.w / p.velocity_life.w);
    const vec2 quad[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
    vec2 q = quad[corner];
    float angle = p.extra.x * TWO_PI + t * (p.extra.y - 0.5) * 3.0;
    vec2 r = vec2(q.x * cos(angle) - q.y * sin(angle), q.x * sin(angle) + q.y * cos(angle));
    float size = mix(u_size.x, u_size.y, t) * 0.5;
    vec3 center = (u_model * vec4(p.position_age.xyz, 1.0)).xyz;
    vec3 right = u_inv_view[0].xyz;
    vec3 up = u_inv_view[1].xyz;
    vec3 world = center + (right * r.x + up * r.y) * size;
    // Fade in quickly, out over the configured alpha curve.
    float alpha = mix(u_alpha.x, u_alpha.y, t) * saturate(t * 12.0);
    v_color = vec4(mix(u_color_start, u_color_end, t), alpha);
    v_uv = q * 0.5 + 0.5;
    v_view_z = dot(world - u_camera_position.xyz, u_camera_forward.xyz);
    v_world = world;
    gl_Position = u_view_projection * vec4(world, 1.0);
}

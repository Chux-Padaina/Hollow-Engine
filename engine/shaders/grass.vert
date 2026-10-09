// Grass blades generated from the vertex id: 3 tapered quads + a tip
// (21 vertices), bent and swayed by the wind. Used by the prepass
// (PASS_PREPASS: + motion vectors) and the forward pass.
#include "frame.glsl"
#include "common.glsl"
#include "grass_common.glsl"

layout(std430, binding = SSBO_SCRATCH1) readonly buffer GrassVisible {
    uint grass_visible[];
};

out GrassData {
    vec3 world_position;
    vec3 normal;
    vec2 uv; // x across, y up the blade
    float tint;
#if defined(PASS_PREPASS)
    vec4 current_clip;
    vec4 previous_clip;
#endif
} v_out;
invariant gl_Position;

vec3 blade_point(vec3 base, vec4 a, vec4 b, float across, float h, float t, float scale, out vec3 tangent_up) {
    float height = b.x * scale;
    float width = b.y;
    vec3 facing = vec3(cos(a.w), 0.0, sin(a.w));
    vec3 side = vec3(-facing.z, 0.0, facing.x);
    float bend = 0.25 * height;
    float gust = sin(t * 1.3 + base.x * 0.21 + base.z * 0.17) * 0.5 + 0.5;
    float sway = u_wind * (sin(t * 2.3 + b.w + base.x * 0.4) * 0.35 + gust * 0.65) * 0.35 * height;
    vec3 wind_dir = normalize(vec3(1.0, 0.0, 0.4));
    float h2 = h * h;
    vec3 p = base + vec3(0.0, height * h, 0.0) + facing * bend * h2 + wind_dir * sway * h2;
    p.y -= sway * h2 * 0.3; // keep the length roughly constant
    p += side * (across - 0.5) * width * (1.0 - h * 0.85);
    tangent_up = vec3(0.0, height, 0.0) + facing * bend * 2.0 * h + wind_dir * sway * 2.0 * h;
    return p;
}

void main() {
    uint blade = grass_visible[u_offset + uint(gl_InstanceID)];
    vec4 a = grass_blades[blade * 2u];
    vec4 b = grass_blades[blade * 2u + 1u];
    vec3 base = (u_model * vec4(a.xyz, 1.0)).xyz;
    float d = distance(base, u_camera_position.xyz);
    // Thinned blades get wider; blades shrink away near the draw distance.
    b.y /= grass_keep(d);
    float scale = 1.0 - smoothstep(0.8, 1.0, d / u_distance);

    int v = gl_VertexID;
    float across;
    float level;
    if (v < 18) {
        int segment = v / 6;
        int corner = v % 6;
        const vec2 quad[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
        across = quad[corner].x;
        level = float(segment) + quad[corner].y;
    } else {
        int corner = v - 18;
        across = corner == 0 ? 0.0 : (corner == 1 ? 1.0 : 0.5);
        level = corner == 2 ? 4.0 : 3.0;
    }
    float h = level / 4.0;
    vec3 up_tangent;
    vec3 world = blade_point(base, a, b, across, h, u_time.y, scale, up_tangent);
    vec3 facing = vec3(cos(a.w), 0.0, sin(a.w));
    vec3 side = vec3(-facing.z, 0.0, facing.x);
    vec3 n = normalize(cross(side, normalize(up_tangent)));
    // Round the blade a little across its width.
    n = normalize(n + side * (across - 0.5) * 0.6);

    v_out.world_position = world;
    v_out.normal = n;
    v_out.uv = vec2(across, h);
    v_out.tint = b.z;
    gl_Position = u_view_projection * vec4(world, 1.0);
#if defined(PASS_PREPASS)
    vec3 unused;
    vec3 previous = blade_point(base, a, b, across, h, u_time.y - u_time.x, scale, unused);
    v_out.current_clip = u_unjittered_view_projection * vec4(world, 1.0);
    v_out.previous_clip = u_prev_view_projection * vec4(previous, 1.0);
#endif
}

// Water surface: Gerstner waves displaced in world space. The water
// parameters are packed into the material (see RendererScene.cpp):
//   anisotropy = (wave height, wind direction, steepness, wavelength)
//   alpha_clearcoat.y = wave speed
#include "frame.glsl"
#include "scene_data.glsl"
#include "water_waves.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_tangent;
layout(location = 3) in float a_tangent_sign;
layout(location = 4) in vec2 a_uv;
layout(location = 5) in vec3 a_color;
layout(location = 6) in uint a_draw_id;

out VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
} v_out;
flat out uint v_instance;

void main() {
    Instance inst = instances[a_draw_id];
    Material m = materials[inst.material];
    v_instance = a_draw_id;
    vec3 world = (inst.model * vec4(a_position, 1.0)).xyz;
    // Fade the waves out at the edges of the surface so it meets the shore.
    vec2 edge = 1.0 - abs(a_uv * 2.0 - 1.0);
    float fade = saturate(min(edge.x, edge.y) * 12.0);
    vec3 normal;
    vec3 displaced = gerstner(world, m, fade, normal);
    v_out.world_position = displaced;
    v_out.normal = normal;
    v_out.tangent = vec4(1.0, 0.0, 0.0, 1.0);
    v_out.uv = a_uv;
    v_out.color = vec3(displaced.y - world.y, fade, 0.0); // crest height for foam
    gl_Position = u_view_projection * vec4(displaced, 1.0);
}

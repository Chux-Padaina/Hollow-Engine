// Vertex shader for every mesh pass. Instances come from the instance
// buffer; the draw id is a per-instance attribute (divisor 1) whose value
// is offset by each indirect command's baseInstance.
//
// Passes (one define): PASS_SHADOW, PASS_PREPASS, PASS_FORWARD,
// PASS_SELECTION, PASS_CAPTURE (probe capture: forward without TAA/velocity).
#include "frame.glsl"
#include "scene_data.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_tangent;
layout(location = 3) in float a_tangent_sign;
layout(location = 4) in vec2 a_uv;
layout(location = 5) in vec3 a_color;
layout(location = 6) in uint a_draw_id;

#if defined(PASS_SHADOW)
uniform mat4 u_pass_view_projection;
#endif

out VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
#if defined(PASS_PREPASS)
    vec4 current_clip;
    vec4 previous_clip;
#endif
} v_out;
flat out uint v_instance;
// Prepass and forward passes must produce bit-identical depth (depth-equal test).
invariant gl_Position;

layout(std430, binding = SSBO_VERTICES) readonly buffer VertexPool {
    float vertex_floats[];
};

vec3 previous_local_position(Instance inst) {
    if ((inst.flags & INSTANCE_SKINNED) != 0u) {
        int index = gl_VertexID - inst.base_vertex + inst.prev_base_vertex;
        int f = index * 15;
        return vec3(vertex_floats[f], vertex_floats[f + 1], vertex_floats[f + 2]);
    }
    return a_position;
}

void main() {
    Instance inst = instances[a_draw_id];
    v_instance = a_draw_id;
    vec4 world = inst.model * vec4(a_position, 1.0);
    mat3 nmat = instance_normal_matrix(inst);
    v_out.world_position = world.xyz;
    v_out.normal = normalize(nmat * a_normal);
    v_out.tangent = vec4(normalize(mat3(inst.model) * a_tangent), a_tangent_sign);
    v_out.uv = a_uv;
    v_out.color = a_color;

#if defined(PASS_SHADOW)
    gl_Position = u_pass_view_projection * world;
#elif defined(PASS_SELECTION)
    gl_Position = u_unjittered_view_projection * world; // drawn at output resolution, no jitter
#else
    gl_Position = u_view_projection * world;
#endif

#if defined(PASS_PREPASS)
    v_out.current_clip = u_unjittered_view_projection * world;
    vec4 prev_world = inst.prev_model * vec4(previous_local_position(inst), 1.0);
    if ((inst.flags & INSTANCE_NO_MOTION) != 0u) {
        prev_world = world;
    }
    v_out.previous_clip = u_prev_view_projection * prev_world;
#endif
}

// Probe captures: fills the background with the global environment
// (layer 0 of the probe array) where no geometry was drawn.
#include "ibl.glsl"

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec3 dir = normalize(world_from_depth(v_uv, 1.0) - u_camera_position.xyz);
    vec3 color = sample_environment(dir, 0.0, 0.0) * u_ambient.x;
    o_color = vec4(color * u_exposure.x, 1.0);
}

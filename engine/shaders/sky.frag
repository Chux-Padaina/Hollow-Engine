// Sky background on every pixel nothing else covered (depth = 0):
// atmosphere + sun disk, the HDRI, or a flat color.
#include "sky_common.glsl"
#include "fog.glsl"

in vec2 v_uv;
layout(binding = TEX_EXTRA) uniform samplerCube t_background;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 clip = vec4(v_uv * 2.0 - 1.0, 0.5, 1.0);
    vec4 world = u_inv_view_projection * clip;
    vec3 rd = normalize(world.xyz / world.w - u_camera_position.xyz);
    int mode = int(u_ambient.z);
    vec3 color;
    if (mode == 0) {
        color = atmosphere_radiance(rd) + sun_disk(rd);
    } else if (mode == 1) {
        float c = cos(-u_ambient.w);
        float s = sin(-u_ambient.w);
        vec3 r = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
        color = textureLod(t_background, r, 0.0).rgb * u_atmosphere1.w;
    } else {
        color = u_sky_color.rgb * u_sky_color.w;
    }
    color *= u_atmosphere1.z; // background intensity
    if (u_flags.x != DEBUG_FINAL) {
        color = vec3(0.0);
    }
    color *= u_exposure.x;
    color = apply_fog(color, u_camera_position.xyz + rd * u_fog1.w, v_uv, u_fog1.w).rgb;
    o_color = vec4(min(color, vec3(64000.0)), 1.0);
}

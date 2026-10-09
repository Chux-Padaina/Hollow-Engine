// Copies/resamples a texture. u_mode 1: sRGB-encoded source texture whose
// GL format already decodes it (output stays linear).
#include "common.glsl"

in vec2 v_uv;
layout(binding = 0) uniform sampler2D t_source;
uniform int u_mode;
uniform float u_lod;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = u_lod > 0.0 ? textureLod(t_source, v_uv, u_lod) : texture(t_source, v_uv);
    if (u_mode == 1) {
        o_color.rgb = linear_to_srgb(o_color.rgb); // stored sRGB-encoded in an RGBA8 array
    }
}

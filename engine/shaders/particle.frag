// vertex: particle.vert
// Particle shading: soft disc (or sprite), soft depth fade against the
// opaque scene. Additive particles are exposure-relative emitters; alpha
// particles are lit by the sky and sun.
#include "ibl.glsl"
#include "fog.glsl"

in vec4 v_color;
in vec2 v_uv;
in float v_view_z;
in vec3 v_world;

layout(binding = TEX_BASE_COLOR) uniform sampler2D t_sprite;
layout(binding = TEX_SCENE_DEPTH) uniform sampler2D t_scene_depth;

uniform int u_has_sprite;
uniform int u_additive;
uniform float u_emissive;
uniform float u_softness;

layout(location = 0) out vec4 o_color;

void main() {
    float shape;
    vec3 tint = vec3(1.0);
    if (u_has_sprite != 0) {
        vec4 sprite = texture(t_sprite, v_uv);
        shape = sprite.a;
        tint = sprite.rgb;
    } else {
        float r = length(v_uv * 2.0 - 1.0);
        shape = pow(saturate(1.0 - r), 1.5);
    }
    vec2 screen_uv = gl_FragCoord.xy * u_render_size.zw;
    float scene_z = linear_depth(textureLod(t_scene_depth, screen_uv, 0.0).r);
    float soft = saturate((scene_z - v_view_z) / max(u_softness, 1e-3));
    float alpha = saturate(v_color.a * shape * soft);
    if (alpha <= 0.002) {
        discard;
    }
    vec3 base = v_color.rgb * tint;
    if (u_additive != 0) {
        o_color = vec4(base * u_emissive * alpha, 0.0);
        return;
    }
    vec3 light = sky_diffuse(vec3(0.0, 1.0, 0.0)) * u_ambient.x + u_sun_illuminance.rgb * saturate(u_sun_direction.y + 0.2) * 0.25 * INV_PI;
    vec3 color = base * light * u_exposure.x + base * u_emissive;
    if (u_flags.x == DEBUG_FINAL) {
        color = apply_fog(color, v_world, screen_uv, v_view_z).rgb;
    }
    o_color = vec4(color * alpha, alpha);
}

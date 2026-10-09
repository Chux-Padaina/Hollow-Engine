// vertex: water.vert
// Water shading: screen-space refraction of the opaque scene with depth-
// based absorption and in-scattering, reflections from the environment
// (sky / probes), sun glints and shoreline + crest foam. Output is
// pre-exposed HDR (premultiplied, alpha 1).
#include "lighting.glsl"
#include "fog.glsl"
#include "water_waves.glsl"

in VertexData {
    vec3 world_position;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    vec3 color;
} v_in;
flat in uint v_instance;

layout(binding = TEX_SCENE_COLOR) uniform sampler2D t_scene_color;
layout(binding = TEX_SCENE_DEPTH) uniform sampler2D t_scene_depth;

layout(location = 0) out vec4 o_color;

float foam_pattern(vec2 p) {
    float t = u_time.y * 0.35;
    float a = sin(p.x * 3.1 + t) * sin(p.y * 2.7 - t * 1.3);
    float b = sin(p.x * 7.3 - p.y * 5.9 + t * 2.1) * 0.5;
    float c = sin(p.x * 13.7 + p.y * 11.3 - t * 3.0) * 0.25;
    return saturate((a + b + c) * 0.5 + 0.5);
}

void main() {
    Instance inst = instances[v_instance];
    Material m = materials[inst.material];
    vec3 shallow = m.base_color.rgb;
    vec3 deep = m.attenuation.rgb;
    float clarity = max(m.attenuation.w, 0.05);
    float foam_amount = m.transmission.y;
    float roughness = clamp(m.pbr.y, 0.02, 1.0);

    vec3 position = v_in.world_position;
    float distance_to_camera = length(u_camera_position.xyz - position);
    vec3 n = normalize(v_in.normal);
    n = water_ripples(position, n, m.anisotropy.y, saturate(1.0 - distance_to_camera / 80.0));
    if (!gl_FrontFacing) {
        n = -n;
    }

    Surface s = make_surface(position, n, normalize(v_in.normal) * (gl_FrontFacing ? 1.0 : -1.0), vec3(0.0), roughness, 0.0);
    s.f0 = vec3(0.02);
    s.ior = 1.333;

    vec2 pixel = gl_FragCoord.xy;
    vec2 screen_uv = pixel * u_render_size.zw;
    float view_z = dot(position - u_camera_position.xyz, u_camera_forward.xyz);
    LightingResult lit = evaluate_lighting(s, pixel, screen_uv, view_z, false, false);

    // Path length through the water to the opaque surface behind.
    float scene_z = linear_depth(textureLod(t_scene_depth, screen_uv, 0.0).r);
    float path_scale = distance_to_camera / max(view_z, 1e-3);
    float path = max(scene_z - view_z, 0.0) * path_scale;

    // Refraction: bend the lookup by the surface slope (less in shallows),
    // and fall back to the undistorted lookup if it lands in front of us.
    vec2 offset = (n.xz - s.geometric_normal.xz) * 0.06 * saturate(path * 0.5) / max(view_z * 0.1, 1.0);
    vec2 ruv = clamp(screen_uv + offset, vec2(0.0), vec2(1.0));
    float refracted_z = linear_depth(textureLod(t_scene_depth, ruv, 0.0).r);
    if (refracted_z < view_z) {
        ruv = screen_uv;
        refracted_z = scene_z;
    }
    float refracted_path = max(refracted_z - view_z, 0.0) * path_scale;
    float lod = saturate(refracted_path / (clarity * 4.0)) * 4.0;
    vec3 behind = textureLod(t_scene_color, ruv, lod).rgb * u_exposure.w;

    // Absorption tints the floor toward the shallow color; in-scattering
    // fills in the deep color as the water gets deeper.
    vec3 sigma = (vec3(1.0) - shallow) * 2.0 / clarity + vec3(0.25 / clarity);
    vec3 transmittance = exp(-sigma * refracted_path);
    float opacity = 1.0 - exp(-refracted_path / clarity);
    float sun_up = saturate(u_sun_direction.y);
    vec3 ambient = diffuse_environment(position, vec3(0.0, 1.0, 0.0));
    vec3 scatter = deep * (ambient + u_sun_illuminance.rgb * sun_up * INV_PI * 0.5);
    vec3 refracted = behind * transmittance + scatter * opacity;

    float F = F_Schlick(0.02, 1.0, s.NoV);
    vec3 color = refracted * (1.0 - F) + lit.direct + lit.indirect_specular;

    // Foam: along the shore (thin water) and on wave crests.
    float crest = saturate(v_in.color.x / max(m.anisotropy.x * 0.6, 1e-3) - 0.55) * v_in.color.y;
    float shore = saturate(1.0 - path / max(foam_amount * 0.8, 1e-3));
    float pattern = foam_pattern(position.xz * 1.3);
    float foam = saturate((shore * shore + crest) * foam_amount) * smoothstep(0.35, 0.75, pattern + shore * 0.3);
    if (foam > 0.0) {
        float NoL = saturate(dot(s.normal, u_sun_direction.xyz));
        vec3 foam_light = 0.85 * (diffuse_environment(position, s.normal) + u_sun_illuminance.rgb * NoL * INV_PI);
        color = mix(color, foam_light, foam);
    }

    // Soft intersection with the shore.
    float edge = saturate(path * 6.0);
    color = mix(behind, color, edge);

    color *= u_exposure.x;
    if (u_flags.x == DEBUG_FINAL) {
        color = apply_fog(color, position, screen_uv, view_z).rgb;
    } else if (u_flags.x == DEBUG_NORMALS) {
        color = n * 0.5 + 0.5;
    }
    o_color = vec4(color, 1.0);
}

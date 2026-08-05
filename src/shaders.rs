pub(crate) const OBJECT_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec4 a_color;
layout (location = 3) in vec2 a_tex_coord;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 v_world_position;
out vec3 v_normal;
out vec4 v_color;
out vec2 v_tex_coord;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_position = world.xyz;
    v_normal = normalize(mat3(transpose(inverse(u_model))) * a_normal);
    v_color = a_color;
    v_tex_coord = a_tex_coord;
    gl_Position = u_projection * u_view * world;
}
"#;

pub(crate) const OBJECT_FRAGMENT: &str = r#"#version 330 core
in vec3 v_world_position;
in vec3 v_normal;
in vec4 v_color;
in vec2 v_tex_coord;

uniform vec3 u_camera_position;
uniform vec3 u_directional_light_direction;
uniform vec3 u_directional_light_color;
uniform float u_directional_light_intensity;
uniform float u_selected;
uniform float u_has_texture;
uniform float u_metallic;
uniform float u_roughness;
uniform sampler2D u_base_texture;
uniform int u_debug_view;
uniform int u_local_light_count;
uniform vec3 u_local_light_positions[8];
uniform vec3 u_local_light_colors[8];
uniform vec3 u_local_light_directions[8];
uniform vec4 u_local_light_params[8];

out vec4 out_color;

pub(crate) const float PI = 3.14159265359;

float distributionGGX(vec3 normal, vec3 half_dir, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndh = max(dot(normal, half_dir), 0.0);
    float ndh2 = ndh * ndh;
    float denom = ndh2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float geometrySchlickGGX(float ndv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndv / max(ndv * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 normal, vec3 view_dir, vec3 light_dir, float roughness) {
    float ndv = max(dot(normal, view_dir), 0.0);
    float ndl = max(dot(normal, light_dir), 0.0);
    return geometrySchlickGGX(ndv, roughness) * geometrySchlickGGX(ndl, roughness);
}

vec3 fresnelSchlick(float cos_theta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

vec3 pbrLight(
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 normal,
    vec3 view_dir,
    vec3 light_dir,
    vec3 radiance
) {
    vec3 half_dir = normalize(view_dir + light_dir);
    float ndl = max(dot(normal, light_dir), 0.0);
    float ndv = max(dot(normal, view_dir), 0.0);
    if (ndl <= 0.0 || ndv <= 0.0) {
        return vec3(0.0);
    }

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float d = distributionGGX(normal, half_dir, roughness);
    float g = geometrySmith(normal, view_dir, light_dir, roughness);
    vec3 f = fresnelSchlick(max(dot(half_dir, view_dir), 0.0), f0);

    vec3 numerator = d * g * f;
    float denominator = max(4.0 * ndv * ndl, 0.0001);
    vec3 specular = numerator / denominator;

    vec3 ks = f;
    vec3 kd = (1.0 - ks) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * radiance * ndl;
}

void main() {
    vec3 normal = normalize(v_normal);
    vec3 view_dir = normalize(u_camera_position - v_world_position);
    vec3 key_dir = normalize(-u_directional_light_direction);
    vec3 sampled = u_has_texture > 0.5 ? texture(u_base_texture, v_tex_coord).rgb : vec3(1.0);
    vec3 albedo = pow(clamp(v_color.rgb * sampled, 0.0, 1.0), vec3(2.2));
    float metallic = clamp(u_metallic, 0.0, 1.0);
    float roughness = clamp(u_roughness, 0.04, 1.0);

    vec3 direct_light = pbrLight(
        albedo,
        metallic,
        roughness,
        normal,
        view_dir,
        key_dir,
        u_directional_light_color * max(u_directional_light_intensity, 0.0) * 3.2
    );
    for (int i = 0; i < 8; ++i) {
        if (i >= u_local_light_count) {
            break;
        }
        vec3 to_light = u_local_light_positions[i] - v_world_position;
        float distance_to_light = length(to_light);
        vec3 light_dir = to_light / max(distance_to_light, 0.001);
        vec4 params = u_local_light_params[i];
        float attenuation = 1.0 - smoothstep(params.x * 0.18, params.x, distance_to_light);
        float cone = 1.0;
        if (params.w > -1.5) {
            float spot_dot = dot(normalize(u_local_light_directions[i]), -light_dir);
            cone = smoothstep(params.w, params.z, spot_dot);
        }
        float visibility = attenuation * cone * params.y;
        direct_light += pbrLight(
            albedo,
            metallic,
            roughness,
            normal,
            view_dir,
            light_dir,
            u_local_light_colors[i] * visibility * 14.0
        );
    }

    vec3 ambient = albedo * vec3(0.03) * (1.0 - metallic);
    vec3 color = ambient + direct_light;
    color += vec3(1.0, 0.82, 0.18) * u_selected * 0.08;

    if (u_debug_view == 1) {
        out_color = vec4(pow(clamp(albedo, 0.0, 1.0), vec3(1.0 / 2.2)), v_color.a);
        return;
    }
    if (u_debug_view == 2) {
        out_color = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }
    if (u_debug_view == 3) {
        vec3 lighting_only = clamp(ambient + direct_light, 0.0, 8.0);
        out_color = vec4(pow(lighting_only / (lighting_only + vec3(1.0)), vec3(1.0 / 2.2)), 1.0);
        return;
    }
    if (u_debug_view == 4) {
        out_color = vec4(vec3(roughness), 1.0);
        return;
    }
    if (u_debug_view == 5) {
        out_color = vec4(vec3(metallic), 1.0);
        return;
    }

    float camera_distance = length(u_camera_position - v_world_position);
    vec3 fog_color = vec3(0.10, 0.11, 0.10);
    color = mix(color, fog_color, smoothstep(18.0, 38.0, camera_distance) * 0.25);
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    out_color = vec4(color, v_color.a);
}
"#;

pub(crate) const OUTLINE_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 1) in vec3 a_normal;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform float u_outline_width;
uniform vec2 u_viewport;

out float v_world_y;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_y = world.y;
    vec3 view_normal = normalize(mat3(u_view * u_model) * a_normal);
    vec4 clip = u_projection * u_view * world;
    vec2 dir = normalize(view_normal.xy + vec2(0.0001, 0.0001));
    vec2 pixel = vec2(2.0 / u_viewport.x, 2.0 / u_viewport.y);
    clip.xy += dir * pixel * u_outline_width * clip.w;
    gl_Position = clip;
}
"#;

pub(crate) const OUTLINE_FRAGMENT: &str = r#"#version 330 core
in float v_world_y;
uniform vec3 u_outline_color;
uniform float u_ground_clip_y;
out vec4 out_color;

void main() {
    if (v_world_y < u_ground_clip_y) {
        discard;
    }
    out_color = vec4(u_outline_color, 1.0);
}
"#;

pub(crate) const UNLIT_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

void main() {
    gl_Position = u_projection * u_view * u_model * vec4(a_position, 1.0);
}
"#;

pub(crate) const UNLIT_FRAGMENT: &str = r#"#version 330 core
uniform vec3 u_color;
out vec4 out_color;

void main() {
    vec3 color = u_color / (u_color + vec3(1.0));
    out_color = vec4(pow(color, vec3(0.72)), 1.0);
}
"#;

pub(crate) const EDITOR_GRID_VERTEX: &str = r#"#version 330 core
layout (location = 0) in vec3 a_position;
layout (location = 2) in vec4 a_color;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform float u_time;

out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_projection * u_view * u_model * vec4(a_position, 1.0);
}
"#;

pub(crate) const EDITOR_GRID_FRAGMENT: &str = r#"#version 330 core
in vec4 v_color;
out vec4 out_color;

void main() {
    vec3 color = v_color.rgb;
    float glow = max(max(color.r, color.g), color.b);
    color += color * smoothstep(0.55, 1.0, glow) * 0.35;
    out_color = vec4(color, v_color.a);
}
"#;

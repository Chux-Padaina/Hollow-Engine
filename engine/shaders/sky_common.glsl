// Sky radiance lookups shared by the sky background and environment capture.
#pragma once
#include "atmosphere.glsl"

layout(binding = TEX_SKY_VIEW) uniform sampler2D t_sky_view;

// Atmosphere radiance (nits) along `rd` from the camera, without the sun disk.
vec3 atmosphere_radiance(vec3 rd) {
    Atmosphere a = get_atmosphere();
    vec3 ro = atmosphere_position(u_camera_position.xyz);
    float r = length(ro);
    float horizon = acos(clamp(sqrt(max(r * r - a.bottom * a.bottom, 0.0)) / r, -1.0, 1.0)) - 0.5 * PI; // elevation of the horizon (<= 0)
    float lat = asin(clamp(rd.y, -1.0, 1.0));
    float v;
    if (lat >= horizon) {
        float c = sqrt(saturate((lat - horizon) / (0.5 * PI - horizon)));
        v = 0.5 * (1.0 - c);
    } else {
        float c = sqrt(saturate((horizon - lat) / (horizon + 0.5 * PI)));
        v = 0.5 * (1.0 + c);
    }
    vec3 sun = normalize(u_sun_direction.xyz);
    vec3 sun_flat = normalize(vec3(sun.x, 0.0, sun.z) + vec3(1e-5, 0.0, 0.0));
    vec3 side = cross(vec3(0.0, 1.0, 0.0), sun_flat);
    vec2 flat_dir = vec2(dot(rd, sun_flat), dot(rd, side));
    float lon = atan(flat_dir.y, flat_dir.x);
    float u = fract(lon / TWO_PI + 1.0);
    vec3 lum = textureLod(t_sky_view, vec2(u, v), 0.0).rgb;
    return lum * u_sun_illuminance.rgb;
}

// Simple lit ground for directions below the horizon (environment capture).
vec3 ground_radiance(vec3 rd) {
    Atmosphere a = get_atmosphere();
    vec3 sun = normalize(u_sun_direction.xyz);
    vec3 sun_t = transmittance_to_top(a, a.bottom + 0.2, max(sun.y, 0.0));
    vec3 sky = atmosphere_radiance(vec3(0.0, 1.0, 0.0));
    return a.ground_albedo / PI * (u_sun_illuminance.rgb * sun_t * max(sun.y, 0.0) + sky * PI * 0.5);
}

vec3 sun_disk(vec3 rd) {
    vec3 sun = normalize(u_sun_direction.xyz);
    float angular_radius = max(u_sun_illuminance.w, 0.0047);
    float cos_angle = dot(rd, sun);
    float edge = cos(angular_radius);
    if (cos_angle < edge || u_atmosphere1.y < 0.5) {
        return vec3(0.0);
    }
    Atmosphere a = get_atmosphere();
    vec3 ro = atmosphere_position(u_camera_position.xyz);
    vec3 t = transmittance_to_top(a, length(ro), sun.y);
    if (ray_sphere(ro, rd, a.bottom) > 0.0) {
        return vec3(0.0);
    }
    float solid_angle = TWO_PI * (1.0 - edge);
    // Limb darkening.
    float x = saturate((cos_angle - edge) / (1.0 - edge));
    float limb = 0.4 + 0.6 * sqrt(x);
    return u_sun_illuminance.rgb / solid_angle * t * limb;
}

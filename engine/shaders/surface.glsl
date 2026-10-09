// The shading inputs of a pixel, gathered from the material and textures.
#pragma once
#include "frame.glsl"
#include "scene_data.glsl"

struct Surface {
    vec3 position;
    vec3 normal;       // shading normal
    vec3 geometric_normal;
    vec3 tangent;
    vec3 bitangent;
    vec3 view;         // toward the camera
    float NoV;
    vec3 albedo;       // diffuse color (base color * (1 - metallic))
    vec3 f0;
    float roughness;   // perceptual
    float alpha;       // roughness^2
    float metallic;
    float occlusion;
    vec3 emissive;
    float opacity;
    vec3 base_color;
    // Extensions
    float clearcoat;
    float clearcoat_roughness;
    vec3 clearcoat_normal;
    vec3 sheen_color;
    float sheen_roughness;
    float transmission;
    float thickness;
    float ior;
    vec3 attenuation_color;
    float attenuation_distance;
    float anisotropy;
    vec3 anisotropic_t;
    vec3 anisotropic_b;
    float subsurface;
    vec3 subsurface_color;
    uint flags;
    vec3 energy_compensation;
    vec2 dfg;
};

// A plain dielectric/metal surface with every extension off, for shaders
// that build their surface procedurally (water, grass).
Surface make_surface(vec3 position, vec3 n, vec3 geometric_normal, vec3 base_color, float roughness, float metallic) {
    Surface s;
    s.flags = 0u;
    s.position = position;
    s.view = normalize(u_camera_position.xyz - position);
    s.normal = n;
    s.geometric_normal = geometric_normal;
    basis(n, s.tangent, s.bitangent);
    s.NoV = max(dot(n, s.view), 1e-4);
    s.base_color = base_color;
    s.metallic = metallic;
    s.albedo = base_color * (1.0 - metallic);
    s.ior = 1.5;
    s.f0 = mix(vec3(0.04), base_color, metallic);
    s.roughness = roughness;
    s.alpha = roughness * roughness;
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    s.opacity = 1.0;
    s.clearcoat = 0.0;
    s.clearcoat_roughness = 0.0;
    s.clearcoat_normal = n;
    s.sheen_color = vec3(0.0);
    s.sheen_roughness = 0.0;
    s.transmission = 0.0;
    s.thickness = 0.0;
    s.attenuation_color = vec3(1.0);
    s.attenuation_distance = 0.0;
    s.anisotropy = 0.0;
    s.anisotropic_t = s.tangent;
    s.anisotropic_b = s.bitangent;
    s.subsurface = 0.0;
    s.subsurface_color = vec3(0.0);
    s.energy_compensation = vec3(1.0);
    s.dfg = vec2(1.0, 0.0);
    return s;
}

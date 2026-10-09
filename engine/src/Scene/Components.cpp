// Component metadata (names, scene-file keys, factory), per-component
// property descriptions, light defaults, and the terrain height function.
#include "Hollow/Scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Hollow {

namespace {

struct ComponentMeta {
    ComponentType type;
    const char* name;
    const char* key;
};

constexpr std::array<ComponentMeta, static_cast<std::size_t>(ComponentType::Count)> kComponents{{
    {ComponentType::StaticMesh, "Static Mesh", "StaticMesh"},
    {ComponentType::Light, "Light", "Light"},
    {ComponentType::Camera, "Camera", "Camera"},
    {ComponentType::RigidBody, "Rigid Body", "RigidBody"},
    {ComponentType::Collider, "Collider", "Collider"},
    {ComponentType::Script, "Script", "Script"},
    {ComponentType::ReflectionProbe, "Reflection Probe", "ReflectionProbe"},
    {ComponentType::IrradianceVolume, "Irradiance Volume", "IrradianceVolume"},
    {ComponentType::Decal, "Decal", "Decal"},
    {ComponentType::ParticleEmitter, "Particle Emitter", "ParticleEmitter"},
    {ComponentType::Terrain, "Terrain", "Terrain"},
    {ComponentType::Water, "Water", "Water"},
    {ComponentType::Animator, "Animator", "Animator"},
    {ComponentType::CharacterController, "Character Controller", "CharacterController"},
}};

constexpr std::array<const char*, 3> kLightKinds{"Directional", "Point", "Spot"};
constexpr std::array<const char*, 4> kColliderShapes{"box", "sphere", "capsule", "mesh"};
constexpr std::array<const char*, 4> kEmitterShapes{"Point", "Sphere", "Box", "Cone"};
constexpr std::array<const char*, 2> kBlendModes{"Additive", "Alpha"};
constexpr std::array<const char*, 3> kProbeResolutions{"64", "128", "256"};

} // namespace

const char* component_type_name(ComponentType type) {
    const auto index = static_cast<std::size_t>(type);
    return index < kComponents.size() ? kComponents[index].name : "Component";
}

const char* component_type_key(ComponentType type) {
    const auto index = static_cast<std::size_t>(type);
    return index < kComponents.size() ? kComponents[index].key : "Unknown";
}

bool component_type_from_key(const std::string& key, ComponentType& type) {
    for (const ComponentMeta& meta : kComponents) {
        if (key == meta.key) {
            type = meta.type;
            return true;
        }
    }
    return false;
}

bool component_allows_multiple(ComponentType type) {
    return type == ComponentType::Script;
}

std::unique_ptr<Component> create_component(ComponentType type) {
    switch (type) {
    case ComponentType::Light: return std::make_unique<LightComponent>();
    case ComponentType::Camera: return std::make_unique<CameraComponent>();
    case ComponentType::RigidBody: return std::make_unique<RigidBodyComponent>();
    case ComponentType::Collider: return std::make_unique<ColliderComponent>();
    case ComponentType::ReflectionProbe: return std::make_unique<ReflectionProbeComponent>();
    case ComponentType::IrradianceVolume: return std::make_unique<IrradianceVolumeComponent>();
    case ComponentType::Decal: return std::make_unique<DecalComponent>();
    case ComponentType::ParticleEmitter: return std::make_unique<ParticleEmitterComponent>();
    case ComponentType::Terrain: return std::make_unique<TerrainComponent>();
    case ComponentType::Water: return std::make_unique<WaterComponent>();
    case ComponentType::Animator: return std::make_unique<AnimatorComponent>();
    case ComponentType::CharacterController: return std::make_unique<CharacterControllerComponent>();
    case ComponentType::StaticMesh: return std::make_unique<StaticMeshComponent>();
    case ComponentType::Script:
    case ComponentType::Count: break;
    }
    return nullptr;
}

// --- Lights ------------------------------------------------------------------------

const char* light_kind_name(LightKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < kLightKinds.size() ? kLightKinds[index] : "Light";
}

void LightComponent::apply_defaults_for_kind() {
    switch (light_kind) {
    case LightKind::Directional:
        color = {1.0f, 0.97f, 0.92f};
        intensity = 90000.0f;
        source_size = 0.53f;
        break;
    case LightKind::Point:
        color = {0.55f, 0.8f, 1.0f};
        intensity = 1600.0f;
        range = 8.0f;
        source_size = 0.05f;
        break;
    case LightKind::Spot:
        color = {1.0f, 0.75f, 0.45f};
        intensity = 2400.0f;
        range = 10.0f;
        spot_angle_radians = 40.0f * kDegToRad;
        source_size = 0.05f;
        break;
    }
}

void LightComponent::describe(PropertyVisitor& v) {
    v.enumeration("kind", "Type", light_kind, kLightKinds);
    v.boolean("enabled", "Enabled", enabled);
    v.boolean("cast_shadows", "Cast shadows", cast_shadows);
    v.color("color", "Color", color);
    v.boolean("use_temperature", "Use temperature", use_temperature);
    if (v.show(use_temperature)) {
        v.number("temperature", "Temperature", temperature, {1000.0f, 20000.0f, 10.0f, "%.0f K"});
    }
    if (v.show(light_kind == LightKind::Directional)) {
        v.number("intensity_lux", "Illuminance", intensity, {0.0f, 150000.0f, 50.0f, "%.0f lx", true, true}, "Sun ~100,000 lx; overcast ~10,000; moonlight ~0.3.");
        v.number("angular_diameter", "Angular size", source_size, {0.0f, 10.0f, 0.01f, "%.2f deg"}, "The sun is 0.53 degrees. Larger = softer shadows.");
    }
    if (v.show(light_kind != LightKind::Directional)) {
        v.number("intensity_lumens", "Power", intensity, {0.0f, 100000.0f, 5.0f, "%.0f lm", true, true}, "60 W bulb ~800 lm; LED panel ~3,000 lm.");
        v.number("range", "Range", range, {0.1f, 100.0f, 0.05f, "%.2f m"});
        v.number("source_radius", "Source radius", source_size, {0.0f, 2.0f, 0.005f, "%.3f m"}, "Physical bulb size; larger = softer shadows.");
    }
    if (v.show(light_kind == LightKind::Spot)) {
        v.angle("spot_angle_degrees", "Cone angle", spot_angle_radians, 2.0f, 170.0f);
        v.number("spot_softness", "Cone softness", spot_softness, {0.0f, 1.0f, 0.01f, "%.2f"});
    }
    v.number("volumetric", "Volumetric", volumetric, {0.0f, 10.0f, 0.01f, "%.2f"}, "How strongly the light scatters in volumetric fog.");
}

void CameraComponent::describe(PropertyVisitor& v) {
    v.angle("fov_degrees", "Field of view", fov_y_radians, 10.0f, 150.0f);
    v.number("near", "Near plane", near_plane, {0.01f, 5.0f, 0.005f, "%.3f m", true, true});
    v.boolean("primary", "Primary (Play mode)", primary);
}

void ReflectionProbeComponent::describe(PropertyVisitor& v) {
    v.vec3("extents", "Box half size", extents, {0.1f, 500.0f, 0.05f, "%.2f", false});
    v.number("blend_distance", "Blend distance", blend_distance, {0.0f, 10.0f, 0.01f, "%.2f m"});
    v.number("intensity", "Intensity", intensity, {0.0f, 4.0f, 0.01f, "%.2f"});
    v.boolean("box_projection", "Box projection", box_projection, "Parallax-correct reflections for rooms.");
    int choice = resolution <= 64 ? 0 : resolution <= 128 ? 1 : 2;
    v.choice("resolution", "Resolution", choice, kProbeResolutions);
    resolution = choice == 0 ? 64 : choice == 1 ? 128 : 256;
}

void IrradianceVolumeComponent::describe(PropertyVisitor& v) {
    v.vec3("extents", "Box half size", extents, {0.5f, 500.0f, 0.05f, "%.2f", false});
    v.integer("probes_x", "Probes X", probes_x, 2, 32);
    v.integer("probes_y", "Probes Y", probes_y, 2, 32);
    v.integer("probes_z", "Probes Z", probes_z, 2, 32);
    v.number("blend_distance", "Blend distance", blend_distance, {0.0f, 10.0f, 0.01f, "%.2f m"});
    v.number("intensity", "Intensity", intensity, {0.0f, 4.0f, 0.01f, "%.2f"});
    v.number("normal_bias", "Normal bias", normal_bias, {0.0f, 2.0f, 0.01f, "%.2f m"}, "Samples slightly off the surface to reduce leaking.");
    v.integer("bounces", "Bounces", bounces, 1, 4);
}

void DecalComponent::describe(PropertyVisitor& v) {
    v.path("albedo", "Albedo texture", albedo_texture_path, "Images");
    v.path("normal", "Normal texture", normal_texture_path, "Images");
    v.color("color", "Tint", color);
    v.number("opacity", "Opacity", opacity, {0.0f, 1.0f, 0.01f, "%.2f"});
    if (v.show(!normal_texture_path.empty())) {
        v.number("normal_strength", "Normal strength", normal_strength, {0.0f, 3.0f, 0.01f, "%.2f"});
    }
    v.boolean("override_roughness", "Override roughness", override_roughness);
    if (v.show(override_roughness)) {
        v.number("roughness", "Roughness", roughness, {0.02f, 1.0f, 0.01f, "%.2f"});
    }
    v.number("angle_fade", "Angle fade", angle_fade, {-1.0f, 0.99f, 0.01f, "%.2f"});
}

void ParticleEmitterComponent::describe(PropertyVisitor& v) {
    v.boolean("emitting", "Emitting", emitting);
    v.integer("max_particles", "Max particles", max_particles, 16, 262144);
    v.number("rate", "Rate", rate, {0.0f, 20000.0f, 1.0f, "%.0f /s", true, true});
    v.number("lifetime", "Lifetime", lifetime, {0.05f, 30.0f, 0.01f, "%.2f s"});
    v.number("lifetime_variance", "Lifetime variance", lifetime_variance, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.enumeration("shape", "Shape", shape, kEmitterShapes);
    if (v.show(shape != EmitterShape::Point)) {
        v.vec3("shape_size", "Shape size", shape_size, {0.0f, 100.0f, 0.01f, "%.2f", false});
    }
    if (v.show(shape == EmitterShape::Cone)) {
        v.angle("cone_angle_degrees", "Cone angle", cone_angle, 0.0f, 180.0f);
    }
    v.number("speed", "Speed", speed, {0.0f, 50.0f, 0.01f, "%.2f m/s"});
    v.number("speed_variance", "Speed variance", speed_variance, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("gravity", "Gravity", gravity, {-5.0f, 5.0f, 0.01f, "%.2f"});
    v.number("drag", "Drag", drag, {0.0f, 10.0f, 0.01f, "%.2f"});
    v.number("noise", "Turbulence", noise, {0.0f, 10.0f, 0.01f, "%.2f"});
    v.number("size_start", "Size start", size_start, {0.0f, 20.0f, 0.005f, "%.3f m"});
    v.number("size_end", "Size end", size_end, {0.0f, 20.0f, 0.005f, "%.3f m"});
    v.color("color_start", "Color start", color_start, true);
    v.color("color_end", "Color end", color_end, true);
    v.number("alpha_start", "Alpha start", alpha_start, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("alpha_end", "Alpha end", alpha_end, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("emissive", "Brightness", emissive, {0.0f, 100.0f, 0.05f, "%.2f", true, true});
    v.enumeration("blend", "Blend", blend, kBlendModes);
    v.boolean("world_space", "World space", world_space, "Particles stay behind when the emitter moves.");
    v.path("texture", "Sprite", texture_path, "Images");
}

void TerrainComponent::describe(PropertyVisitor& v) {
    v.number("size", "Size", size, {8.0f, 2048.0f, 1.0f, "%.0f m", true, true});
    v.integer("resolution", "Resolution", resolution, 33, 1025);
    v.number("height", "Height", height, {0.0f, 300.0f, 0.1f, "%.1f m"});
    v.integer("seed", "Seed", seed, 0, 100000);
    v.number("frequency", "Frequency", frequency, {0.1f, 16.0f, 0.01f, "%.2f"});
    v.integer("octaves", "Octaves", octaves, 1, 10);
    v.number("ridges", "Ridges", ridges, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("flatten", "Flatten", flatten, {0.0f, 1.0f, 0.01f, "%.2f"});
    if (PropertyGroup g{v, nullptr, "Basin"}) {
        v.number("basin_depth", "Depth", basin_depth, {0.0f, 100.0f, 0.05f, "%.2f m"});
        v.number("basin_radius", "Radius", basin_radius, {1.0f, 1000.0f, 0.1f, "%.1f m"});
        v.number("basin_x", "Center X", basin_x, {-1000.0f, 1000.0f, 0.1f, "%.1f m"});
        v.number("basin_z", "Center Z", basin_z, {-1000.0f, 1000.0f, 0.1f, "%.1f m"});
    }
    if (PropertyGroup g{v, nullptr, "Layers"}) {
        v.color("grass_color", "Grass", grass_color);
        v.color("rock_color", "Rock", rock_color);
        v.color("high_color", "High ground", high_color);
        v.color("low_color", "Low ground", low_color);
    }
    if (PropertyGroup g{v, nullptr, "Grass"}) {
        v.number("grass_density", "Density", grass_density, {0.0f, 40.0f, 0.1f, "%.1f /m2"});
        v.number("grass_height", "Height", grass_height, {0.05f, 2.0f, 0.01f, "%.2f m"});
        v.number("grass_distance", "Draw distance", grass_distance, {5.0f, 200.0f, 1.0f, "%.0f m"});
        v.number("grass_min_height", "Min height", grass_min_height, {-100.0f, 500.0f, 0.05f, "%.2f m"});
        v.number("wind", "Wind", wind, {0.0f, 3.0f, 0.01f, "%.2f"});
    }
}

void WaterComponent::describe(PropertyVisitor& v) {
    v.number("size_x", "Size X", size_x, {1.0f, 2000.0f, 0.5f, "%.1f m", true, true});
    v.number("size_z", "Size Z", size_z, {1.0f, 2000.0f, 0.5f, "%.1f m", true, true});
    v.number("wave_height", "Wave height", wave_height, {0.0f, 4.0f, 0.005f, "%.3f m"});
    v.number("wavelength", "Wavelength", wavelength, {0.5f, 100.0f, 0.05f, "%.2f m", true, true});
    v.number("steepness", "Steepness", steepness, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("wave_speed", "Speed", wave_speed, {0.0f, 5.0f, 0.01f, "%.2f"});
    v.angle("wind_direction_degrees", "Wind direction", wind_direction, -180.0f, 180.0f);
    v.color("shallow_color", "Shallow color", shallow_color);
    v.color("deep_color", "Deep color", deep_color);
    v.number("clarity", "Clarity", clarity, {0.05f, 50.0f, 0.05f, "%.2f m", true, true});
    v.number("foam", "Foam", foam, {0.0f, 2.0f, 0.01f, "%.2f"});
    v.number("roughness", "Roughness", roughness, {0.0f, 0.5f, 0.005f, "%.3f"});
}

void AnimatorComponent::describe(PropertyVisitor& v) {
    v.text("clip", "Clip", clip, "Animation clip name (empty = the first clip).");
    v.number("speed", "Speed", speed, {-4.0f, 4.0f, 0.01f, "%.2f"});
    v.boolean("loop", "Loop", loop);
    v.boolean("playing", "Playing", playing);
    v.boolean("preview_in_editor", "Preview in editor", preview_in_editor);
    v.number("crossfade", "Crossfade", crossfade, {0.0f, 2.0f, 0.01f, "%.2f s"});
}

void RigidBodyComponent::describe(PropertyVisitor& v) {
    v.number("mass", "Mass", mass, {0.01f, 10000.0f, 0.05f, "%.2f kg", true, true});
    v.boolean("use_gravity", "Gravity", use_gravity);
    v.boolean("kinematic", "Kinematic", is_kinematic, "Follows the actor transform (scripts, animation); pushes dynamic bodies but isn't moved by them.");
    v.number("restitution", "Bounciness", restitution, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("friction", "Friction", friction, {0.0f, 1.5f, 0.01f, "%.2f"});
    v.number("linear_damping", "Damping", linear_damping, {0.0f, 5.0f, 0.01f, "%.2f"});
    v.number("angular_damping", "Angular damping", angular_damping, {0.0f, 5.0f, 0.01f, "%.2f"});
    v.boolean("freeze_rotation", "Lock rotation", freeze_rotation, "Keep the body upright (no rotation from collisions).");
    v.boolean("continuous", "Continuous collision", continuous, "Swept collision so fast bodies don't tunnel through thin walls.");
}

void ColliderComponent::describe(PropertyVisitor& v) {
    v.enumeration("shape", "Shape", shape, kColliderShapes);
    v.vec3("center", "Center", center, {0.0f, 0.0f, 0.02f, "%.2f", false});
    if (v.show(shape == ColliderShape::Box)) {
        v.vec3("half_extents", "Half size", half_extents, {0.001f, 1000.0f, 0.02f, "%.2f", false});
    }
    if (v.show(shape == ColliderShape::Sphere || shape == ColliderShape::Capsule)) {
        v.number("radius", "Radius", radius, {0.001f, 1000.0f, 0.02f, "%.2f", false});
    }
    if (v.show(shape == ColliderShape::Capsule)) {
        v.number("height", "Height", height, {0.0f, 1000.0f, 0.02f, "%.2f", false}, "Length of the straight part (total = height + 2 x radius).");
    }
    if (v.show(shape == ColliderShape::Mesh)) {
        v.note("Uses the actor's mesh. Dynamic bodies get its convex hull.");
    }
    v.boolean("trigger", "Trigger", is_trigger, "Reports overlaps to scripts without pushing.");
    v.integer("layer", "Layer", layer, 0, 7, "Collision layer (see the physics settings for which layers collide).");
}

void CharacterControllerComponent::describe(PropertyVisitor& v) {
    v.number("radius", "Radius", radius, {0.05f, 5.0f, 0.01f, "%.2f m"});
    v.number("height", "Height", height, {0.0f, 10.0f, 0.01f, "%.2f m"}, "Cylinder part; total height = height + 2 x radius.");
    v.vec3("center", "Center", center, {0.0f, 0.0f, 0.02f, "%.2f", false});
    v.angle("max_slope_degrees", "Max slope", max_slope, 0.0f, 89.0f);
    v.number("step_height", "Step height", step_height, {0.0f, 2.0f, 0.01f, "%.2f m"});
    v.number("push_strength", "Push strength", push_strength, {0.0f, 20.0f, 0.05f, "%.2f"});
    v.number("gravity_scale", "Gravity scale", gravity_scale, {0.0f, 5.0f, 0.01f, "%.2f"});
    v.integer("layer", "Layer", layer, 0, 7);
}

// --- Terrain height ------------------------------------------------------------------

namespace {

float hash2(int x, int y, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + static_cast<std::uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}

// Gradient noise in [-1, 1].
float gradient_noise(float x, float y, int seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float xf = x - static_cast<float>(xi);
    const float yf = y - static_cast<float>(yi);
    auto grad = [seed](int gx, int gy, float dx, float dy) {
        const float angle = hash2(gx, gy, seed) * 6.2831853f;
        return std::cos(angle) * dx + std::sin(angle) * dy;
    };
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(xf);
    const float v = fade(yf);
    const float n00 = grad(xi, yi, xf, yf);
    const float n10 = grad(xi + 1, yi, xf - 1.0f, yf);
    const float n01 = grad(xi, yi + 1, xf, yf - 1.0f);
    const float n11 = grad(xi + 1, yi + 1, xf - 1.0f, yf - 1.0f);
    const float nx0 = n00 + (n10 - n00) * u;
    const float nx1 = n01 + (n11 - n01) * u;
    return (nx0 + (nx1 - nx0) * v) * 1.41421356f;
}

} // namespace

float TerrainComponent::sample_height(float x, float z) const {
    // Normalized coordinates over the terrain width.
    const float s = std::max(size, 1.0f);
    float px = (x / s + 0.5f) * frequency;
    float pz = (z / s + 0.5f) * frequency;
    float amplitude = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int octave = 0; octave < std::clamp(octaves, 1, 10); ++octave) {
        const float n = gradient_noise(px, pz, seed + octave * 131);
        const float ridge = 1.0f - std::abs(n);
        const float value = n * 0.5f + 0.5f;
        sum += amplitude * (value * (1.0f - ridges) + ridge * ridge * ridges);
        norm += amplitude;
        amplitude *= 0.5f;
        px = px * 2.03f + 17.1f;
        pz = pz * 2.03f - 9.7f;
    }
    float h = sum / std::max(norm, 1e-4f); // 0..1
    // Flatten valleys: smooth curve that keeps peaks.
    h = std::pow(std::clamp(h, 0.0f, 1.0f), 1.0f + flatten * 2.0f);
    // Fade toward the edges so the terrain sits in the world nicely.
    const float ex = std::abs(x / s) * 2.0f;
    const float ez = std::abs(z / s) * 2.0f;
    const float edge = std::clamp((1.0f - std::max(ex, ez)) * 6.0f, 0.0f, 1.0f);
    float result = h * height * (0.3f + 0.7f * edge) - 0.5f;
    if (basin_depth > 0.0f) {
        const float d = std::sqrt((x - basin_x) * (x - basin_x) + (z - basin_z) * (z - basin_z)) / std::max(basin_radius, 0.01f);
        const float t = std::clamp(1.0f - d, 0.0f, 1.0f);
        result -= basin_depth * t * t * (3.0f - 2.0f * t);
    }
    return result;
}

} // namespace Hollow

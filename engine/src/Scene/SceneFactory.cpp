// Implementation of SceneFactory.hpp.
#include "Hollow/Scene/SceneFactory.hpp"

#include "Hollow/Scripting/Script.hpp"

#include <algorithm>

namespace Hollow {

Actor& spawn_primitive(Scene& scene, AssetManager& assets, BuiltinMesh mesh, const std::string& name, const Transform& transform) {
    Actor& actor = scene.create_actor(name);
    actor.transform = transform;
    auto& component = actor.add_component<StaticMeshComponent>();
    MeshPart part;
    part.name = name;
    part.mesh = assets.builtin_mesh(mesh);
    if (mesh == BuiltinMesh::Plane) {
        part.material.base_color = {0.46f, 0.48f, 0.46f};
        part.material.roughness = 0.86f;
    } else if (mesh == BuiltinMesh::Sphere) {
        part.material.base_color = {0.58f, 0.68f, 0.84f};
        part.material.roughness = 0.48f;
    }
    component.parts.push_back(std::move(part));
    return actor;
}

Actor& spawn_light(Scene& scene, LightKind kind, const std::string& name, const Transform& transform) {
    Actor& actor = scene.create_actor(name);
    actor.transform = transform;
    auto& light = actor.add_component<LightComponent>();
    light.light_kind = kind;
    light.apply_defaults_for_kind();
    return actor;
}

Actor& spawn_camera(Scene& scene, const std::string& name, const Transform& transform) {
    Actor& actor = scene.create_actor(name);
    actor.transform = transform;
    actor.add_component<CameraComponent>();
    return actor;
}

Actor& spawn_model(Scene& scene, const ModelHandle& model, const std::string& name, const Transform& transform) {
    Actor& actor = scene.create_actor(name);
    actor.transform = transform;
    auto& mesh = actor.add_component<StaticMeshComponent>();
    bool skinned = false;
    for (const ModelPart& part : model->parts) {
        mesh.parts.push_back({part.name, part.mesh, part.material});
        skinned = skinned || (part.mesh && part.mesh->skinned());
    }
    if (skinned) {
        actor.add_component<AnimatorComponent>();
    }
    return actor;
}

void fit_collider_to_mesh(const Actor& actor, ColliderComponent& collider) {
    const auto* mesh = actor.find_component<StaticMeshComponent>();
    if (mesh == nullptr || mesh->parts.empty()) {
        collider.center = {};
        collider.half_extents = {0.5f, 0.5f, 0.5f};
        collider.radius = 0.5f;
        return;
    }
    const Aabb bounds = mesh->local_bounds();
    collider.center = bounds.center();
    // Flat meshes (planes) still need some thickness to collide reliably.
    collider.half_extents = component_max(bounds.extents(), {0.01f, 0.01f, 0.01f});
    collider.radius = std::max(max_component(bounds.extents()), 0.01f);
}

namespace {
Material& material_of(Actor& actor) {
    return actor.find_component<StaticMeshComponent>()->parts.front().material;
}
} // namespace

Actor& spawn_component_actor(Scene& scene, ComponentType type, const std::string& name, const Transform& transform) {
    Actor& actor = scene.create_actor(scene.unique_actor_name(name));
    actor.transform = transform;
    if (auto component = create_component(type)) {
        actor.add_component(std::move(component));
    }
    return actor;
}

void build_default_scene(Scene& scene, AssetManager& assets) {
    scene.clear();
    Transform ground;
    ground.scale = {14.0f, 1.0f, 14.0f};
    Actor& floor = spawn_primitive(scene, assets, BuiltinMesh::Plane, "Ground", ground);
    floor.find_component<StaticMeshComponent>()->parts.front().material.base_color = {0.36f, 0.37f, 0.36f};
    floor.find_component<StaticMeshComponent>()->parts.front().material.roughness = 0.8f;

    Transform sphere;
    sphere.translation = {0.0f, 0.5f, 0.0f};
    Actor& ball = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Sphere", sphere);
    Material& ball_material = ball.find_component<StaticMeshComponent>()->parts.front().material;
    ball_material.base_color = {0.83f, 0.68f, 0.38f};
    ball_material.metallic = 1.0f;
    ball_material.roughness = 0.28f;

    Transform cube;
    cube.translation = {-1.4f, 0.5f, -0.6f};
    cube.rotation = {0.0f, 0.5f, 0.0f};
    Actor& box = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Cube", cube);
    box.find_component<StaticMeshComponent>()->parts.front().material.base_color = {0.14f, 0.15f, 0.16f};
    box.find_component<StaticMeshComponent>()->parts.front().material.roughness = 0.45f;

    Transform light;
    light.translation = {1.35f, 4.0f, 1.05f};
    light.rotation = {0.62f, -0.55f, 0.0f};
    spawn_light(scene, LightKind::Directional, "Sun", light);
    scene.select_actor(ball.id());
    scene.update_world_transforms();
}

void build_showcase_scene(Scene& scene, AssetManager& assets) {
    scene.clear();
    // Terrain + lake
    {
        Transform t;
        t.translation = {0.0f, -2.0f, 0.0f};
        Actor& terrain = spawn_component_actor(scene, ComponentType::Terrain, "Terrain", t);
        auto& c = *terrain.find_component<TerrainComponent>();
        c.size = 120.0f;
        c.resolution = 193;
        c.height = 14.0f;
        c.seed = 11;
        c.frequency = 1.4f;
        c.flatten = 0.75f;
        c.grass_density = 3.0f;
        c.basin_depth = 4.5f;
        c.basin_radius = 24.0f;
        c.basin_z = -17.0f;
        c.grass_min_height = 2.25f; // above the waterline
        Transform w;
        w.translation = {0.0f, 0.0f, 0.0f};
        Actor& water = spawn_component_actor(scene, ComponentType::Water, "Lake", w);
        water.find_component<WaterComponent>()->size_x = 120.0f;
        water.find_component<WaterComponent>()->size_z = 120.0f;
    }
    // Material gallery on a stone plinth.
    Transform plinth_t;
    plinth_t.translation = {0.0f, 1.2f, 6.0f};
    plinth_t.scale = {9.0f, 0.8f, 2.4f};
    Actor& plinth = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Plinth", plinth_t);
    material_of(plinth).base_color = {0.42f, 0.41f, 0.39f};
    material_of(plinth).roughness = 0.85f;
    struct Sample {
        const char* name;
        void (*setup)(Material&);
    };
    const Sample samples[] = {
        {"Gold", [](Material& m) { m.base_color = {1.0f, 0.78f, 0.34f}; m.metallic = 1.0f; m.roughness = 0.22f; }},
        {"Car paint", [](Material& m) { m.base_color = {0.55f, 0.02f, 0.03f}; m.roughness = 0.45f; m.clearcoat = 1.0f; m.clearcoat_roughness = 0.04f; }},
        {"Glass", [](Material& m) { m.base_color = {0.95f, 0.98f, 1.0f}; m.roughness = 0.03f; m.transmission = 1.0f; m.thickness = 0.5f; m.attenuation_color = {0.75f, 0.92f, 0.9f}; m.attenuation_distance = 1.5f; }},
        {"Velvet", [](Material& m) { m.base_color = {0.18f, 0.04f, 0.3f}; m.roughness = 0.9f; m.sheen_color = {0.85f, 0.6f, 1.0f}; m.sheen_roughness = 0.35f; }},
        {"Brushed steel", [](Material& m) { m.base_color = {0.86f, 0.86f, 0.88f}; m.metallic = 1.0f; m.roughness = 0.35f; m.anisotropy = 0.8f; }},
        {"Wax", [](Material& m) { m.base_color = {0.95f, 0.88f, 0.72f}; m.roughness = 0.4f; m.subsurface = 0.8f; m.subsurface_color = {1.0f, 0.55f, 0.3f}; }},
        {"Emissive", [](Material& m) { m.base_color = {0.05f, 0.05f, 0.05f}; m.emissive_factor = {0.83f, 1.0f, 0.25f}; m.emissive_strength = 6.0f; }},
    };
    float x = -3.6f;
    for (const Sample& sample : samples) {
        Transform t;
        t.translation = {x, 2.1f, 6.0f};
        t.scale = {0.95f, 0.95f, 0.95f};
        Actor& ball = spawn_primitive(scene, assets, BuiltinMesh::Sphere, sample.name, t);
        sample.setup(material_of(ball));
        x += 1.2f;
    }
    // Local lights + particles: a "campfire" by the shore.
    {
        Transform fire;
        fire.translation = {-6.5f, 0.75f, 7.0f};
        Actor& emitter = spawn_component_actor(scene, ComponentType::ParticleEmitter, "Campfire", fire);
        auto& flames = *emitter.find_component<ParticleEmitterComponent>();
        flames.rate = 140.0f;
        flames.lifetime = 0.8f;
        flames.lifetime_variance = 0.35f;
        flames.shape_size = {0.25f, 0.25f, 0.25f};
        flames.cone_angle = 0.25f;
        flames.speed = 1.1f;
        flames.gravity = -0.25f;
        flames.drag = 1.2f;
        flames.size_start = 0.35f;
        flames.size_end = 0.08f;
        flames.color_start = {1.0f, 0.55f, 0.15f};
        flames.color_end = {0.8f, 0.12f, 0.02f};
        flames.emissive = 2.5f;
        flames.noise = 1.2f;
        // Scorch mark under the fire (decal with the built-in splat).
        Transform scorch_t;
        scorch_t.translation = {-6.5f, 0.6f, 7.0f};
        scorch_t.scale = {2.4f, 1.5f, 2.4f};
        Actor& scorch = spawn_component_actor(scene, ComponentType::Decal, "Scorch", scorch_t);
        auto& scorch_decal = *scorch.find_component<DecalComponent>();
        scorch_decal.color = {0.05f, 0.04f, 0.035f};
        scorch_decal.opacity = 0.9f;
        scorch_decal.override_roughness = true;
        scorch_decal.roughness = 0.95f;
        Transform smoke_t;
        smoke_t.translation = {-6.5f, 1.3f, 7.0f};
        Actor& smoke_actor = spawn_component_actor(scene, ComponentType::ParticleEmitter, "Smoke", smoke_t);
        auto& smoke = *smoke_actor.find_component<ParticleEmitterComponent>();
        smoke.blend = ParticleBlend::Alpha;
        smoke.rate = 18.0f;
        smoke.max_particles = 512;
        smoke.lifetime = 4.5f;
        smoke.speed = 0.7f;
        smoke.cone_angle = 0.2f;
        smoke.gravity = -0.12f;
        smoke.drag = 0.4f;
        smoke.size_start = 0.35f;
        smoke.size_end = 2.0f;
        smoke.color_start = {0.35f, 0.33f, 0.31f};
        smoke.color_end = {0.55f, 0.55f, 0.56f};
        smoke.alpha_start = 0.45f;
        smoke.alpha_end = 0.0f;
        smoke.emissive = 0.0f;
        smoke.noise = 0.8f;
        Transform light;
        light.translation = {-6.5f, 1.4f, 7.0f};
        Actor& fire_light = spawn_light(scene, LightKind::Point, "Fire light", light);
        auto& l = *fire_light.find_component<LightComponent>();
        l.color = {1.0f, 0.55f, 0.2f};
        l.intensity = 3000.0f;
        l.range = 10.0f;
        Transform spot;
        spot.translation = {4.0f, 6.0f, 9.0f};
        spot.rotation = {0.9f, 0.3f, 0.0f};
        Actor& spot_light = spawn_light(scene, LightKind::Spot, "Spot light", spot);
        spot_light.find_component<LightComponent>()->intensity = 6000.0f;
        spot_light.find_component<LightComponent>()->range = 16.0f;
    }
    // Lighting helpers.
    {
        Transform probe;
        probe.translation = {0.0f, 2.6f, 6.0f};
        Actor& p = spawn_component_actor(scene, ComponentType::ReflectionProbe, "Reflection probe", probe);
        p.find_component<ReflectionProbeComponent>()->extents = {7.0f, 3.0f, 4.0f};
        Transform volume;
        volume.translation = {0.0f, 2.5f, 4.0f};
        Actor& v = spawn_component_actor(scene, ComponentType::IrradianceVolume, "Irradiance volume", volume);
        auto& vc = *v.find_component<IrradianceVolumeComponent>();
        vc.extents = {10.0f, 3.0f, 8.0f};
        vc.probes_x = 6;
        vc.probes_y = 3;
        vc.probes_z = 6;
    }
    Transform sun;
    sun.translation = {0.0f, 20.0f, 0.0f};
    sun.rotation = {1.28f, -0.35f, 0.0f};
    Actor& sun_actor = spawn_light(scene, LightKind::Directional, "Sun", sun);
    sun_actor.find_component<LightComponent>()->intensity = 70000.0f;
    sun_actor.find_component<LightComponent>()->use_temperature = true;
    sun_actor.find_component<LightComponent>()->temperature = 4300.0f;
    scene.update_world_transforms();
}

namespace {

Actor& add_body(Actor& actor, ColliderShape shape, float mass, bool kinematic = false) {
    auto& collider = actor.add_component<ColliderComponent>();
    collider.shape = shape;
    fit_collider_to_mesh(actor, collider);
    if (mass > 0.0f || kinematic) {
        auto& body = actor.add_component<RigidBodyComponent>();
        body.mass = std::max(mass, 0.1f);
        body.is_kinematic = kinematic;
    }
    return actor;
}

ScriptComponent& add_script(Actor& actor, const std::string& name) {
    auto& script = actor.add_component<ScriptComponent>();
    ScriptRegistry::instance().assign(script, name);
    return script;
}

void set_param(ScriptComponent& script, const std::string& name, float value) {
    for (ScriptParam& param : script.params) {
        if (param.name == name) {
            param.number = value;
        }
    }
}


} // namespace

void build_demo_scene(Scene& scene, AssetManager& assets) {
    scene.clear();

    Transform ground;
    ground.translation = {0.0f, -0.51f, 0.0f};
    ground.scale = {30.0f, 1.0f, 30.0f};
    Actor& floor = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Ground", ground);
    material_of(floor).base_color = {0.42f, 0.44f, 0.43f};
    material_of(floor).roughness = 0.9f;
    add_body(floor, ColliderShape::Box, 0.0f);

    Transform player_transform;
    player_transform.translation = {0.0f, 1.0f, 3.0f};
    Actor& player = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Player", player_transform);
    material_of(player).base_color = {0.25f, 0.55f, 0.95f};
    material_of(player).roughness = 0.35f;
    add_body(player, ColliderShape::Sphere, 1.0f);
    player.find_component<RigidBodyComponent>()->friction = 0.2f;
    player.find_component<RigidBodyComponent>()->freeze_rotation = true; // steered, not rolled
    add_script(player, "PlayerController");

    Transform camera_transform;
    camera_transform.translation = {0.0f, 4.5f, 10.0f};
    camera_transform.rotation = rotation_for_forward(Vec3{0.0f, 1.0f, 3.0f} - camera_transform.translation);
    Actor& camera = spawn_camera(scene, "Main Camera", camera_transform);
    add_script(camera, "FollowCamera");

    // A small pyramid of crates to knock over.
    int crate_index = 0;
    for (int layer = 0; layer < 3; ++layer) {
        for (int i = 0; i < 3 - layer; ++i) {
            Transform crate;
            crate.translation = {-1.05f + static_cast<float>(i) * 1.05f + static_cast<float>(layer) * 0.525f, 0.5f + static_cast<float>(layer) * 1.0f, -3.0f};
            Actor& box = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Crate " + std::to_string(++crate_index), crate);
            material_of(box).base_color = {0.62f, 0.43f, 0.24f};
            material_of(box).roughness = 0.7f;
            add_body(box, ColliderShape::Box, 0.6f);
        }
    }

    Transform spinner_transform;
    spinner_transform.translation = {-6.0f, 0.3f, -1.0f};
    spinner_transform.scale = {3.0f, 0.6f, 3.0f};
    Actor& spinner = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Spinning Block", spinner_transform);
    material_of(spinner).base_color = {0.8f, 0.3f, 0.3f};
    add_body(spinner, ColliderShape::Box, 0.0f, true); // kinematic: moved by its script
    set_param(add_script(spinner, "Rotator"), "speed_y", 30.0f);

    Transform lift_transform;
    lift_transform.translation = {6.0f, 1.0f, -2.0f};
    lift_transform.scale = {2.5f, 0.3f, 2.5f};
    Actor& lift = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Lift", lift_transform);
    material_of(lift).base_color = {0.3f, 0.75f, 0.45f};
    add_body(lift, ColliderShape::Box, 0.0f, true);
    ScriptComponent& lift_script = add_script(lift, "Oscillator");
    set_param(lift_script, "amplitude", 0.9f);
    set_param(lift_script, "frequency", 0.25f);

    const Vec3 coin_positions[] = {{3.0f, 0.6f, 0.0f}, {-3.0f, 0.6f, 2.0f}, {6.0f, 2.6f, -2.0f}, {0.0f, 0.6f, -6.0f}};
    int coin_index = 0;
    for (Vec3 position : coin_positions) {
        Transform coin;
        coin.translation = position;
        coin.scale = {0.4f, 0.4f, 0.4f};
        Actor& pickup = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Coin " + std::to_string(++coin_index), coin);
        material_of(pickup).base_color = {1.0f, 0.8f, 0.2f};
        material_of(pickup).metallic = 1.0f;
        material_of(pickup).roughness = 0.25f;
        material_of(pickup).emissive_factor = {0.6f, 0.4f, 0.05f};
        add_body(pickup, ColliderShape::Sphere, 0.0f);
        pickup.find_component<ColliderComponent>()->is_trigger = true;
        add_script(pickup, "Collectible");
    }

    Transform key;
    key.translation = {2.0f, 6.0f, 2.0f};
    key.rotation = {0.75f, -0.5f, 0.0f};
    spawn_light(scene, LightKind::Directional, "Sun", key);

    Transform fill;
    fill.translation = {-4.0f, 3.0f, 3.0f};
    Actor& fill_light = spawn_light(scene, LightKind::Point, "Fill Light", fill);
    fill_light.find_component<LightComponent>()->color = {1.0f, 0.75f, 0.5f};
    fill_light.find_component<LightComponent>()->range = 12.0f;
    fill_light.find_component<LightComponent>()->cast_shadows = false;

    scene.select_actor(player.id());
    scene.update_world_transforms();
}

} // namespace Hollow

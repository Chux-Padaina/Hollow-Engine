// Engine logic tests (no window or GPU needed): math, JSON, scene hierarchy,
// serialization, undo, physics, scripting/Play mode, and model import.
// Build with -DHOLLOW_BUILD_TESTS=ON (default) and run `ctest` or
// HollowTests.exe [path/to/assets/showcase].
#include "Hollow/Assets/AssetManager.hpp"
#include "Hollow/Assets/Importer.hpp"
#include "Hollow/Core/Engine.hpp"
#include "Hollow/Core/Input.hpp"
#include "Hollow/Core/Json.hpp"
#include "Hollow/Core/Log.hpp"
#include "Hollow/Core/Math.hpp"
#include "Hollow/Scene/SceneFactory.hpp"
#include "Hollow/Scene/SceneSerializer.hpp"
#include "Hollow/Scene/UndoStack.hpp"
#include "Hollow/Assets/Animation.hpp"
#include "Hollow/Assets/MeshSimplify.hpp"
#include "Hollow/Core/PropertiesJson.hpp"
#include "Hollow/Scene/AnimationSystem.hpp"

#include <algorithm>
#include <cmath>

#include <cstdio>
#include <fstream>
#include <string>

using namespace Hollow;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
static bool near3(Vec3 a, Vec3 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

static void test_math() {
    Transform t;
    t.translation = {1, 2, 3};
    t.rotation = {0.3f, -0.7f, 1.1f};
    t.scale = {2, 0.5f, 3};
    const Mat4 m = t.matrix();
    const Mat4 id = m * inverse(m);
    for (int i = 0; i < 16; ++i) CHECK(near(id.m[i], (i % 5 == 0) ? 1.0f : 0.0f));
    const Transform d = decompose(m);
    CHECK(near3(d.translation, t.translation));
    CHECK(near3(d.scale, t.scale));
    CHECK(near3(d.rotation, t.rotation));
    // forward/rotation round trip
    const Vec3 f = normalize(Vec3{0.3f, -0.5f, -0.8f});
    CHECK(near3(forward_from_rotation(rotation_for_forward(f)), f));
    // look_at straight down must not produce NaN (old CSM bug)
    const Mat4 down = Mat4::look_at({0, 10, 0}, {0, 0, 0}, {0, 1, 0});
    for (float v : down.m) CHECK(std::isfinite(v));
    // frustum
    const Mat4 vp = Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f) * Mat4::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    const Frustum fr = Frustum::from_matrix(vp);
    CHECK(fr.intersects_sphere({0, 0, 0}, 0.5f));
    CHECK(!fr.intersects_sphere({0, 0, 20}, 0.5f)); // behind camera
    CHECK(!fr.intersects_sphere({100, 0, 0}, 1.0f));
    // rays
    const Ray r{{0, 0, 5}, {0, 0, -1}};
    CHECK(near(ray_aabb(r, {{-1, -1, -1}, {1, 1, 1}}), 4.0f));
    CHECK(near(ray_sphere(r, {0, 0, 0}, 1.0f), 4.0f));
    CHECK(near(ray_triangle(r, {-1, -1, 0}, {1, -1, 0}, {0, 1, 0}), 5.0f));
    CHECK(ray_triangle(r, {2, 2, 0}, {3, 2, 0}, {2, 3, 0}) < 0.0f);
    // transformed box
    const Aabb box = transform_aabb({{-1, -1, -1}, {1, 1, 1}}, Mat4::translation({5, 0, 0}) * Mat4::scale({2, 1, 1}));
    CHECK(near3(box.min, {3, -1, -1}) && near3(box.max, {7, 1, 1}));
}

static void test_json() {
    const std::string text = R"({"a": [1, 2.5, -3e2], "s": "café 😀 \"q\"", "b": true, "n": null, "o": {"x": 1}})";
    const JsonValue v = parse_json(text);
    CHECK(v.at("a").size() == 3);
    CHECK(near(v.at("a").at(std::size_t{2}).as_float(), -300.0f));
    CHECK(v.at("s").as_string() == "caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\"");
    CHECK(v.at("b").as_bool());
    const JsonValue round = parse_json(write_json(v, true));
    CHECK(write_json(round, false) == write_json(v, false));
    CHECK(write_json(JsonValue(0.1f), false) == "0.1");
    bool threw = false;
    try { parse_json("{\"a\": [1, 2"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

static void test_scene_hierarchy(AssetManager& assets) {
    Scene scene;
    Actor& parent = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Parent", {});
    parent.transform.translation = {10, 0, 0};
    parent.transform.rotation = {0, kPi * 0.5f, 0};
    Actor& child = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Child", {});
    child.transform.translation = {1, 2, 3};
    const ActorId p = parent.id();
    const ActorId c = child.id();
    CHECK(scene.set_parent(c, p, true));
    scene.update_world_transforms();
    CHECK(near3(scene.find(c)->world_position(), {1, 2, 3})); // world kept
    CHECK(!scene.set_parent(p, c, true)); // cycle refused
    const ActorId copy = scene.duplicate_actor(p);
    CHECK(scene.actor_count() == 4);
    CHECK(scene.find(copy)->children().size() == 1);
    CHECK(scene.find(copy)->name() == "Parent 2");
    scene.destroy_actor(p);
    CHECK(scene.actor_count() == 2);
    CHECK(scene.find(c) == nullptr);
}

static void test_serialization(AssetManager& assets) {
    Scene scene;
    build_demo_scene(scene, assets);
    Actor* camera = scene.find_by_name("Main Camera");
    Actor* player = scene.find_by_name("Player");
    CHECK(camera != nullptr && player != nullptr);
    scene.set_parent(camera->id(), player->id(), true);
    const JsonValue first = serialize_scene(scene);
    Scene loaded;
    deserialize_scene(loaded, first, assets);
    const JsonValue second = serialize_scene(loaded);
    // Same actors with the same transforms (degrees<->radians conversion
    // allows tiny float differences in the text).
    for (const auto& actor : scene.actors()) {
        const Actor* other = loaded.find(actor->id());
        CHECK(other != nullptr && other->name() == actor->name() && other->parent() == actor->parent());
        CHECK(other != nullptr && near3(other->transform.translation, actor->transform.translation) && near3(other->transform.rotation, actor->transform.rotation) && near3(other->transform.scale, actor->transform.scale));
        CHECK(other != nullptr && other->components().size() == actor->components().size());
    }
    (void)second;
    CHECK(loaded.actor_count() == scene.actor_count());
    CHECK(loaded.find_by_name("Main Camera")->parent() == loaded.find_by_name("Player")->id());
    const auto* script = loaded.find_by_name("Player")->find_component<ScriptComponent>();
    CHECK(script != nullptr && script->script_name == "PlayerController" && near(script->number("speed"), 4.0f));
    // Shared mesh assets: every crate references the same cube.
    CHECK(loaded.find_by_name("Crate 1")->find_component<StaticMeshComponent>()->parts[0].mesh == assets.builtin_mesh(BuiltinMesh::Cube));

    // File round trip with render settings + camera.
    SceneDocument doc;
    doc.camera.distance = 12.0f;
    doc.settings.exposure.mode = ExposureMode::Manual;
    doc.settings.exposure.ev100 = 11.5f;
    doc.settings.fog.enabled = true;
    doc.settings.fog.density = 0.033f;
    doc.settings.sky.mode = SkyMode::Color;
    doc.settings.post.tonemapper = Tonemapper::KhronosNeutral;
    doc.settings.aa.render_scale = 0.75f;
    const auto path = std::filesystem::temp_directory_path() / "hollow_test.hscene";
    save_scene_file(path, scene, doc);
    Scene file_scene;
    const SceneDocument back = load_scene_file(path, file_scene, assets);
    CHECK(near(back.camera.distance, 12.0f) && back.settings.exposure.mode == ExposureMode::Manual && near(back.settings.exposure.ev100, 11.5f));
    CHECK(back.settings.fog.enabled && near(back.settings.fog.density, 0.033f) && back.settings.sky.mode == SkyMode::Color);
    CHECK(back.settings.post.tonemapper == Tonemapper::KhronosNeutral && near(back.settings.aa.render_scale, 0.75f));
    CHECK(file_scene.actor_count() == scene.actor_count());
}

static void test_undo() {
    UndoStack undo;
    undo.record(JsonValue(1));
    undo.record(JsonValue(2));
    auto a = undo.undo(JsonValue(3));
    CHECK(a && a->as_int() == 2);
    auto b = undo.undo(JsonValue(2));
    CHECK(b && b->as_int() == 1);
    CHECK(!undo.can_undo());
    auto c = undo.redo(JsonValue(1));
    CHECK(c && c->as_int() == 2);
    undo.record(JsonValue(9));
    CHECK(!undo.can_redo());
}

static void test_physics(AssetManager& assets) {
    Scene scene;
    Transform ground_t;
    ground_t.translation = {0, -0.5f, 0};
    ground_t.scale = {20, 1, 20};
    Actor& ground = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Ground", ground_t);
    fit_collider_to_mesh(ground, ground.add_component<ColliderComponent>());

    Transform ball_t;
    ball_t.translation = {0, 5, 0};
    Actor& ball = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Ball", ball_t);
    auto& ball_collider = ball.add_component<ColliderComponent>();
    ball_collider.shape = ColliderShape::Sphere;
    fit_collider_to_mesh(ball, ball_collider);
    ball.add_component<RigidBodyComponent>();

    // A stack of 3 boxes.
    for (int i = 0; i < 3; ++i) {
        Transform t;
        t.translation = {3, 0.5f + static_cast<float>(i) * 1.0f, 0};
        Actor& box = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Box", t);
        fit_collider_to_mesh(box, box.add_component<ColliderComponent>());
        box.add_component<RigidBodyComponent>();
    }

    Transform trigger_t;
    trigger_t.translation = {0, 2, 0};
    Actor& trigger = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Trigger", trigger_t);
    auto& trigger_collider = trigger.add_component<ColliderComponent>();
    trigger_collider.shape = ColliderShape::Sphere;
    trigger_collider.is_trigger = true;
    fit_collider_to_mesh(trigger, trigger_collider);

    PhysicsWorld physics;
    bool trigger_hit = false;
    for (int step = 0; step < 240; ++step) {
        physics.step(scene, 1.0f / 60.0f);
        for (const Contact& contact : physics.contacts()) {
            if (contact.trigger && (contact.a == trigger.id() || contact.b == trigger.id())) trigger_hit = true;
        }
    }
    const Vec3 ball_pos = scene.find(ball.id())->world_position();
    std::printf("  ball rests at y=%.4f (expect ~0.5), grounded=%d\n", ball_pos.y, ball.find_component<RigidBodyComponent>()->grounded);
    CHECK(near(ball_pos.y, 0.5f, 0.02f));
    CHECK(ball.find_component<RigidBodyComponent>()->grounded);
    CHECK(trigger_hit);
    // Stack stays stacked.
    int index = 0;
    for (const auto& actor : scene.actors()) {
        if (actor->name().rfind("Box", 0) == 0) {
            const float y = actor->world_position().y;
            std::printf("  box %d y=%.4f\n", index, y);
            CHECK(near(y, 0.5f + static_cast<float>(index), 0.05f));
            CHECK(near(actor->world_position().x, 3.0f, 0.05f));
            ++index;
        }
    }
    RaycastHit hit;
    CHECK(physics.raycast(scene, {{0, 10, 0}, {0, -1, 0}}, 100.0f, hit, kNoActor));
    CHECK(hit.actor == ball.id());
    CHECK(near(hit.point.y, 1.0f, 0.03f));
    CHECK(near3(hit.normal, {0, 1, 0}, 0.05f));
}

// Jolt-specific behaviour: rotation, trigger enter/exit, layers, characters.
static void test_physics_advanced(AssetManager& assets) {
    Scene scene;
    Transform ground_t;
    ground_t.translation = {0, -0.5f, 0};
    ground_t.scale = {40, 1, 40};
    Actor& ground = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Ground", ground_t);
    fit_collider_to_mesh(ground, ground.add_component<ColliderComponent>());

    // A box dropped on its edge tips over and comes to rest flat (it rotates).
    Transform tilted;
    tilted.translation = {5, 2, 0};
    tilted.rotation = {0.0f, 0.0f, 0.6f};
    Actor& box = spawn_primitive(scene, assets, BuiltinMesh::Cube, "Tilted", tilted);
    fit_collider_to_mesh(box, box.add_component<ColliderComponent>());
    box.add_component<RigidBodyComponent>();

    // A ball falling through a trigger: enter then exit.
    Transform ball_t;
    ball_t.translation = {-5, 6, 0};
    Actor& ball = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Ball", ball_t);
    auto& ball_collider = ball.add_component<ColliderComponent>();
    ball_collider.shape = ColliderShape::Sphere;
    fit_collider_to_mesh(ball, ball_collider);
    ball.add_component<RigidBodyComponent>();
    Transform zone_t;
    zone_t.translation = {-5, 3, 0};
    Actor& zone = scene.create_actor("Zone");
    zone.transform = zone_t;
    auto& zone_collider = zone.add_component<ColliderComponent>();
    zone_collider.half_extents = {1, 0.5f, 1};
    zone_collider.is_trigger = true;

    // A ball on layer 1 that ignores layer 0 falls through the ground.
    Transform ghost_t;
    ghost_t.translation = {10, 2, 0};
    Actor& ghost = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Ghost", ghost_t);
    auto& ghost_collider = ghost.add_component<ColliderComponent>();
    ghost_collider.shape = ColliderShape::Sphere;
    ghost_collider.layer = 1;
    fit_collider_to_mesh(ghost, ghost_collider);
    ghost.add_component<RigidBodyComponent>();

    // A capsule character walking +X.
    Actor& hero = scene.create_actor("Hero");
    hero.transform.translation = {0, 0.2f, 5};
    auto& character = hero.add_component<CharacterControllerComponent>();

    PhysicsWorld physics;
    physics.settings.set_collides(1, 0, false);
    int enters = 0;
    int exits = 0;
    for (int step = 0; step < 240; ++step) {
        character.velocity.x = 2.0f;
        character.velocity.z = 0.0f;
        physics.step(scene, 1.0f / 60.0f);
        for (const ContactEvent& e : physics.events()) {
            if (e.trigger && (e.a == zone.id() || e.b == zone.id())) {
                (e.begin ? enters : exits) += 1;
            }
        }
    }
    const Vec3 box_up = transform_vector(scene.find(box.id())->world_matrix(), {0, 1, 0});
    const float box_y = scene.find(box.id())->world_position().y;
    std::printf("  tilted box y=%.3f, up=(%.2f, %.2f, %.2f); trigger enters=%d exits=%d\n", box_y, box_up.x, box_up.y, box_up.z, enters, exits);
    CHECK(near(box_y, 0.5f, 0.05f));
    CHECK(std::abs(std::abs(box_up.y) - 1.0f) < 0.05f || std::abs(box_up.y) < 0.05f); // resting on a face
    CHECK(enters == 1 && exits == 1);
    CHECK(scene.find(ghost.id())->world_position().y < -1.0f);
    const Vec3 hero_pos = scene.find(hero.id())->world_position();
    std::printf("  character at (%.2f, %.2f, %.2f), grounded=%d\n", hero_pos.x, hero_pos.y, hero_pos.z, character.grounded);
    CHECK(character.grounded);
    CHECK(near(hero_pos.y, 0.0f, 0.06f));
    CHECK(hero_pos.x > 6.0f);
    // Overlap query finds the ground under the character.
    const std::vector<ActorId> near_hero = physics.overlap_sphere(hero_pos, 0.3f);
    CHECK(std::find(near_hero.begin(), near_hero.end(), ground.id()) != near_hero.end());
}

// Bodies and characters rest on terrain (height field from the terrain function).
static void test_terrain_collision(AssetManager& assets) {
    Scene scene;
    Actor& terrain_actor = scene.create_actor("Terrain");
    terrain_actor.transform.translation = {0.0f, -2.0f, 0.0f};
    auto& terrain = terrain_actor.add_component<TerrainComponent>();
    terrain.size = 64.0f;
    terrain.height = 10.0f;
    Transform ball_t;
    ball_t.translation = {6.0f, 20.0f, -4.0f};
    Actor& ball = spawn_primitive(scene, assets, BuiltinMesh::Sphere, "Ball", ball_t);
    auto& collider = ball.add_component<ColliderComponent>();
    collider.shape = ColliderShape::Sphere;
    fit_collider_to_mesh(ball, collider);
    ball.add_component<RigidBodyComponent>().freeze_rotation = true;
    ball.find_component<RigidBodyComponent>()->linear_damping = 2.0f; // settle instead of rolling away
    PhysicsWorld physics;
    for (int step = 0; step < 300; ++step) {
        physics.step(scene, 1.0f / 60.0f);
    }
    const Vec3 p = scene.find(ball.id())->world_position();
    const float ground = terrain.sample_height(p.x, p.z) - 2.0f;
    std::printf("  ball at (%.2f, %.2f, %.2f), terrain %.2f\n", p.x, p.y, p.z, ground);
    CHECK(p.y > ground && p.y < ground + 1.0f);
}

static void test_scripts_and_play(AssetManager&) {
    Engine engine;
    build_demo_scene(engine.scene(), engine.assets());
    Input input;
    engine.begin_play();
    const Vec3 start = engine.scene().find_by_name("Player")->world_position();
    // Hold D (move +X towards Coin 1 at (3, 0.6, 0) from (0, 1, 3) after
    // settling: also hold W).
    for (int frame = 0; frame < 30; ++frame) {
        input.begin_frame();
        engine.update(1.0f / 60.0f, input);
    }
    const std::size_t coins_before = [&] { std::size_t n = 0; for (auto& a : engine.scene().actors()) if (a->name().rfind("Coin", 0) == 0) ++n; return n; }();
    input.on_key('D', true);
    input.on_key('W', true);
    for (int frame = 0; frame < 90; ++frame) {
        input.begin_frame();
        engine.update(1.0f / 60.0f, input);
    }
    const Vec3 moved = engine.scene().find_by_name("Player")->world_position();
    std::printf("  player %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f\n", start.x, start.y, start.z, moved.x, moved.y, moved.z);
    CHECK(moved.x > start.x + 1.0f);
    CHECK(moved.z < start.z - 1.0f);
    std::size_t coins_after = 0;
    for (auto& a : engine.scene().actors()) if (a->name().rfind("Coin", 0) == 0) ++coins_after;
    std::printf("  coins %zu -> %zu\n", coins_before, coins_after);
    CHECK(coins_after < coins_before);
    // Rotator spins the block.
    CHECK(std::abs(engine.scene().find_by_name("Spinning Block")->transform.rotation.y) > 0.5f);
    // Camera followed.
    const Vec3 cam = engine.scene().find_by_name("Main Camera")->world_position();
    CHECK(length(cam - moved) < 12.0f);
    engine.end_play();
}

static void test_import(AssetManager& assets, const char* root) {
    if (root == nullptr) return;
    const std::filesystem::path base(root);
    for (const char* rel : {"characters/adventurer.glb", "dungeon/room-large-variation.glb", "dungeon/corridor.glb"}) {
        const auto path = base / rel;
        if (!std::filesystem::exists(path)) continue;
        ModelHandle model = assets.load_model(path);
        std::size_t triangles = 0;
        for (const auto& part : model->parts) triangles += part.mesh->indices.size() / 3;
        std::printf("  %s: %zu parts, %zu tris, bounds %.2f..%.2f x %.2f..%.2f x %.2f..%.2f, tex0=%s\n", rel, model->parts.size(), triangles,
            model->bounds.min.x, model->bounds.max.x, model->bounds.min.y, model->bounds.max.y, model->bounds.min.z, model->bounds.max.z,
            model->parts.empty() ? "" : model->parts[0].material.base_color_texture_path.c_str());
        CHECK(!model->parts.empty() && triangles > 0);
        CHECK(assets.load_model(path) == model); // cached
    }
    // Tiny OBJ with a quad and shared vertices.
    const auto obj = std::filesystem::temp_directory_path() / "hollow_quad.obj";
    {
        std::ofstream out(obj);
        out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvn 0 0 1\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n";
    }
    ImportedModel quad = import_model_file(obj);
    CHECK(quad.parts.size() == 1 && quad.parts[0].indices.size() == 6 && quad.parts[0].vertices.size() == 4);
}

// Version-1 files: old light units and flat render settings migrate.
static void test_migration(AssetManager& assets) {
    const JsonValue v1 = parse_json(R"({
        "actors": [{"id": 1, "name": "Sun", "parent": 0,
                    "transform": {"position": [0, 5, 0], "rotation_degrees": [-60, 0, 0], "scale": [1, 1, 1]},
                    "components": [{"type": "Light", "kind": "Directional", "color": [1, 1, 1], "intensity": 3.0}]}]
    })");
    Scene scene;
    deserialize_scene(scene, v1, assets, {}, 1);
    const Actor* sun = scene.find_by_name("Sun");
    CHECK(sun != nullptr);
    const auto* light = sun != nullptr ? sun->find_component<LightComponent>() : nullptr;
    CHECK(light != nullptr && light->intensity > 1000.0f); // scaled into lux
    RenderPipelineSettings settings;
    migrate_render_settings_v1(settings, parse_json(R"({"exposure": 2.0, "bloom": true})"), false);
    CHECK(settings.exposure.mode == ExposureMode::Manual && near(settings.exposure.ev100, 12.0f));
}

// Component property descriptions round-trip through JSON.
static void test_component_properties() {
    WaterComponent water;
    water.wave_height = 0.7f;
    water.shallow_color = {0.2f, 0.3f, 0.4f};
    JsonValue json = JsonValue::object();
    {
        JsonWriteVisitor writer(json, {});
        water.describe(writer);
    }
    WaterComponent back;
    JsonReadVisitor reader(json, {});
    back.describe(reader);
    CHECK(near(back.wave_height, 0.7f) && near3(back.shallow_color, {0.2f, 0.3f, 0.4f}));

    TerrainComponent terrain;
    terrain.size = 64.0f;
    const float before = terrain.sample_height(5.0f, -3.0f);
    terrain.basin_depth = 4.0f;
    terrain.basin_x = 5.0f;
    terrain.basin_z = -3.0f;
    CHECK(near(terrain.sample_height(5.0f, -3.0f), before - 4.0f, 1e-3f)); // full depth at the center
    terrain.basin_x = 100.0f;
    CHECK(near(terrain.sample_height(5.0f, -3.0f), before)); // outside the radius
}

// Skeleton sampling, blending and the animator system.
static void test_animation(AssetManager& assets) {
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->joints.push_back({"root", -1, {}});
    Joint child{"child", 0, {}};
    child.rest.translation = {0.0f, 1.0f, 0.0f};
    skeleton->joints.push_back(child);
    AnimationClip clip;
    clip.name = "Move";
    clip.duration = 2.0f;
    AnimationChannel channel;
    channel.joint = 0;
    channel.path = AnimationPath::Translation;
    channel.times = {0.0f, 2.0f};
    channel.values = {0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f};
    clip.channels.push_back(channel);
    skeleton->clips.push_back(clip);

    std::vector<JointPose> pose = rest_pose(*skeleton);
    sample_clip(skeleton->clips[0], 1.0f, pose);
    CHECK(near3(pose[0].translation, {2.0f, 0.0f, 0.0f}));
    std::vector<Mat4> model;
    pose_to_model(*skeleton, pose, model);
    CHECK(model.size() == 2 && near(model[1].m[12], 2.0f) && near(model[1].m[13], 1.0f));

    // A skinned mesh on an actor with an animator gets joint matrices.
    MeshAsset mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    mesh.skin.resize(3);
    mesh.skin_joints = {0, 1};
    mesh.inverse_bind = {Mat4::identity(), Mat4::identity()};
    mesh.skeleton = skeleton;
    MeshHandle handle = make_mesh_asset(std::move(mesh));
    CHECK(handle->skinned() && handle->lods.size() == 1);
    Scene scene;
    Actor& actor = scene.create_actor("Skinned");
    actor.add_component<StaticMeshComponent>().parts.push_back({"part", handle, Material{}});
    auto& animator = actor.add_component<AnimatorComponent>();
    update_animators(scene, 0.5f, true);
    CHECK(animator.joint_matrices.size() == 2 && near(animator.joint_matrices[0].m[12], 1.0f));
    (void)assets;
}

// Mesh simplification produces smaller LODs.
static void test_lods(AssetManager& assets) {
    MeshHandle sphere = assets.builtin_mesh(BuiltinMesh::Sphere);
    CHECK(sphere != nullptr);
    if (sphere == nullptr) return;
    MeshAsset copy = *sphere;
    copy.lods.clear();
    generate_lods(copy);
    CHECK(copy.lods.size() >= 2);
    for (std::size_t i = 1; i < copy.lods.size(); ++i) {
        CHECK(copy.lods[i].index_count < copy.lods[i - 1].index_count);
        CHECK(copy.lods[i].first_index + copy.lods[i].index_count <= copy.indices.size());
    }
}

#ifndef HOLLOW_TEST_ASSETS
#define HOLLOW_TEST_ASSETS nullptr
#endif

int main(int argc, char** argv) {
    AssetManager assets;
    register_builtin_scripts();
    std::printf("math\n"); test_math();
    std::printf("json\n"); test_json();
    std::printf("hierarchy\n"); test_scene_hierarchy(assets);
    std::printf("serialization\n"); test_serialization(assets);
    std::printf("undo\n"); test_undo();
    std::printf("physics\n"); test_physics(assets);
    std::printf("physics (advanced)\n"); test_physics_advanced(assets);
    std::printf("terrain collision\n"); test_terrain_collision(assets);
    std::printf("scripts/play\n"); test_scripts_and_play(assets);
    std::printf("migration\n"); test_migration(assets);
    std::printf("component properties\n"); test_component_properties();
    std::printf("animation\n"); test_animation(assets);
    std::printf("lods\n"); test_lods(assets);
    std::printf("import\n"); test_import(assets, argc > 1 ? argv[1] : HOLLOW_TEST_ASSETS);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

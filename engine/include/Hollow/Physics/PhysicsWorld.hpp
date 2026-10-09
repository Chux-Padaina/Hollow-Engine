// Rigid-body physics on top of Jolt Physics.
//
// The scene is the source of truth: every step, PhysicsWorld mirrors the
// actors that have a ColliderComponent (and optionally a RigidBodyComponent)
// or a CharacterControllerComponent into Jolt bodies, simulates, and writes
// dynamic bodies' positions/rotations and velocities back to the actors.
//
//   - Collider without RigidBody: static scenery.
//   - Collider + RigidBody: dynamic, or kinematic (follows the actor).
//   - Collider with is_trigger: a sensor that reports overlaps.
//   - CharacterController: a capsule moved by velocity (Jolt CharacterVirtual)
//     that slides along walls, climbs steps and slopes and pushes bodies.
//
// Collision layers (0-7) and which layers collide live in PhysicsSettings.
// Jolt is an implementation detail: no Jolt headers leak out of this file.
#pragma once

#include "Hollow/Core/Properties.hpp"
#include "Hollow/Scene/Scene.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Hollow {

constexpr int kPhysicsLayerCount = 8;

struct PhysicsSettings {
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    std::array<std::string, kPhysicsLayerCount> layer_names{"Default", "Player", "Enemy", "Pickup", "Projectile", "Environment", "Layer 6", "Layer 7"};
    // Bit j of layer_mask[i]: layers i and j collide. Kept symmetric.
    std::array<std::uint8_t, kPhysicsLayerCount> layer_mask{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    bool collides(int a, int b) const;
    void set_collides(int a, int b, bool value);
};

// Scene-file / inspector description (the layer matrix has its own editor UI).
void describe(PhysicsSettings& settings, PropertyVisitor& v);

struct RaycastHit {
    ActorId actor = kNoActor;
    Vec3 point{};
    Vec3 normal{};
    float distance = 0.0f;
};

// Contacts touching after the most recent step (both orders are not
// listed: each pair appears once).
struct Contact {
    ActorId a = kNoActor;
    ActorId b = kNoActor;
    Vec3 normal{}; // points from a towards b
    float depth = 0.0f;
    bool trigger = false;
};

// A contact that started or ended during the most recent step.
struct ContactEvent {
    ActorId a = kNoActor;
    ActorId b = kNoActor;
    bool begin = true;
    bool trigger = false;
};

struct PhysicsStats {
    int bodies = 0;
    int dynamic_bodies = 0;
    int active_bodies = 0;
    int characters = 0;
    int contacts = 0;
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    PhysicsSettings settings;

    // Syncs the scene into the physics world, advances it by `dt` seconds
    // (call with a fixed step) and writes the results back to the scene.
    void step(Scene& scene, float dt);
    // Removes every body (call when Play starts/stops or the scene changes).
    void reset();

    // Nearest hit along the ray (triggers ignored), skipping `ignore`
    // (typically the caster itself). Works on the bodies of the last step.
    bool raycast(const Scene& scene, const Ray& ray, float max_distance, RaycastHit& hit, ActorId ignore = kNoActor) const;
    // Actors whose colliders overlap a sphere (triggers included).
    std::vector<ActorId> overlap_sphere(Vec3 center, float radius) const;

    const std::vector<Contact>& contacts() const { return contacts_; }
    const std::vector<ContactEvent>& events() const { return events_; }
    const PhysicsStats& stats() const { return stats_; }

    // World-space shape of an actor's collider: what the physics body uses
    // (also drawn by the editor).
    struct WorldShape {
        ColliderShape shape = ColliderShape::Box;
        Vec3 center{};       // world space
        Quat rotation{};     // world space (actor rotation)
        Vec3 half_extents{}; // box, scaled
        float radius = 0.0f; // sphere / capsule, scaled
        float half_height = 0.0f; // capsule cylinder half length, scaled
        Aabb bounds;
    };
    static WorldShape world_shape(const Actor& actor, const ColliderComponent& collider);
    static WorldShape world_shape(const Actor& actor, const CharacterControllerComponent& character);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<Contact> contacts_;
    std::vector<ContactEvent> events_;
    PhysicsStats stats_;
};

} // namespace Hollow

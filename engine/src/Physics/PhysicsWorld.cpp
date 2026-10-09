// PhysicsWorld on Jolt Physics: scene <-> body mirroring, contact and
// trigger events, character controllers and queries.
#include "Hollow/Physics/PhysicsWorld.hpp"

#include "Hollow/Core/Log.hpp"

// Jolt must be included first (it configures its own platform macros).
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Hollow {

// --- Settings ---------------------------------------------------------------------------

bool PhysicsSettings::collides(int a, int b) const {
    if (a < 0 || b < 0 || a >= kPhysicsLayerCount || b >= kPhysicsLayerCount) {
        return true;
    }
    return (layer_mask[static_cast<std::size_t>(a)] & (1u << b)) != 0;
}

void PhysicsSettings::set_collides(int a, int b, bool value) {
    if (a < 0 || b < 0 || a >= kPhysicsLayerCount || b >= kPhysicsLayerCount) {
        return;
    }
    auto set = [value](std::uint8_t& mask, int bit) {
        mask = static_cast<std::uint8_t>(value ? (mask | (1u << bit)) : (mask & ~(1u << bit)));
    };
    set(layer_mask[static_cast<std::size_t>(a)], b);
    set(layer_mask[static_cast<std::size_t>(b)], a);
}

void describe(PhysicsSettings& settings, PropertyVisitor& v) {
    v.vec3("gravity", "Gravity", settings.gravity, {-100.0f, 100.0f, 0.05f, "%.2f", false});
    if (PropertyGroup g{v, "layers", "Layers"}) {
        for (int i = 0; i < kPhysicsLayerCount; ++i) {
            const std::string key = "layer" + std::to_string(i);
            v.text(key.c_str(), ("Layer " + std::to_string(i)).c_str(), settings.layer_names[static_cast<std::size_t>(i)]);
            const std::string mask_key = key + "_mask";
            int mask = settings.layer_mask[static_cast<std::size_t>(i)];
            v.integer(mask_key.c_str(), (std::string("Mask ") + std::to_string(i)).c_str(), mask, 0, 255);
            settings.layer_mask[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(mask);
        }
    }
}

namespace {

// --- Jolt glue ------------------------------------------------------------------------------

JPH::Vec3 to_jolt(Vec3 v) {
    return {v.x, v.y, v.z};
}
JPH::Quat to_jolt(Quat q) {
    return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
}
Vec3 from_jolt(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
Quat from_jolt(JPH::QuatArg q) {
    return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

// Object layer = user layer * 2 + (1 if it can move).
JPH::ObjectLayer object_layer(int user_layer, bool moving) {
    return static_cast<JPH::ObjectLayer>(std::clamp(user_layer, 0, kPhysicsLayerCount - 1) * 2 + (moving ? 1 : 0));
}

namespace bp {
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
} // namespace bp

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return (layer & 1) != 0 ? bp::kMoving : bp::kStatic; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return layer == bp::kMoving ? "Moving" : "Static"; }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase) const override {
        return (layer & 1) != 0 || broad_phase == bp::kMoving; // static only meets moving
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectPairs(const PhysicsSettings& settings) : settings_(settings) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if ((a & 1) == 0 && (b & 1) == 0) {
            return false;
        }
        return settings_.collides(a >> 1, b >> 1);
    }

private:
    const PhysicsSettings& settings_;
};

// Jolt's global state (allocator, factory, type registry) is set up once
// and torn down with the last world.
std::mutex g_jolt_mutex;
int g_jolt_users = 0;

void jolt_trace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_info(std::string("Jolt: ") + buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool jolt_assert(const char* expression, const char* message, const char* file, JPH::uint line) {
    log_error(std::format("Jolt assert: {} {} ({}:{})", expression, message != nullptr ? message : "", file, line));
    return false; // log and continue instead of breaking into the debugger
}
#endif

void acquire_jolt() {
    std::lock_guard lock(g_jolt_mutex);
    if (g_jolt_users++ == 0) {
        JPH::Trace = jolt_trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert;)
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void release_jolt() {
    std::lock_guard lock(g_jolt_mutex);
    if (--g_jolt_users == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

std::uint64_t pair_key(JPH::BodyID a, JPH::BodyID b) {
    std::uint32_t x = a.GetIndexAndSequenceNumber();
    std::uint32_t y = b.GetIndexAndSequenceNumber();
    if (x > y) {
        std::swap(x, y);
    }
    return (static_cast<std::uint64_t>(x) << 32) | y;
}

float column_length(const Mat4& m, int column) {
    const auto c = static_cast<std::size_t>(column * 4);
    return std::sqrt(m.m[c] * m.m[c] + m.m[c + 1] * m.m[c + 1] + m.m[c + 2] * m.m[c + 2]);
}

bool active_in_hierarchy(const Scene& scene, const Actor& actor) {
    const Actor* current = &actor;
    while (current != nullptr) {
        if (!current->active) {
            return false;
        }
        current = current->parent() != kNoActor ? scene.find(current->parent()) : nullptr;
    }
    return true;
}

float max_axis(Vec3 v) {
    return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
}

// World transform split into position, rotation and (absolute) scale.
struct Pose {
    Vec3 position;
    Quat rotation;
    Vec3 scale;
};

Pose pose_of(const Mat4& world) {
    Pose p;
    p.position = world.translation_part();
    p.scale = {column_length(world, 0), column_length(world, 1), column_length(world, 2)};
    p.rotation = quat_from_matrix(world);
    return p;
}

bool nearly(Vec3 a, Vec3 b, float eps) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps && std::abs(a.z - b.z) <= eps;
}

bool nearly(Quat a, Quat b, float eps) {
    const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return d >= 1.0f - eps;
}

} // namespace

// --- Shapes ------------------------------------------------------------------------------------

PhysicsWorld::WorldShape PhysicsWorld::world_shape(const Actor& actor, const ColliderComponent& collider) {
    const Mat4& world = actor.world_matrix();
    const Pose pose = pose_of(world);
    WorldShape shape;
    shape.shape = collider.shape;
    shape.rotation = pose.rotation;
    shape.center = transform_point(world, collider.center);
    switch (collider.shape) {
    case ColliderShape::Sphere: {
        shape.radius = std::max(0.001f, collider.radius * max_axis(pose.scale));
        const Vec3 r{shape.radius, shape.radius, shape.radius};
        shape.bounds = {shape.center - r, shape.center + r};
        break;
    }
    case ColliderShape::Capsule: {
        shape.radius = std::max(0.001f, collider.radius * std::max(std::abs(pose.scale.x), std::abs(pose.scale.z)));
        shape.half_height = std::max(0.0f, collider.height * 0.5f * std::abs(pose.scale.y));
        const Vec3 axis = transform_vector(Mat4::rotation(pose.rotation), {0.0f, shape.half_height, 0.0f});
        const Vec3 r{shape.radius, shape.radius, shape.radius};
        shape.bounds = {component_min(shape.center - axis, shape.center + axis) - r, component_max(shape.center - axis, shape.center + axis) + r};
        break;
    }
    case ColliderShape::Mesh:
    case ColliderShape::Box: {
        Vec3 half = component_mul(collider.half_extents, pose.scale);
        half = {std::max(std::abs(half.x), 0.001f), std::max(std::abs(half.y), 0.001f), std::max(std::abs(half.z), 0.001f)};
        if (collider.shape == ColliderShape::Mesh) {
            if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
                const Aabb local = mesh->local_bounds();
                if (local.valid()) {
                    half = component_mul(local.extents(), pose.scale);
                    shape.center = transform_point(world, local.center());
                }
            }
        }
        shape.half_extents = half;
        // World AABB of the rotated box.
        const Mat4 rotation = Mat4::rotation(pose.rotation);
        Vec3 extent{};
        for (int axis = 0; axis < 3; ++axis) {
            const Vec3 basis = transform_vector(rotation, {axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f});
            const float h = axis == 0 ? half.x : (axis == 1 ? half.y : half.z);
            extent += Vec3{std::abs(basis.x), std::abs(basis.y), std::abs(basis.z)} * h;
        }
        shape.bounds = {shape.center - extent, shape.center + extent};
        break;
    }
    }
    return shape;
}

PhysicsWorld::WorldShape PhysicsWorld::world_shape(const Actor& actor, const CharacterControllerComponent& character) {
    const Mat4& world = actor.world_matrix();
    WorldShape shape;
    shape.shape = ColliderShape::Capsule;
    shape.center = world.translation_part() + character.center; // characters stay upright
    shape.radius = character.radius;
    shape.half_height = character.height * 0.5f;
    const Vec3 r{shape.radius, shape.radius + shape.half_height, shape.radius};
    shape.bounds = {shape.center - r, shape.center + r};
    return shape;
}

// --- Impl --------------------------------------------------------------------------------------

struct PhysicsWorld::Impl final : public JPH::ContactListener {
    explicit Impl(const PhysicsSettings& s) : pair_filter(s) {
        acquire_jolt();
        temp = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(32 * 1024 * 1024);
        const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 8);
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
        system = std::make_unique<JPH::PhysicsSystem>();
        system->Init(16384, 0, 32768, 16384, broad_phase, object_vs_bp, pair_filter);
        system->SetContactListener(this);
    }

    ~Impl() override {
        clear();
        system.reset();
        jobs.reset();
        temp.reset();
        release_jolt();
    }

    struct BodyRecord {
        JPH::BodyID id;
        std::uint64_t key = 0; // what the body was built from (rebuild on change)
        bool dynamic = false;
        bool kinematic = false;
        bool trigger = false;
        Pose pose;             // last pose written to / read from the actor
        Vec3 velocity{};       // last velocity read back (to detect script writes)
        Vec3 angular_velocity{};
        std::uint64_t seen = 0;
    };

    struct CharacterRecord {
        JPH::Ref<JPH::CharacterVirtual> character;
        std::uint64_t key = 0;
        std::uint64_t seen = 0;
        std::set<ActorId> touching;
    };

    struct PairState {
        ActorId a = kNoActor;
        ActorId b = kNoActor;
        JPH::BodyID id1;
        JPH::BodyID id2;
        int count = 0;       // touching sub-shape pairs
        bool sleeping = false; // both bodies asleep while touching
        bool trigger = false;
        Vec3 normal{};
        float depth = 0.0f;
    };

    // ContactListener (called from Jolt's job threads during Update).
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        record_contact(body1, body2, manifold, true);
    }
    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        record_contact(body1, body2, manifold, false);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        std::lock_guard lock(contact_mutex);
        const std::uint64_t key = pair_key(pair.GetBody1ID(), pair.GetBody2ID());
        auto it = pairs.find(key);
        if (it == pairs.end()) {
            return;
        }
        // Jolt also removes contacts when bodies fall asleep; whether this
        // is a real separation is decided after the step (see settle_pairs).
        it->second.count = std::max(it->second.count - 1, 0);
    }

    // After a step: pairs with no touching sub-shapes either went to sleep
    // (keep them, still touching) or separated (exit event).
    void settle_pairs() {
        const JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
        std::lock_guard lock(contact_mutex);
        for (auto it = pairs.begin(); it != pairs.end();) {
            PairState& state = it->second;
            if (state.count > 0) {
                state.sleeping = false;
                ++it;
                continue;
            }
            const bool awake = bodies.IsActive(state.id1) || bodies.IsActive(state.id2);
            if (!awake && bodies.IsAdded(state.id1) && bodies.IsAdded(state.id2)) {
                state.sleeping = true;
                ++it;
            } else {
                pending_events.push_back({state.a, state.b, false, state.trigger});
                it = pairs.erase(it);
            }
        }
    }

    void record_contact(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, bool added) {
        const auto a = static_cast<ActorId>(body1.GetUserData());
        const auto b = static_cast<ActorId>(body2.GetUserData());
        if (a == kNoActor || b == kNoActor || a == b) {
            return;
        }
        const bool trigger = body1.IsSensor() || body2.IsSensor();
        // Triggers only report bodies that move (not the ground they sit on).
        if (trigger && (body1.IsStatic() || body2.IsStatic())) {
            return;
        }
        std::lock_guard lock(contact_mutex);
        PairState& state = pairs[pair_key(body1.GetID(), body2.GetID())];
        if (added) {
            if (state.count == 0 && !state.sleeping) {
                state.a = a;
                state.b = b;
                state.id1 = body1.GetID();
                state.id2 = body2.GetID();
                state.trigger = trigger;
                pending_events.push_back({a, b, true, trigger});
            }
            state.sleeping = false;
            ++state.count;
        }
        const bool swapped = state.a != a;
        const Vec3 n = from_jolt(manifold.mWorldSpaceNormal);
        state.normal = swapped ? -n : n;
        state.depth = manifold.mPenetrationDepth;
    }

    void clear() {
        JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
        for (auto& [actor, record] : body_records) {
            bodies.RemoveBody(record.id);
            bodies.DestroyBody(record.id);
        }
        body_records.clear();
        character_records.clear();
        std::lock_guard lock(contact_mutex);
        pairs.clear();
        pending_events.clear();
    }

    void remove_body(ActorId actor) {
        auto it = body_records.find(actor);
        if (it == body_records.end()) {
            return;
        }
        JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
        bodies.RemoveBody(it->second.id);
        bodies.DestroyBody(it->second.id);
        body_records.erase(it);
        purge_pairs(actor);
    }

    // Jolt doesn't always report removals for bodies that vanish: end their
    // contacts ourselves.
    void purge_pairs(ActorId actor) {
        std::lock_guard lock(contact_mutex);
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (it->second.a == actor || it->second.b == actor) {
                pending_events.push_back({it->second.a, it->second.b, false, it->second.trigger});
                it = pairs.erase(it);
            } else {
                ++it;
            }
        }
    }

    JPH::RefConst<JPH::Shape> build_shape(const Actor& actor, const ColliderComponent& collider, bool dynamic, const Pose& pose) const {
        JPH::RefConst<JPH::Shape> shape;
        Vec3 offset = component_mul(collider.center, pose.scale);
        switch (collider.shape) {
        case ColliderShape::Sphere:
            shape = new JPH::SphereShape(std::max(0.001f, collider.radius * max_axis(pose.scale)));
            break;
        case ColliderShape::Capsule: {
            const float radius = std::max(0.001f, collider.radius * std::max(std::abs(pose.scale.x), std::abs(pose.scale.z)));
            const float half = collider.height * 0.5f * std::abs(pose.scale.y);
            if (half > 0.001f) {
                shape = new JPH::CapsuleShape(half, radius);
            } else {
                shape = new JPH::SphereShape(radius);
            }
            break;
        }
        case ColliderShape::Mesh: {
            const auto* mesh = actor.find_component<StaticMeshComponent>();
            if (mesh != nullptr) {
                if (dynamic) {
                    JPH::Array<JPH::Vec3> points;
                    for (const MeshPart& part : mesh->parts) {
                        if (!part.mesh) continue;
                        const std::size_t count = part.mesh->vertices.size();
                        const std::size_t stride = std::max<std::size_t>(1, count / 1024); // a hull doesn't need every vertex
                        for (std::size_t i = 0; i < count; i += stride) {
                            points.push_back(to_jolt(component_mul(part.mesh->vertices[i].position, pose.scale)));
                        }
                    }
                    if (points.size() >= 4) {
                        JPH::ConvexHullShapeSettings settings(points);
                        JPH::Shape::ShapeResult result = settings.Create();
                        if (result.IsValid()) {
                            shape = result.Get();
                        }
                    }
                } else {
                    JPH::TriangleList triangles;
                    for (const MeshPart& part : mesh->parts) {
                        if (!part.mesh) continue;
                        const MeshAsset& m = *part.mesh;
                        const std::size_t index_count = m.lods.empty() ? m.indices.size() : m.lods[0].index_count;
                        for (std::size_t i = 0; i + 2 < index_count; i += 3) {
                            const Vec3 p0 = component_mul(m.vertices[m.indices[i]].position, pose.scale);
                            const Vec3 p1 = component_mul(m.vertices[m.indices[i + 1]].position, pose.scale);
                            const Vec3 p2 = component_mul(m.vertices[m.indices[i + 2]].position, pose.scale);
                            triangles.push_back(JPH::Triangle(JPH::Float3(p0.x, p0.y, p0.z), JPH::Float3(p1.x, p1.y, p1.z), JPH::Float3(p2.x, p2.y, p2.z)));
                        }
                    }
                    if (!triangles.empty()) {
                        JPH::MeshShapeSettings settings(triangles);
                        JPH::Shape::ShapeResult result = settings.Create();
                        if (result.IsValid()) {
                            shape = result.Get();
                        }
                    }
                }
            }
            offset = {};
            if (shape == nullptr) {
                log_warning(std::format("'{}': mesh collider has no usable mesh; using a box", actor.name()));
            } else {
                break;
            }
            [[fallthrough]];
        }
        case ColliderShape::Box: {
            Vec3 half = component_mul(collider.half_extents, pose.scale);
            half = {std::max(std::abs(half.x), 0.002f), std::max(std::abs(half.y), 0.002f), std::max(std::abs(half.z), 0.002f)};
            const float convex_radius = std::min({0.05f, half.x * 0.5f, half.y * 0.5f, half.z * 0.5f});
            shape = new JPH::BoxShape(to_jolt(half), convex_radius);
            break;
        }
        }
        if (length_squared(offset) > 1e-10f) {
            shape = new JPH::RotatedTranslatedShape(to_jolt(offset), JPH::Quat::sIdentity(), shape);
        }
        return shape;
    }

    static std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
        return (h ^ v) * 1099511628211ull;
    }
    static std::uint64_t quantize(float f) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(f * 1000.0f)));
    }

    std::uint64_t body_key(const Actor& actor, const ColliderComponent& collider, const RigidBodyComponent* body, const Pose& pose) const {
        std::uint64_t h = 1469598103934665603ull;
        h = mix(h, collider.revision);
        h = mix(h, static_cast<std::uint64_t>(collider.shape));
        h = mix(h, quantize(collider.radius));
        h = mix(h, quantize(collider.height));
        h = mix(h, quantize(collider.center.x) ^ (quantize(collider.center.y) << 20) ^ (quantize(collider.center.z) << 40));
        h = mix(h, quantize(collider.half_extents.x) ^ (quantize(collider.half_extents.y) << 20) ^ (quantize(collider.half_extents.z) << 40));
        h = mix(h, static_cast<std::uint64_t>(collider.layer) * 2 + (collider.is_trigger ? 1 : 0));
        h = mix(h, quantize(pose.scale.x) ^ (quantize(pose.scale.y) << 20) ^ (quantize(pose.scale.z) << 40));
        if (body != nullptr) {
            h = mix(h, body->revision + 0x9E3779B97F4A7C15ull);
            h = mix(h, quantize(body->mass) ^ (quantize(body->friction) << 24) ^ (quantize(body->restitution) << 44));
            h = mix(h, (body->is_kinematic ? 1u : 0u) | (body->use_gravity ? 2u : 0u) | (body->freeze_rotation ? 4u : 0u) | (body->continuous ? 8u : 0u));
            h = mix(h, quantize(body->linear_damping) ^ (quantize(body->angular_damping) << 24));
        }
        if (collider.shape == ColliderShape::Mesh) {
            if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
                for (const MeshPart& part : mesh->parts) {
                    h = mix(h, part.mesh ? part.mesh->id : 0);
                }
            }
        }
        return h;
    }

    void create_body(const Actor& actor, const ColliderComponent& collider, const RigidBodyComponent* body, const Pose& pose, std::uint64_t key) {
        const bool dynamic = body != nullptr && !body->is_kinematic && !collider.is_trigger;
        const bool kinematic = (body != nullptr && !dynamic) || collider.is_trigger;
        JPH::RefConst<JPH::Shape> shape = build_shape(actor, collider, dynamic, pose);
        if (shape == nullptr) {
            return;
        }
        const JPH::EMotionType motion = dynamic ? JPH::EMotionType::Dynamic : (kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static);
        JPH::BodyCreationSettings settings(shape, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z), to_jolt(pose.rotation), motion,
            object_layer(collider.layer, motion != JPH::EMotionType::Static));
        settings.mUserData = actor.id();
        settings.mIsSensor = collider.is_trigger;
        if (collider.is_trigger) {
            settings.mCollideKinematicVsNonDynamic = true;
        }
        if (body != nullptr) {
            settings.mFriction = body->friction;
            settings.mRestitution = body->restitution;
            settings.mLinearDamping = body->linear_damping;
            settings.mAngularDamping = body->angular_damping;
            settings.mGravityFactor = body->use_gravity ? 1.0f : 0.0f;
            settings.mMotionQuality = body->continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            if (body->freeze_rotation && dynamic) {
                settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
            }
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = std::max(body->mass, 0.001f);
            settings.mLinearVelocity = to_jolt(body->velocity);
            settings.mAngularVelocity = to_jolt(body->angular_velocity);
        } else {
            settings.mFriction = 0.6f;
        }
        JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
        const JPH::BodyID id = bodies.CreateAndAddBody(settings, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (id.IsInvalid()) {
            log_warning(std::format("Physics: too many bodies, '{}' skipped", actor.name()));
            return;
        }
        BodyRecord record;
        record.id = id;
        record.key = key;
        record.dynamic = dynamic;
        record.kinematic = kinematic;
        record.trigger = collider.is_trigger;
        record.pose = pose;
        if (body != nullptr) {
            record.velocity = body->velocity;
            record.angular_velocity = body->angular_velocity;
        }
        body_records[actor.id()] = record;
    }

    std::uint64_t terrain_key(const TerrainComponent& t, const Pose& pose) const {
        std::uint64_t h = 1469598103934665603ull;
        h = mix(h, t.revision);
        for (float f : {t.size, t.height, t.frequency, t.ridges, t.flatten, t.basin_depth, t.basin_radius, t.basin_x, t.basin_z}) {
            h = mix(h, quantize(f));
        }
        h = mix(h, static_cast<std::uint64_t>(t.seed) * 31 + static_cast<std::uint64_t>(t.octaves));
        h = mix(h, quantize(pose.scale.x) ^ (quantize(pose.scale.y) << 20) ^ (quantize(pose.scale.z) << 40));
        return h;
    }

    // Terrain: a static height field sampled from the same function the
    // renderer meshes (256 x 256 samples).
    void create_terrain_body(const Actor& actor, const TerrainComponent& terrain, const Pose& pose, std::uint64_t key) {
        constexpr std::uint32_t n = 256;
        const float size = std::max(terrain.size, 1.0f);
        const float step = size / static_cast<float>(n - 1);
        JPH::Array<float> samples(static_cast<std::size_t>(n) * n);
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                samples[z * n + x] = terrain.sample_height(-size * 0.5f + static_cast<float>(x) * step, -size * 0.5f + static_cast<float>(z) * step);
            }
        }
        const Vec3 scale = pose.scale;
        JPH::HeightFieldShapeSettings settings(samples.data(), JPH::Vec3(-size * 0.5f * scale.x, 0.0f, -size * 0.5f * scale.z),
            JPH::Vec3(step * scale.x, scale.y, step * scale.z), n);
        JPH::Shape::ShapeResult result = settings.Create();
        if (!result.IsValid()) {
            log_warning(std::format("'{}': terrain collision failed: {}", actor.name(), result.GetError().c_str()));
            return;
        }
        JPH::BodyCreationSettings body(result.Get(), JPH::RVec3(pose.position.x, pose.position.y, pose.position.z), to_jolt(pose.rotation),
            JPH::EMotionType::Static, object_layer(0, false));
        body.mUserData = actor.id();
        body.mFriction = 0.8f;
        const JPH::BodyID id = system->GetBodyInterfaceNoLock().CreateAndAddBody(body, JPH::EActivation::DontActivate);
        if (id.IsInvalid()) {
            return;
        }
        BodyRecord record;
        record.id = id;
        record.key = key;
        record.pose = pose;
        body_records[actor.id()] = record;
    }

    std::uint64_t character_key(const CharacterControllerComponent& c) const {
        std::uint64_t h = 1469598103934665603ull;
        h = mix(h, c.revision);
        h = mix(h, quantize(c.radius) ^ (quantize(c.height) << 24));
        h = mix(h, quantize(c.center.x) ^ (quantize(c.center.y) << 20) ^ (quantize(c.center.z) << 40));
        h = mix(h, quantize(c.max_slope) ^ (static_cast<std::uint64_t>(c.layer) << 40));
        return h;
    }

    void create_character(const Actor& actor, const CharacterControllerComponent& c, std::uint64_t key) {
        const float half = std::max(c.height * 0.5f, 0.01f);
        const float radius = std::max(c.radius, 0.02f);
        JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(half, radius);
        JPH::RefConst<JPH::Shape> shape = new JPH::RotatedTranslatedShape(to_jolt(c.center), JPH::Quat::sIdentity(), capsule);
        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = shape;
        settings->mInnerBodyShape = shape; // lets triggers and raycasts see the character
        settings->mInnerBodyLayer = object_layer(c.layer, true);
        settings->mMaxSlopeAngle = std::clamp(c.max_slope, 0.0f, 1.55f);
        // Contacts below the lower hemisphere's center can support the character.
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -(c.center.y - half));
        settings->mMass = 70.0f;
        settings->mMaxStrength = 100.0f * std::max(c.push_strength, 0.0f);
        settings->mEnhancedInternalEdgeRemoval = true;
        const Vec3 p = actor.world_position();
        CharacterRecord record;
        record.character = new JPH::CharacterVirtual(settings, JPH::RVec3(p.x, p.y, p.z), JPH::Quat::sIdentity(), actor.id(), system.get());
        record.key = key;
        character_records[actor.id()] = std::move(record);
    }

    std::unique_ptr<JPH::TempAllocatorImplWithMallocFallback> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BroadPhaseLayers broad_phase;
    ObjectVsBroadPhase object_vs_bp;
    ObjectPairs pair_filter;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::unordered_map<ActorId, BodyRecord> body_records;
    std::unordered_map<ActorId, CharacterRecord> character_records;
    std::uint64_t frame = 0;

    std::mutex contact_mutex;
    std::map<std::uint64_t, PairState> pairs;
    std::vector<ContactEvent> pending_events;
};

// --- PhysicsWorld ---------------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>(settings)) {}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::reset() {
    impl_->clear();
    contacts_.clear();
    events_.clear();
    stats_ = {};
}

namespace {

// Writes a world pose (keeping the world scale) into the actor's local transform.
void set_world_pose(Scene& scene, Actor& actor, Vec3 position, Quat rotation, Vec3 world_scale) {
    const Mat4 world = Mat4::translation(position) * Mat4::rotation(rotation) * Mat4::scale(world_scale);
    Mat4 local = world;
    if (actor.parent() != kNoActor) {
        local = inverse(scene.compute_parent_world_matrix(actor.id())) * world;
    }
    const Transform t = decompose(local);
    actor.transform.translation = t.translation;
    actor.transform.rotation = t.rotation;
}

} // namespace

void PhysicsWorld::step(Scene& scene, float dt) {
    Impl& impl = *impl_;
    ++impl.frame;
    impl.system->SetGravity(to_jolt(settings.gravity));
    JPH::BodyInterface& bodies = impl.system->GetBodyInterfaceNoLock();
    scene.update_world_transforms();

    // 1. Scene -> physics.
    for (const auto& actor_ptr : scene.actors()) {
        Actor& actor = *actor_ptr;
        if (!active_in_hierarchy(scene, actor)) {
            continue;
        }
        if (auto* character = actor.find_component<CharacterControllerComponent>()) {
            const std::uint64_t key = impl.character_key(*character);
            auto it = impl.character_records.find(actor.id());
            if (it == impl.character_records.end() || it->second.key != key) {
                impl.create_character(actor, *character, key);
                it = impl.character_records.find(actor.id());
            }
            it->second.seen = impl.frame;
            continue; // a character's own collider (if any) is ignored
        }
        auto* collider = actor.find_component<ColliderComponent>();
        if (collider == nullptr) {
            if (const auto* terrain = actor.find_component<TerrainComponent>()) {
                const Pose pose = pose_of(actor.world_matrix());
                const std::uint64_t key = impl.terrain_key(*terrain, pose);
                auto it = impl.body_records.find(actor.id());
                if (it == impl.body_records.end() || it->second.key != key) {
                    impl.remove_body(actor.id());
                    impl.create_terrain_body(actor, *terrain, pose, key);
                    it = impl.body_records.find(actor.id());
                }
                if (it != impl.body_records.end()) {
                    it->second.seen = impl.frame;
                    if (!nearly(pose.position, it->second.pose.position, 1e-5f) || !nearly(pose.rotation, it->second.pose.rotation, 1e-6f)) {
                        bodies.SetPositionAndRotation(it->second.id, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z), to_jolt(pose.rotation),
                            JPH::EActivation::DontActivate);
                        it->second.pose = pose;
                    }
                }
            }
            continue;
        }
        auto* body = actor.find_component<RigidBodyComponent>();
        const Pose pose = pose_of(actor.world_matrix());
        const std::uint64_t key = impl.body_key(actor, *collider, body, pose);
        auto it = impl.body_records.find(actor.id());
        if (it == impl.body_records.end() || it->second.key != key) {
            if (it != impl.body_records.end()) {
                impl.remove_body(actor.id());
            }
            impl.create_body(actor, *collider, body, pose, key);
            it = impl.body_records.find(actor.id());
            if (it == impl.body_records.end()) {
                continue;
            }
        }
        Impl::BodyRecord& record = it->second;
        record.seen = impl.frame;
        const bool moved = !nearly(pose.position, record.pose.position, 1e-5f) || !nearly(pose.rotation, record.pose.rotation, 1e-6f);
        if (record.kinematic) {
            if (moved) {
                bodies.MoveKinematic(record.id, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z), to_jolt(pose.rotation), dt);
            } else if (bodies.IsActive(record.id)) {
                bodies.SetLinearAndAngularVelocity(record.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            }
        } else if (moved) {
            // Teleported by a script or the editor.
            bodies.SetPositionAndRotation(record.id, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z), to_jolt(pose.rotation),
                record.dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        }
        record.pose = pose;
        if (record.dynamic && body != nullptr) {
            if (!nearly(body->velocity, record.velocity, 1e-5f)) {
                bodies.SetLinearVelocity(record.id, to_jolt(body->velocity));
            }
            if (!nearly(body->angular_velocity, record.angular_velocity, 1e-5f)) {
                bodies.SetAngularVelocity(record.id, to_jolt(body->angular_velocity));
            }
            if (length_squared(body->pending_impulse) > 0.0f) {
                bodies.AddImpulse(record.id, to_jolt(body->pending_impulse));
                body->pending_impulse = {};
            }
        }
    }
    // Remove what's gone (deleted actors, removed components, inactive).
    std::vector<ActorId> stale;
    for (const auto& [actor, record] : impl.body_records) {
        if (record.seen != impl.frame) stale.push_back(actor);
    }
    for (ActorId actor : stale) {
        impl.remove_body(actor);
    }
    for (auto it = impl.character_records.begin(); it != impl.character_records.end();) {
        if (it->second.seen != impl.frame) {
            impl.purge_pairs(it->first);
            it = impl.character_records.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Characters move first (they push bodies through their contacts).
    const JPH::Vec3 gravity = to_jolt(settings.gravity);
    std::vector<ContactEvent> character_events;
    for (auto& [actor_id, record] : impl.character_records) {
        Actor* actor = scene.find(actor_id);
        auto* c = actor != nullptr ? actor->find_component<CharacterControllerComponent>() : nullptr;
        if (c == nullptr) {
            continue;
        }
        JPH::CharacterVirtual& ch = *record.character;
        // Follow editor/script teleports.
        const Vec3 wanted = actor->world_position();
        const Vec3 current = from_jolt(JPH::Vec3(ch.GetPosition()));
        if (!nearly(wanted, current, 1e-4f)) {
            ch.SetPosition(JPH::RVec3(wanted.x, wanted.y, wanted.z));
        }
        Vec3 velocity = c->velocity;
        const bool on_ground = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        if (on_ground && velocity.y <= 0.0f) {
            velocity.y = 0.0f;
            velocity += from_jolt(ch.GetGroundVelocity());
        } else {
            velocity += settings.gravity * (c->gravity_scale * dt);
        }
        ch.SetLinearVelocity(to_jolt(velocity));
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, c->step_height, 0.0f);
        update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(c->step_height, 0.1f), 0.0f);
        const JPH::ObjectLayer layer = object_layer(c->layer, true);
        ch.ExtendedUpdate(dt, gravity * c->gravity_scale, update, impl.system->GetDefaultBroadPhaseLayerFilter(layer),
            impl.system->GetDefaultLayerFilter(layer), {}, {}, *impl.temp);
        const Vec3 position = from_jolt(JPH::Vec3(ch.GetPosition()));
        scene.set_world_position(actor_id, position);
        Vec3 result = from_jolt(ch.GetLinearVelocity());
        c->grounded = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        c->ground_normal = c->grounded ? from_jolt(ch.GetGroundNormal()) : Vec3{0.0f, 1.0f, 0.0f};
        if (c->grounded && result.y < 0.0f) {
            result.y = 0.0f;
        }
        c->velocity = {c->velocity.x, result.y, c->velocity.z};

        // Contacts -> enter/exit events (characters aren't in the contact listener).
        std::set<ActorId> touching;
        for (const JPH::CharacterVirtual::Contact& contact : ch.GetActiveContacts()) {
            if (!contact.mHadCollision || contact.mBodyB.IsInvalid()) {
                continue;
            }
            const auto other = static_cast<ActorId>(bodies.GetUserData(contact.mBodyB));
            if (other != kNoActor && other != actor_id) {
                touching.insert(other);
            }
        }
        for (ActorId other : touching) {
            if (!record.touching.contains(other)) character_events.push_back({actor_id, other, true, false});
        }
        for (ActorId other : record.touching) {
            if (!touching.contains(other)) character_events.push_back({actor_id, other, false, false});
        }
        record.touching = std::move(touching);
    }

    // 3. Simulate.
    const JPH::EPhysicsUpdateError error = impl.system->Update(dt, 1, impl.temp.get(), impl.jobs.get());
    if (error != JPH::EPhysicsUpdateError::None) {
        log_warning("Physics: the step ran out of contact/pair capacity; some collisions were skipped");
    }
    impl.settle_pairs();

    // 4. Physics -> scene.
    stats_ = {};
    for (auto& [actor_id, record] : impl.body_records) {
        ++stats_.bodies;
        if (!record.dynamic) {
            continue;
        }
        ++stats_.dynamic_bodies;
        Actor* actor = scene.find(actor_id);
        if (actor == nullptr) {
            continue;
        }
        auto* body = actor->find_component<RigidBodyComponent>();
        if (!bodies.IsActive(record.id)) {
            if (body != nullptr) {
                body->velocity = {};
                body->angular_velocity = {};
                record.velocity = {};
                record.angular_velocity = {};
            }
            continue;
        }
        ++stats_.active_bodies;
        JPH::RVec3 position;
        JPH::Quat rotation;
        bodies.GetPositionAndRotation(record.id, position, rotation);
        const Vec3 p{static_cast<float>(position.GetX()), static_cast<float>(position.GetY()), static_cast<float>(position.GetZ())};
        const Quat q = from_jolt(rotation);
        set_world_pose(scene, *actor, p, q, record.pose.scale);
        record.pose.position = p;
        record.pose.rotation = q;
        if (body != nullptr) {
            body->velocity = from_jolt(bodies.GetLinearVelocity(record.id));
            body->angular_velocity = from_jolt(bodies.GetAngularVelocity(record.id));
            record.velocity = body->velocity;
            record.angular_velocity = body->angular_velocity;
        }
    }
    stats_.characters = static_cast<int>(impl.character_records.size());
    scene.update_world_transforms();
    // update_world_transforms may have nudged poses through parents; keep
    // the records in sync so we don't mistake that for a teleport.
    for (auto& [actor_id, record] : impl.body_records) {
        if (const Actor* actor = scene.find(actor_id)) {
            const Pose pose = pose_of(actor->world_matrix());
            record.pose.position = pose.position;
            record.pose.rotation = pose.rotation;
        }
    }

    // 5. Contacts and events.
    contacts_.clear();
    events_.clear();
    {
        std::lock_guard lock(impl.contact_mutex);
        for (const auto& [key, state] : impl.pairs) {
            contacts_.push_back({state.a, state.b, state.normal, state.depth, state.trigger});
        }
        events_ = std::move(impl.pending_events);
        impl.pending_events.clear();
        // Grounded: touching something that pushes up on it (the pair map
        // keeps contacts of sleeping bodies, so resting bodies stay grounded).
        std::set<ActorId> grounded;
        for (const auto& [key, state] : impl.pairs) {
            if (state.trigger) continue;
            if (state.normal.y > 0.5f) grounded.insert(state.b); // a pushes b up
            if (state.normal.y < -0.5f) grounded.insert(state.a);
        }
        for (auto& [actor_id, record] : impl.body_records) {
            if (!record.dynamic) continue;
            if (Actor* actor = scene.find(actor_id)) {
                if (auto* body = actor->find_component<RigidBodyComponent>()) {
                    body->grounded = grounded.contains(actor_id);
                }
            }
        }
    }
    for (const auto& [actor_id, record] : impl.character_records) {
        for (ActorId other : record.touching) {
            contacts_.push_back({actor_id, other, {}, 0.0f, false});
        }
    }
    events_.insert(events_.end(), character_events.begin(), character_events.end());
    stats_.contacts = static_cast<int>(contacts_.size());
}

namespace {

class SkipFilter final : public JPH::BodyFilter {
public:
    explicit SkipFilter(std::vector<JPH::BodyID> skip) : skip_(std::move(skip)) {}
    bool ShouldCollide(const JPH::BodyID& id) const override {
        return std::find(skip_.begin(), skip_.end(), id) == skip_.end();
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return !body.IsSensor();
    }

private:
    std::vector<JPH::BodyID> skip_;
};

} // namespace

bool PhysicsWorld::raycast(const Scene&, const Ray& ray, float max_distance, RaycastHit& hit, ActorId ignore) const {
    const Impl& impl = *impl_;
    std::vector<JPH::BodyID> skip;
    if (ignore != kNoActor) {
        if (auto it = impl.body_records.find(ignore); it != impl.body_records.end()) {
            skip.push_back(it->second.id);
        }
        if (auto it = impl.character_records.find(ignore); it != impl.character_records.end()) {
            skip.push_back(it->second.character->GetInnerBodyID());
        }
    }
    const Vec3 direction = normalize(ray.direction) * max_distance;
    const JPH::RRayCast cast{JPH::RVec3(ray.origin.x, ray.origin.y, ray.origin.z), to_jolt(direction)};
    JPH::RayCastResult result;
    SkipFilter filter(std::move(skip));
    if (!impl.system->GetNarrowPhaseQuery().CastRay(cast, result, {}, {}, filter)) {
        return false;
    }
    const JPH::RVec3 point = cast.GetPointOnRay(result.mFraction);
    hit.point = {static_cast<float>(point.GetX()), static_cast<float>(point.GetY()), static_cast<float>(point.GetZ())};
    hit.distance = result.mFraction * max_distance;
    JPH::BodyLockRead lock(impl.system->GetBodyLockInterfaceNoLock(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.actor = static_cast<ActorId>(body.GetUserData());
        hit.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    }
    return true;
}

std::vector<ActorId> PhysicsWorld::overlap_sphere(Vec3 center, float radius) const {
    const Impl& impl = *impl_;
    JPH::SphereShape sphere(std::max(radius, 0.001f));
    sphere.SetEmbedded();
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl.system->GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(center.x, center.y, center.z)),
        settings, JPH::RVec3::sZero(), collector);
    std::vector<ActorId> actors;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const auto actor = static_cast<ActorId>(impl.system->GetBodyInterfaceNoLock().GetUserData(r.mBodyID2));
        if (actor != kNoActor && std::find(actors.begin(), actors.end(), actor) == actors.end()) {
            actors.push_back(actor);
        }
    }
    return actors;
}

} // namespace Hollow

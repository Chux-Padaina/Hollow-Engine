// Scene graph: Actors arranged in a parent/child hierarchy, each owning a
// local Transform and a list of Components. Components are plain data; the
// systems that act on them (Renderer, PhysicsWorld, ScriptSystem) live in
// their own modules and walk the scene each frame.
//
// Actor ids are stable for an actor's lifetime and survive save/load and
// undo/redo, so systems and the editor refer to actors by ActorId rather
// than by pointer.
#pragma once

#include "Hollow/Assets/Animation.hpp"
#include "Hollow/Assets/MeshAsset.hpp"
#include "Hollow/Core/Math.hpp"
#include "Hollow/Core/Properties.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Hollow {

class Script;

using ActorId = std::uint32_t;
constexpr ActorId kNoActor = 0;

enum class ComponentType {
    StaticMesh,
    Light,
    Camera,
    RigidBody,
    Collider,
    Script,
    ReflectionProbe,
    IrradianceVolume,
    Decal,
    ParticleEmitter,
    Terrain,
    Water,
    Animator,
    CharacterController,
    Count,
};

const char* component_type_name(ComponentType type);
// Stable key used in scene files ("StaticMesh", "Light", ...).
const char* component_type_key(ComponentType type);
bool component_type_from_key(const std::string& key, ComponentType& type);
// Script components may be stacked; every other type is one-per-actor.
bool component_allows_multiple(ComponentType type);

class Component {
public:
    virtual ~Component() = default;
    virtual ComponentType type() const = 0;
    virtual std::unique_ptr<Component> clone() const = 0;
    // Editable/saved fields (see Core/Properties.hpp). StaticMesh and Script
    // components have custom serialization and inspectors instead.
    virtual void describe(PropertyVisitor& v) { (void)v; }
    // Bumped by the editor when a property changes (lets systems rebuild
    // derived data, e.g. terrain meshes or baked probes).
    std::uint32_t revision = 0;
};

// Creates a default component of `type` (nullptr for Script/StaticMesh,
// which the caller sets up).
std::unique_ptr<Component> create_component(ComponentType type);

template <typename Derived, ComponentType Type>
class ComponentBase : public Component {
public:
    static constexpr ComponentType kType = Type;
    ComponentType type() const override { return Type; }
    std::unique_ptr<Component> clone() const override {
        return std::make_unique<Derived>(static_cast<const Derived&>(*this));
    }
};

// --- Rendering ---------------------------------------------------------

struct MeshPart {
    std::string name;
    MeshHandle mesh;
    Material material;
};

class StaticMeshComponent final : public ComponentBase<StaticMeshComponent, ComponentType::StaticMesh> {
public:
    std::vector<MeshPart> parts;
    bool visible = true;
    bool cast_shadows = true;

    // Union of every part's local bounds (actor space).
    Aabb local_bounds() const;
};

enum class LightKind {
    Directional,
    Point,
    Spot,
};

const char* light_kind_name(LightKind kind);

// Physical light. Directional lights are measured in lux (illuminance, a
// clear-sky sun is ~100,000 lx), point and spot lights in lumens (luminous
// power: a 60 W incandescent bulb is ~800 lm). A spot light keeps a point
// light's intensity per direction, so narrowing the cone doesn't brighten
// it.
class LightComponent final : public ComponentBase<LightComponent, ComponentType::Light> {
public:
    LightKind light_kind = LightKind::Point;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 800.0f;
    bool use_temperature = false;
    float temperature = 6500.0f; // Kelvin, multiplies color
    float range = 8.0f;          // attenuation radius (point/spot)
    float spot_angle_radians = 0.55f; // full cone angle
    float spot_softness = 0.2f;  // fraction of the cone that fades
    // Physical size for soft shadows: radius in meters (point/spot) or the
    // angular diameter in degrees (directional; the sun is ~0.53).
    float source_size = 0.05f;
    float volumetric = 1.0f; // contribution to volumetric fog
    bool enabled = true;
    bool cast_shadows = true;

    void apply_defaults_for_kind();
    void describe(PropertyVisitor& v) override;
};

// A camera the game view renders through in Play mode. Looks down the
// actor's local -Z axis.
class CameraComponent final : public ComponentBase<CameraComponent, ComponentType::Camera> {
public:
    float fov_y_radians = 1.05f;
    float near_plane = 0.05f;
    bool primary = true;

    void describe(PropertyVisitor& v) override;
};

// A box-projected reflection probe: the renderer captures a cubemap from
// the actor's position and uses it for reflections inside the box.
class ReflectionProbeComponent final : public ComponentBase<ReflectionProbeComponent, ComponentType::ReflectionProbe> {
public:
    Vec3 extents{6.0f, 3.0f, 6.0f}; // half size (actor scale ignored)
    float blend_distance = 0.75f;
    float intensity = 1.0f;
    bool box_projection = true;
    int resolution = 128;

    void describe(PropertyVisitor& v) override;
};

// A grid of baked irradiance probes (L1 spherical harmonics) for diffuse
// global illumination inside a box.
class IrradianceVolumeComponent final : public ComponentBase<IrradianceVolumeComponent, ComponentType::IrradianceVolume> {
public:
    Vec3 extents{8.0f, 3.0f, 8.0f};
    int probes_x = 8;
    int probes_y = 4;
    int probes_z = 8;
    float blend_distance = 1.0f;
    float intensity = 1.0f;
    float normal_bias = 0.25f;
    int bounces = 2;

    void describe(PropertyVisitor& v) override;
};

// Projects a texture onto surfaces inside the actor's unit cube (scaled by
// the actor transform), along its local -Y axis.
class DecalComponent final : public ComponentBase<DecalComponent, ComponentType::Decal> {
public:
    std::string albedo_texture_path; // RGBA; alpha masks the decal
    std::string normal_texture_path;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    float normal_strength = 1.0f;
    bool override_roughness = false;
    float roughness = 0.5f;
    float angle_fade = 0.3f; // surfaces facing away beyond this fade out

    void describe(PropertyVisitor& v) override;
};

enum class EmitterShape {
    Point,
    Sphere,
    Box,
    Cone,
};

enum class ParticleBlend {
    Additive,
    Alpha,
};

// GPU-simulated particles (compute shader), drawn as camera-facing
// billboards with soft depth fade.
class ParticleEmitterComponent final : public ComponentBase<ParticleEmitterComponent, ComponentType::ParticleEmitter> {
public:
    bool emitting = true;
    int max_particles = 4096;
    float rate = 200.0f; // particles per second
    float lifetime = 2.0f;
    float lifetime_variance = 0.5f;
    EmitterShape shape = EmitterShape::Cone;
    Vec3 shape_size{0.2f, 0.2f, 0.2f};
    float cone_angle = 0.4f; // radians
    float speed = 2.0f;
    float speed_variance = 0.5f;
    float gravity = -0.5f; // multiplies 9.81, negative rises
    float drag = 0.5f;
    float size_start = 0.15f;
    float size_end = 0.6f;
    Vec3 color_start{1.0f, 0.6f, 0.2f};
    Vec3 color_end{0.3f, 0.3f, 0.3f};
    float alpha_start = 1.0f;
    float alpha_end = 0.0f;
    float emissive = 4.0f; // exposure-relative brightness multiplier
    ParticleBlend blend = ParticleBlend::Additive;
    bool world_space = true;
    float noise = 0.5f; // curl-ish turbulence strength
    std::string texture_path; // optional sprite (default: soft disc)

    void describe(PropertyVisitor& v) override;
};

// Procedural heightfield terrain (fBm noise) rendered in LOD'd chunks with
// four slope/height blended layers and instanced grass.
class TerrainComponent final : public ComponentBase<TerrainComponent, ComponentType::Terrain> {
public:
    float size = 128.0f;   // meters per side
    int resolution = 257;  // vertices per side
    float height = 18.0f;  // max height (m)
    int seed = 7;
    float frequency = 1.6f; // features per terrain width
    int octaves = 6;
    float ridges = 0.35f;   // 0 smooth hills .. 1 sharp ridges
    float flatten = 0.4f;   // flattens low areas
    // Optional basin (lake bed) carved into the heightfield, actor-local.
    float basin_depth = 0.0f;
    float basin_radius = 20.0f;
    float basin_x = 0.0f;
    float basin_z = 0.0f;
    Vec3 grass_color{0.24f, 0.33f, 0.12f};
    Vec3 rock_color{0.36f, 0.33f, 0.30f};
    Vec3 high_color{0.48f, 0.47f, 0.43f};
    Vec3 low_color{0.42f, 0.36f, 0.26f};
    float grass_density = 6.0f; // blades per square meter (0 = none)
    float grass_height = 0.45f;
    float grass_distance = 45.0f;
    float grass_min_height = 0.2f; // no grass below this (actor-local), e.g. under water
    float wind = 0.6f;

    void describe(PropertyVisitor& v) override;
    // Height (actor space) at a point given in actor-local x/z.
    float sample_height(float x, float z) const;
};

// A water surface: Gerstner waves, screen-space refraction and
// reflections, depth-based absorption and shoreline foam.
class WaterComponent final : public ComponentBase<WaterComponent, ComponentType::Water> {
public:
    float size_x = 60.0f;
    float size_z = 60.0f;
    float wave_height = 0.25f;
    float wavelength = 6.0f;
    float steepness = 0.45f;
    float wave_speed = 1.0f;
    float wind_direction = 0.6f; // radians
    Vec3 shallow_color{0.1f, 0.45f, 0.45f};
    Vec3 deep_color{0.01f, 0.06f, 0.1f};
    float clarity = 2.5f; // meters of visibility
    float foam = 0.6f;
    float roughness = 0.04f;

    void describe(PropertyVisitor& v) override;
};

// Plays a skeletal animation clip on the actor's skinned meshes.
class AnimatorComponent final : public ComponentBase<AnimatorComponent, ComponentType::Animator> {
public:
    std::string clip;      // clip name (empty = first clip)
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;
    bool preview_in_editor = true; // animate outside Play mode too
    float crossfade = 0.25f;       // seconds when switching clips

    // Runtime state (not saved).
    float time = 0.0f;
    std::string active_clip;
    float fade_time = 0.0f;
    std::string previous_clip;
    float previous_time = 0.0f;
    SkeletonHandle skeleton;            // resolved from the actor's meshes
    std::vector<Mat4> joint_matrices;   // model space, per skeleton joint
    std::uint64_t pose_frame = 0;

    void describe(PropertyVisitor& v) override;
};

// --- Physics -----------------------------------------------------------

// Physics (Jolt). Every actor with a ColliderComponent is in the physics
// world: with a RigidBodyComponent it's dynamic (or kinematic), without one
// it's static scenery. Collision layers 0-7 filter which bodies collide
// (PhysicsSettings::layer_mask).
class RigidBodyComponent final : public ComponentBase<RigidBodyComponent, ComponentType::RigidBody> {
public:
    float mass = 1.0f;
    bool use_gravity = true;
    // Kinematic bodies follow the actor transform (moved by scripts or
    // animation) and push dynamic bodies, but nothing pushes them.
    bool is_kinematic = false;
    float restitution = 0.1f;
    float friction = 0.6f;
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    bool freeze_rotation = false; // lock all rotation (e.g. a rolling-free player)
    bool continuous = false;      // swept collision for fast bodies

    // Runtime state (not saved). Scripts may write velocity / angular
    // velocity; the physics step reads them back in.
    Vec3 velocity{};
    Vec3 angular_velocity{};
    bool grounded = false;
    // Impulse to apply at the next step (N*s), consumed by the physics step.
    Vec3 pending_impulse{};

    void add_impulse(Vec3 impulse) { pending_impulse += impulse; }
    void describe(PropertyVisitor& v) override;
};

enum class ColliderShape {
    Box,
    Sphere,
    Capsule, // along the actor's local Y
    Mesh,    // the actor's StaticMesh triangles (static/kinematic), or its convex hull (dynamic)
};

class ColliderComponent final : public ComponentBase<ColliderComponent, ComponentType::Collider> {
public:
    ColliderShape shape = ColliderShape::Box;
    Vec3 center{};                     // actor-local offset
    Vec3 half_extents{0.5f, 0.5f, 0.5f}; // box, actor-local (scaled by the actor)
    float radius = 0.5f;               // sphere / capsule (scaled by the actor's largest horizontal axis)
    float height = 1.0f;               // capsule: length of the cylinder part
    // Triggers (sensors) report overlaps to scripts but never push anything.
    bool is_trigger = false;
    int layer = 0; // 0-7

    void describe(PropertyVisitor& v) override;
};

// A capsule character moved by velocity, sliding along walls, climbing
// steps and slopes up to max_slope, and pushing dynamic bodies. Scripts set
// `velocity` (horizontal movement, and y for jumps); gravity is applied
// while airborne.
class CharacterControllerComponent final : public ComponentBase<CharacterControllerComponent, ComponentType::CharacterController> {
public:
    float radius = 0.35f;
    float height = 1.1f;          // cylinder part; total height = height + 2 * radius
    Vec3 center{0.0f, 0.9f, 0.0f}; // capsule center, actor-local (feet at the actor origin)
    float max_slope = 0.87f;      // radians (~50 degrees)
    float step_height = 0.35f;
    float push_strength = 1.0f;   // how hard the character shoves dynamic bodies
    float gravity_scale = 1.0f;
    int layer = 0;

    // Runtime state (not saved).
    Vec3 velocity{};
    bool grounded = false;
    Vec3 ground_normal{0.0f, 1.0f, 0.0f};

    void describe(PropertyVisitor& v) override;
};

// --- Scripting ---------------------------------------------------------

struct ScriptParam {
    std::string name;
    float number = 0.0f;
    std::string text;
    bool is_text = false;
};

class ScriptComponent final : public Component {
public:
    static constexpr ComponentType kType = ComponentType::Script;

    ScriptComponent();
    ScriptComponent(const ScriptComponent& other);
    ScriptComponent& operator=(const ScriptComponent& other);
    ~ScriptComponent() override;

    ComponentType type() const override { return kType; }
    std::unique_ptr<Component> clone() const override;

    float number(const std::string& name, float fallback = 0.0f) const;
    std::string text(const std::string& name, const std::string& fallback = {}) const;

    std::string script_name;
    std::vector<ScriptParam> params;

    // Runtime state (not saved/copied): the live script object while
    // playing, created by ScriptSystem.
    std::unique_ptr<Script> instance;
    bool started = false;
};

// --- Actors ------------------------------------------------------------

class Actor {
public:
    Actor(ActorId id, std::string name);

    ActorId id() const { return id_; }
    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    Transform transform; // local, relative to the parent
    bool active = true;

    ActorId parent() const { return parent_; }
    const std::vector<ActorId>& children() const { return children_; }

    // World matrix as of the last Scene::update_world_transforms() call.
    const Mat4& world_matrix() const { return world_; }
    Vec3 world_position() const { return world_.translation_part(); }

    template <typename T, typename... Args>
    T& add_component(Args&&... args) {
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *component;
        components_.push_back(std::move(component));
        return ref;
    }
    Component& add_component(std::unique_ptr<Component> component);
    void remove_component(std::size_t index);

    template <typename T>
    T* find_component() {
        for (auto& component : components_) {
            if (component->type() == T::kType) {
                return static_cast<T*>(component.get());
            }
        }
        return nullptr;
    }

    template <typename T>
    const T* find_component() const {
        for (const auto& component : components_) {
            if (component->type() == T::kType) {
                return static_cast<const T*>(component.get());
            }
        }
        return nullptr;
    }

    bool has_component(ComponentType type) const;
    const std::vector<std::unique_ptr<Component>>& components() const { return components_; }
    std::string display_type() const;

private:
    friend class Scene;
    ActorId id_ = kNoActor;
    std::string name_;
    ActorId parent_ = kNoActor;
    std::vector<ActorId> children_;
    std::vector<std::unique_ptr<Component>> components_;
    Mat4 world_ = Mat4::identity();
};

class Scene {
public:
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = default;
    Scene& operator=(Scene&&) = default;

    Actor& create_actor(std::string name, ActorId parent = kNoActor);
    // Used by deserialization to recreate an actor with its saved id.
    Actor& create_actor_with_id(ActorId id, std::string name);
    // Destroys the actor and its whole subtree.
    void destroy_actor(ActorId id);
    void clear();

    Actor* find(ActorId id);
    const Actor* find(ActorId id) const;
    Actor* find_by_name(const std::string& name);

    // Reparents `child` under `parent` (kNoActor = make it a root). With
    // keep_world the child's world transform is preserved. Refuses (returns
    // false) if that would create a cycle.
    bool set_parent(ActorId child, ActorId parent, bool keep_world = true);
    bool is_ancestor(ActorId ancestor, ActorId actor) const;
    // Deep copy of the actor and its subtree, placed under the same parent.
    ActorId duplicate_actor(ActorId id);

    // Every actor, in creation order (parents always before their children
    // after a load).
    const std::vector<std::unique_ptr<Actor>>& actors() const { return actors_; }
    std::vector<ActorId> root_actors() const;
    std::size_t actor_count() const { return actors_.size(); }

    // Recomputes every actor's cached world matrix (call once per frame
    // after transforms change, before systems that read world_matrix()).
    void update_world_transforms();
    // Computes one actor's world matrix from scratch (always current).
    Mat4 compute_world_matrix(ActorId id) const;
    Mat4 compute_parent_world_matrix(ActorId id) const;
    // Sets an actor's local transform so its world translation becomes
    // `world_position`.
    void set_world_position(ActorId id, Vec3 world_position);

    Actor* selected_actor();
    const Actor* selected_actor() const;
    ActorId selected_actor_id() const { return selected_; }
    void select_actor(ActorId id);
    void clear_selection() { selected_ = kNoActor; }

    std::string unique_actor_name(const std::string& base) const;
    ActorId next_actor_id() const { return next_actor_id_; }

private:
    void detach_from_parent(Actor& actor);
    std::size_t index_of(ActorId id) const;
    void update_world_recursive(Actor& actor, const Mat4& parent_world);

    ActorId next_actor_id_ = 1;
    ActorId selected_ = kNoActor;
    std::vector<std::unique_ptr<Actor>> actors_;
};

} // namespace Hollow

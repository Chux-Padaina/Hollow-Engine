#pragma once

#include "Forge/Math.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Forge {

enum class Mobility {
    Static,
    Stationary,
    Movable,
};

enum class ComponentKind {
    StaticMesh,
    SkeletalMesh,
    Light,
    Camera,
    Collision,
    PhysicsBody,
    Script,
    Audio,
    Animation,
    Input,
    Ui,
    ParticleSystem,
    NavigationAgent,
    AiController,
    PostProcessVolume,
    NetworkReplication,
};

struct SceneComponent {
    Transform local_transform;
    Mobility mobility = Mobility::Movable;
};

class Component {
public:
    explicit Component(ComponentKind kind);
    virtual ~Component() = default;

    ComponentKind kind() const { return kind_; }
    virtual std::string display_name() const = 0;

private:
    ComponentKind kind_;
};

struct Material {
    Vec3 base_color{0.75f, 0.72f, 0.62f};
    float metallic = 0.0f;
    float roughness = 0.55f;
};

enum class MeshPrimitive {
    Cube,
    Plane,
    Sphere,
};

struct MeshPart {
    std::string name;
    MeshPrimitive primitive = MeshPrimitive::Cube;
    Material material;
};

class StaticMeshComponent final : public Component {
public:
    StaticMeshComponent();
    std::string display_name() const override;

    std::vector<MeshPart> parts;
    bool visible = true;
};

enum class LightKind {
    Directional,
    Point,
    Spot,
};

class LightComponent final : public Component {
public:
    explicit LightComponent(LightKind kind);
    std::string display_name() const override;

    LightKind light_kind = LightKind::Point;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 5.0f;
    float spot_angle_radians = 0.55f;
    bool enabled = true;
};

class PlaceholderComponent final : public Component {
public:
    PlaceholderComponent(ComponentKind kind, std::string name);
    std::string display_name() const override;

private:
    std::string name_;
};

using ActorId = std::uint32_t;

class Actor {
public:
    Actor(ActorId id, std::string name);

    ActorId id() const { return id_; }
    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    SceneComponent& root() { return root_; }
    const SceneComponent& root() const { return root_; }

    ActorId parent() const { return parent_; }
    const std::vector<ActorId>& children() const { return children_; }

    template <typename T, typename... Args>
    T& add_component(Args&&... args) {
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *component;
        components_.push_back(std::move(component));
        return ref;
    }

    template <typename T>
    T* find_component() {
        for (auto& component : components_) {
            if (auto* typed = dynamic_cast<T*>(component.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    template <typename T>
    const T* find_component() const {
        for (const auto& component : components_) {
            if (const auto* typed = dynamic_cast<const T*>(component.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    const std::vector<std::unique_ptr<Component>>& components() const { return components_; }
    std::string display_type() const;

private:
    ActorId id_ = 0;
    std::string name_;
    SceneComponent root_;
    std::vector<std::unique_ptr<Component>> components_;
    ActorId parent_ = 0;
    std::vector<ActorId> children_;
    bool tick_enabled_ = false;
};

class Scene {
public:
    Actor& create_actor(std::string name);
    Actor& create_static_mesh_actor(std::string name, MeshPrimitive primitive, Transform transform);
    Actor& create_light_actor(std::string name, LightKind kind, Transform transform);
    void destroy_selected_actor();

    std::vector<std::unique_ptr<Actor>>& actors() { return actors_; }
    const std::vector<std::unique_ptr<Actor>>& actors() const { return actors_; }

    Actor* selected_actor();
    const Actor* selected_actor() const;
    void select_actor(ActorId id);
    ActorId selected_actor_id() const { return selected_actor_id_; }

    std::string unique_actor_name(const std::string& base) const;

private:
    ActorId next_actor_id_ = 1;
    ActorId selected_actor_id_ = 0;
    std::vector<std::unique_ptr<Actor>> actors_;
};

Scene make_default_scene();

} // namespace Forge

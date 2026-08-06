#include "Forge/Scene.hpp"

#include <algorithm>
#include <numbers>

namespace Forge {

Component::Component(ComponentKind kind) : kind_(kind) {}

StaticMeshComponent::StaticMeshComponent() : Component(ComponentKind::StaticMesh) {}

std::string StaticMeshComponent::display_name() const {
    return "Static Mesh";
}

LightComponent::LightComponent(LightKind kind) : Component(ComponentKind::Light), light_kind(kind) {}

std::string LightComponent::display_name() const {
    switch (light_kind) {
    case LightKind::Directional:
        return "Directional Light";
    case LightKind::Point:
        return "Point Light";
    case LightKind::Spot:
        return "Spot Light";
    }
    return "Light";
}

PlaceholderComponent::PlaceholderComponent(ComponentKind kind, std::string name)
    : Component(kind), name_(std::move(name)) {}

std::string PlaceholderComponent::display_name() const {
    return name_;
}

Actor::Actor(ActorId id, std::string name) : id_(id), name_(std::move(name)) {}

std::string Actor::display_type() const {
    if (const auto* light = find_component<LightComponent>()) {
        return light->display_name();
    }
    if (find_component<StaticMeshComponent>()) {
        return "Static Mesh Actor";
    }
    return "Actor";
}

Actor& Scene::create_actor(std::string name) {
    actors_.push_back(std::make_unique<Actor>(next_actor_id_++, std::move(name)));
    return *actors_.back();
}

Actor& Scene::create_static_mesh_actor(std::string name, MeshPrimitive primitive, Transform transform) {
    Actor& actor = create_actor(unique_actor_name(name));
    actor.root().local_transform = transform;
    auto& mesh = actor.add_component<StaticMeshComponent>();
    MeshPart part;
    part.name = actor.name();
    part.primitive = primitive;
    if (primitive == MeshPrimitive::Plane) {
        part.material.base_color = {0.46f, 0.48f, 0.46f};
        part.material.roughness = 0.86f;
    } else if (primitive == MeshPrimitive::Sphere) {
        part.material.base_color = {0.58f, 0.68f, 0.84f};
        part.material.roughness = 0.48f;
    }
    mesh.parts.push_back(part);
    selected_actor_id_ = actor.id();
    return actor;
}

Actor& Scene::create_mesh_actor(std::string name, std::vector<MeshPart> parts, Transform transform) {
    Actor& actor = create_actor(unique_actor_name(name));
    actor.root().local_transform = transform;
    auto& mesh = actor.add_component<StaticMeshComponent>();
    mesh.parts = std::move(parts);
    selected_actor_id_ = actor.id();
    return actor;
}

Actor& Scene::create_light_actor(std::string name, LightKind kind, Transform transform) {
    Actor& actor = create_actor(unique_actor_name(name));
    actor.root().local_transform = transform;
    auto& light = actor.add_component<LightComponent>(kind);
    switch (kind) {
    case LightKind::Directional:
        light.color = {1.0f, 0.98f, 0.9f};
        light.intensity = 1.2f;
        break;
    case LightKind::Point:
        light.color = {0.35f, 0.75f, 1.0f};
        light.intensity = 1.5f;
        light.range = 8.0f;
        break;
    case LightKind::Spot:
        light.color = {1.0f, 0.68f, 0.32f};
        light.intensity = 1.7f;
        light.range = 9.0f;
        light.spot_angle_radians = 32.0f * std::numbers::pi_v<float> / 180.0f;
        break;
    }
    selected_actor_id_ = actor.id();
    return actor;
}

void Scene::destroy_selected_actor() {
    if (selected_actor_id_ == 0) {
        return;
    }
    const auto old_selection = selected_actor_id_;
    actors_.erase(
        std::remove_if(
            actors_.begin(),
            actors_.end(),
            [old_selection](const std::unique_ptr<Actor>& actor) {
                return actor->id() == old_selection;
            }),
        actors_.end());
    selected_actor_id_ = actors_.empty() ? 0 : actors_.front()->id();
}

Actor* Scene::selected_actor() {
    const auto id = selected_actor_id_;
    auto it = std::find_if(actors_.begin(), actors_.end(), [id](const std::unique_ptr<Actor>& actor) {
        return actor->id() == id;
    });
    return it == actors_.end() ? nullptr : it->get();
}

const Actor* Scene::selected_actor() const {
    const auto id = selected_actor_id_;
    auto it = std::find_if(actors_.begin(), actors_.end(), [id](const std::unique_ptr<Actor>& actor) {
        return actor->id() == id;
    });
    return it == actors_.end() ? nullptr : it->get();
}

void Scene::select_actor(ActorId id) {
    const auto exists = std::any_of(actors_.begin(), actors_.end(), [id](const std::unique_ptr<Actor>& actor) {
        return actor->id() == id;
    });
    if (exists) {
        selected_actor_id_ = id;
    }
}

std::string Scene::unique_actor_name(const std::string& base) const {
    auto exists = [this](const std::string& name) {
        return std::any_of(actors_.begin(), actors_.end(), [&name](const std::unique_ptr<Actor>& actor) {
            return actor->name() == name;
        });
    };
    if (!exists(base)) {
        return base;
    }
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + " " + std::to_string(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
}

Scene make_default_scene() {
    Scene scene;
    Transform cube_a;
    cube_a.translation = {-1.2f, 0.5f, 0.0f};
    auto& a = scene.create_static_mesh_actor("Cube A", MeshPrimitive::Cube, cube_a);
    if (auto* mesh = a.find_component<StaticMeshComponent>()) {
        mesh->parts.front().material.base_color = {0.82f, 0.66f, 0.42f};
        mesh->parts.front().material.roughness = 0.72f;
    }

    Transform cube_b;
    cube_b.translation = {1.2f, 0.5f, 0.0f};
    cube_b.scale = {0.8f, 0.8f, 0.8f};
    auto& b = scene.create_static_mesh_actor("Cube B", MeshPrimitive::Cube, cube_b);
    if (auto* mesh = b.find_component<StaticMeshComponent>()) {
        mesh->parts.front().material.base_color = {0.42f, 0.58f, 0.76f};
        mesh->parts.front().material.metallic = 1.0f;
        mesh->parts.front().material.roughness = 0.28f;
    }

    Transform directional;
    directional.translation = {1.35f, 2.7f, 1.05f};
    directional.rotation = {0.5f, -0.45f, 0.0f};
    scene.create_light_actor("Key Directional", LightKind::Directional, directional);

    Transform point;
    point.translation = {0.0f, 2.4f, 2.2f};
    scene.create_light_actor("Test Point", LightKind::Point, point);

    scene.select_actor(1);
    return scene;
}

} // namespace Forge

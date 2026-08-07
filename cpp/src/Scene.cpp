// Implements the Scene/Actor/Component types declared in Scene.hpp: actor
// creation and lookup, per-light-kind default parameters, and
// make_default_scene(), which builds the scene the editor opens with.

#include "Hollow/Scene.hpp"

#include <algorithm>
#include <numbers>

namespace Hollow {

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

// Deliberately minimal: one sphere, one directional light, and the default
// environment map (loaded separately by App::run — see its comment for why
// that lives outside the scene) for a plain, portfolio-ready first look at
// the renderer. Earlier revisions also seeded a cube alongside the sphere,
// plus a blue point light and an orange spot light to exercise every light
// type by default, but once IBL started sampling a real environment
// correctly those extra lights' direct contributions became a much more
// visible (and confusing) part of the image, making it hard to tell an
// actual rendering bug from "that's just the scene's own point light," and
// the second mesh added nothing the single sphere didn't already show.
// Cubes, point lights, and spot lights are all still fully supported (see
// MeshPrimitive, LightKind, the outliner's "Add Object"/"Add Light" menus)
// — they're just not part of the default scene anymore.
Scene make_default_scene() {
    Scene scene;

    Transform sphere;
    sphere.translation = {0.0f, 0.5f, 0.0f};
    scene.create_static_mesh_actor("Sphere", MeshPrimitive::Sphere, sphere);

    Transform directional;
    directional.translation = {1.35f, 2.7f, 1.05f};
    directional.rotation = {0.5f, -0.45f, 0.0f};
    scene.create_light_actor("Key Directional", LightKind::Directional, directional);

    scene.select_actor(1);
    return scene;
}

} // namespace Hollow

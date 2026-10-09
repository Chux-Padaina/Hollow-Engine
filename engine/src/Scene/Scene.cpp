// Implementation of the Scene/Actor/Component types declared in Scene.hpp.
#include "Hollow/Scene/Scene.hpp"

#include "Hollow/Scripting/Script.hpp"

#include <algorithm>

namespace Hollow {

Aabb StaticMeshComponent::local_bounds() const {
    Aabb bounds = Aabb::empty();
    for (const MeshPart& part : parts) {
        if (part.mesh) {
            bounds.expand(part.mesh->bounds);
        }
    }
    return bounds.valid() ? bounds : Aabb{};
}

ScriptComponent::ScriptComponent() = default;

ScriptComponent::ScriptComponent(const ScriptComponent& other)
    : Component(other), script_name(other.script_name), params(other.params) {}

ScriptComponent& ScriptComponent::operator=(const ScriptComponent& other) {
    if (this != &other) {
        script_name = other.script_name;
        params = other.params;
        instance.reset();
        started = false;
    }
    return *this;
}

ScriptComponent::~ScriptComponent() = default;

std::unique_ptr<Component> ScriptComponent::clone() const {
    return std::make_unique<ScriptComponent>(*this);
}

float ScriptComponent::number(const std::string& name, float fallback) const {
    for (const ScriptParam& param : params) {
        if (param.name == name && !param.is_text) {
            return param.number;
        }
    }
    return fallback;
}

std::string ScriptComponent::text(const std::string& name, const std::string& fallback) const {
    for (const ScriptParam& param : params) {
        if (param.name == name && param.is_text) {
            return param.text;
        }
    }
    return fallback;
}

Actor::Actor(ActorId id, std::string name) : id_(id), name_(std::move(name)) {}

Component& Actor::add_component(std::unique_ptr<Component> component) {
    components_.push_back(std::move(component));
    return *components_.back();
}

void Actor::remove_component(std::size_t index) {
    if (index < components_.size()) {
        components_.erase(components_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

bool Actor::has_component(ComponentType type) const {
    return std::any_of(components_.begin(), components_.end(), [type](const std::unique_ptr<Component>& component) {
        return component->type() == type;
    });
}

std::string Actor::display_type() const {
    if (const auto* light = find_component<LightComponent>()) {
        return std::string(light_kind_name(light->light_kind)) + " Light";
    }
    if (find_component<CameraComponent>() != nullptr) {
        return "Camera";
    }
    if (find_component<StaticMeshComponent>() != nullptr) {
        return "Static Mesh";
    }
    return "Empty";
}

Actor& Scene::create_actor(std::string name, ActorId parent) {
    Actor& actor = create_actor_with_id(next_actor_id_, unique_actor_name(name));
    if (parent != kNoActor) {
        set_parent(actor.id(), parent, false);
    }
    return actor;
}

Actor& Scene::create_actor_with_id(ActorId id, std::string name) {
    actors_.push_back(std::make_unique<Actor>(id, std::move(name)));
    next_actor_id_ = std::max(next_actor_id_, id + 1);
    return *actors_.back();
}

std::size_t Scene::index_of(ActorId id) const {
    for (std::size_t i = 0; i < actors_.size(); ++i) {
        if (actors_[i]->id() == id) {
            return i;
        }
    }
    return actors_.size();
}

Actor* Scene::find(ActorId id) {
    const std::size_t index = index_of(id);
    return index < actors_.size() ? actors_[index].get() : nullptr;
}

const Actor* Scene::find(ActorId id) const {
    const std::size_t index = index_of(id);
    return index < actors_.size() ? actors_[index].get() : nullptr;
}

Actor* Scene::find_by_name(const std::string& name) {
    for (auto& actor : actors_) {
        if (actor->name() == name) {
            return actor.get();
        }
    }
    return nullptr;
}

void Scene::detach_from_parent(Actor& actor) {
    if (actor.parent_ == kNoActor) {
        return;
    }
    if (Actor* parent = find(actor.parent_)) {
        auto& siblings = parent->children_;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), actor.id()), siblings.end());
    }
    actor.parent_ = kNoActor;
}

void Scene::destroy_actor(ActorId id) {
    Actor* actor = find(id);
    if (actor == nullptr) {
        return;
    }
    // Copy: destroying children edits this list.
    const std::vector<ActorId> children = actor->children_;
    for (ActorId child : children) {
        destroy_actor(child);
    }
    actor = find(id);
    detach_from_parent(*actor);
    actors_.erase(actors_.begin() + static_cast<std::ptrdiff_t>(index_of(id)));
    if (selected_ == id) {
        selected_ = kNoActor;
    }
}

void Scene::clear() {
    actors_.clear();
    selected_ = kNoActor;
    next_actor_id_ = 1;
}

bool Scene::is_ancestor(ActorId ancestor, ActorId actor) const {
    const Actor* current = find(actor);
    int guard = 0;
    while (current != nullptr && current->parent_ != kNoActor && guard++ < 10000) {
        if (current->parent_ == ancestor) {
            return true;
        }
        current = find(current->parent_);
    }
    return false;
}

bool Scene::set_parent(ActorId child_id, ActorId parent_id, bool keep_world) {
    Actor* child = find(child_id);
    if (child == nullptr || child_id == parent_id) {
        return false;
    }
    if (parent_id != kNoActor && (find(parent_id) == nullptr || is_ancestor(child_id, parent_id))) {
        return false;
    }
    if (child->parent_ == parent_id) {
        return true;
    }

    const Mat4 world = compute_world_matrix(child_id);
    detach_from_parent(*child);
    if (parent_id != kNoActor) {
        Actor* parent = find(parent_id);
        parent->children_.push_back(child_id);
        child->parent_ = parent_id;
    }
    if (keep_world) {
        child->transform = decompose(inverse(compute_parent_world_matrix(child_id)) * world);
    }
    return true;
}

ActorId Scene::duplicate_actor(ActorId id) {
    const Actor* source = find(id);
    if (source == nullptr) {
        return kNoActor;
    }
    Actor& copy = create_actor(source->name(), kNoActor);
    copy.transform = source->transform;
    copy.active = source->active;
    for (const auto& component : source->components()) {
        copy.add_component(component->clone());
    }
    const ActorId copy_id = copy.id();
    const ActorId parent_id = source->parent_;
    const std::vector<ActorId> children = source->children_;
    if (parent_id != kNoActor) {
        set_parent(copy_id, parent_id, false);
    }
    for (ActorId child : children) {
        const ActorId child_copy = duplicate_actor(child);
        set_parent(child_copy, copy_id, false);
    }
    return copy_id;
}

std::vector<ActorId> Scene::root_actors() const {
    std::vector<ActorId> roots;
    for (const auto& actor : actors_) {
        if (actor->parent_ == kNoActor) {
            roots.push_back(actor->id());
        }
    }
    return roots;
}

void Scene::update_world_recursive(Actor& actor, const Mat4& parent_world) {
    actor.world_ = parent_world * actor.transform.matrix();
    for (ActorId child_id : actor.children_) {
        if (Actor* child = find(child_id)) {
            update_world_recursive(*child, actor.world_);
        }
    }
}

void Scene::update_world_transforms() {
    const Mat4 identity = Mat4::identity();
    for (auto& actor : actors_) {
        if (actor->parent_ == kNoActor) {
            update_world_recursive(*actor, identity);
        }
    }
}

Mat4 Scene::compute_world_matrix(ActorId id) const {
    const Actor* actor = find(id);
    if (actor == nullptr) {
        return Mat4::identity();
    }
    return compute_parent_world_matrix(id) * actor->transform.matrix();
}

Mat4 Scene::compute_parent_world_matrix(ActorId id) const {
    const Actor* actor = find(id);
    if (actor == nullptr || actor->parent_ == kNoActor) {
        return Mat4::identity();
    }
    return compute_world_matrix(actor->parent_);
}

void Scene::set_world_position(ActorId id, Vec3 world_position) {
    Actor* actor = find(id);
    if (actor == nullptr) {
        return;
    }
    if (actor->parent_ == kNoActor) {
        actor->transform.translation = world_position;
        return;
    }
    actor->transform.translation = transform_point(inverse(compute_parent_world_matrix(id)), world_position);
}

Actor* Scene::selected_actor() {
    return selected_ == kNoActor ? nullptr : find(selected_);
}

const Actor* Scene::selected_actor() const {
    return selected_ == kNoActor ? nullptr : find(selected_);
}

void Scene::select_actor(ActorId id) {
    if (id == kNoActor || find(id) != nullptr) {
        selected_ = id;
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
    // "Cube 3" duplicates as "Cube 4", not "Cube 3 2".
    std::string stem = base;
    const std::size_t space = base.find_last_of(' ');
    if (space != std::string::npos && space + 1 < base.size() &&
        std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space + 1), base.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        stem = base.substr(0, space);
    }
    for (int suffix = 2;; ++suffix) {
        std::string candidate = stem + " " + std::to_string(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
}

} // namespace Hollow

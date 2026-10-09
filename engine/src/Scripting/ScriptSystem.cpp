// ScriptRegistry and ScriptSystem: name -> factory lookup, and the per-frame
// driver that instantiates, starts and updates script components in Play
// mode.
#include "Hollow/Scripting/Script.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <format>

namespace Hollow {

ScriptRegistry& ScriptRegistry::instance() {
    static ScriptRegistry registry;
    return registry;
}

void ScriptRegistry::add(ScriptInfo info) {
    if (info.module.empty()) {
        info.module = current_module_;
    }
    auto it = std::find_if(scripts_.begin(), scripts_.end(), [&info](const ScriptInfo& existing) { return existing.name == info.name; });
    if (it != scripts_.end()) {
        *it = std::move(info);
        return;
    }
    scripts_.push_back(std::move(info));
}

void ScriptRegistry::remove_module(const std::string& module) {
    std::erase_if(scripts_, [&module](const ScriptInfo& info) { return info.module == module; });
}

std::size_t ScriptRegistry::count_module(const std::string& module) const {
    return static_cast<std::size_t>(std::count_if(scripts_.begin(), scripts_.end(), [&module](const ScriptInfo& info) { return info.module == module; }));
}

const ScriptInfo* ScriptRegistry::find(const std::string& name) const {
    auto it = std::find_if(scripts_.begin(), scripts_.end(), [&name](const ScriptInfo& info) { return info.name == name; });
    return it == scripts_.end() ? nullptr : &*it;
}

void ScriptRegistry::assign(ScriptComponent& component, const std::string& script_name) const {
    component.script_name = script_name;
    component.params.clear();
    component.instance.reset();
    component.started = false;
    fill_missing_params(component);
}

void ScriptRegistry::fill_missing_params(ScriptComponent& component) const {
    const ScriptInfo* info = find(component.script_name);
    if (info == nullptr) {
        return;
    }
    for (const ScriptParamDefault& def : info->params) {
        const bool present = std::any_of(component.params.begin(), component.params.end(), [&def](const ScriptParam& param) {
            return param.name == def.name && param.is_text == def.is_text;
        });
        if (!present) {
            component.params.push_back({def.name, def.number, def.text, def.is_text});
        }
    }
}

void ScriptSystem::begin_play() {
    playing_ = true;
    destroy_queue_.clear();
}

void ScriptSystem::end_play(Scene& scene) {
    playing_ = false;
    destroy_queue_.clear();
    for (const auto& actor : scene.actors()) {
        for (const auto& component : actor->components()) {
            if (component->type() == ComponentType::Script) {
                auto& script = static_cast<ScriptComponent&>(*component);
                script.instance.reset();
                script.started = false;
            }
        }
    }
}

void ScriptSystem::flush_destroy_queue(Scene& scene) {
    std::vector<ActorId> queue;
    queue.swap(destroy_queue_);
    for (ActorId id : queue) {
        scene.destroy_actor(id);
    }
}

void ScriptSystem::update(Scene& scene, const Input& input, PhysicsWorld& physics, float dt, float time) {
    if (!playing_) {
        return;
    }
    // Snapshot ids: scripts may spawn or queue-destroy actors while running.
    std::vector<ActorId> ids;
    ids.reserve(scene.actor_count());
    for (const auto& actor : scene.actors()) {
        ids.push_back(actor->id());
    }

    for (ActorId id : ids) {
        Actor* actor = scene.find(id);
        if (actor == nullptr || !actor->active) {
            continue;
        }
        for (std::size_t i = 0; i < actor->components().size(); ++i) {
            Component& component = *actor->components()[i];
            if (component.type() != ComponentType::Script) {
                continue;
            }
            auto& script = static_cast<ScriptComponent&>(component);
            if (!script.instance) {
                const ScriptInfo* info = ScriptRegistry::instance().find(script.script_name);
                if (info == nullptr || !info->create) {
                    if (!script.started) {
                        log_warning(std::format("Actor '{}': unknown script '{}'", actor->name(), script.script_name));
                        script.started = true; // warn once
                    }
                    continue;
                }
                script.instance = info->create();
                script.started = false;
            }
            ScriptContext context{scene, *actor, script, input, physics, destroy_queue_, time};
            if (!script.started) {
                script.started = true;
                script.instance->on_start(context);
            }
            script.instance->on_update(context, dt);
        }
    }
    flush_destroy_queue(scene);
}

void ScriptSystem::dispatch_collision(Scene& scene, const Input& input, PhysicsWorld& physics, ActorId actor_id, ActorId other, float time,
    ContactCallback callback) {
    if (!playing_) {
        return;
    }
    Actor* actor = scene.find(actor_id);
    if (actor == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < actor->components().size(); ++i) {
        Component& component = *actor->components()[i];
        if (component.type() != ComponentType::Script) {
            continue;
        }
        auto& script = static_cast<ScriptComponent&>(component);
        if (script.instance && script.started) {
            ScriptContext context{scene, *actor, script, input, physics, destroy_queue_, time};
            switch (callback) {
            case ContactCallback::Stay: script.instance->on_collision(context, other); break;
            case ContactCallback::CollisionEnter: script.instance->on_collision_enter(context, other); break;
            case ContactCallback::CollisionExit: script.instance->on_collision_exit(context, other); break;
            case ContactCallback::TriggerEnter: script.instance->on_trigger_enter(context, other); break;
            case ContactCallback::TriggerExit: script.instance->on_trigger_exit(context, other); break;
            }
        }
    }
    flush_destroy_queue(scene);
}

} // namespace Hollow

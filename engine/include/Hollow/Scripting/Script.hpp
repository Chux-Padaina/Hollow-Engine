// Gameplay scripting: native C++ behaviours attached to actors through a
// ScriptComponent. A script is a class deriving from Script, registered by
// name in the ScriptRegistry together with its editable parameters; the
// editor lists registered scripts and the scene file stores the name +
// parameter values.
//
//   class Spinner : public Script {
//   public:
//       void on_update(ScriptContext& ctx, float dt) override {
//           ctx.actor.transform.rotation.y += ctx.number("speed", 1.0f) * dt;
//       }
//   };
//   ScriptRegistry::instance().add({"Spinner", "Spins around Y",
//       {{"speed", 1.0f}}, [] { return std::make_unique<Spinner>(); }});
//
// Scripts only run in Play mode (see ScriptSystem). Each frame
// on_update() runs once with the variable frame delta; physics runs on a
// fixed step and reports contacts through on_collision().
#pragma once

#include "Hollow/Scene/Scene.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Hollow {

class Input;
class PhysicsWorld;

struct ScriptContext {
    Scene& scene;
    Actor& actor;
    ScriptComponent& component;
    const Input& input;
    PhysicsWorld& physics;
    std::vector<ActorId>& destroy_queue;
    float time = 0.0f; // seconds since Play started

    // Destroys an actor at the end of the frame. Scripts must use this
    // rather than Scene::destroy_actor, which would free the running script.
    void destroy(ActorId id) const { destroy_queue.push_back(id); }

    float number(const std::string& name, float fallback = 0.0f) const { return component.number(name, fallback); }
    std::string text(const std::string& name, const std::string& fallback = {}) const { return component.text(name, fallback); }
};

class Script {
public:
    virtual ~Script() = default;
    virtual void on_start(ScriptContext&) {}
    virtual void on_update(ScriptContext&, float) {}
    // Called after every physics step for each actor this one is touching
    // (including trigger overlaps).
    virtual void on_collision(ScriptContext&, ActorId) {}
    // Called once when a contact starts / ends.
    virtual void on_collision_enter(ScriptContext&, ActorId) {}
    virtual void on_collision_exit(ScriptContext&, ActorId) {}
    // Called once when a trigger overlap starts / ends (on both the trigger
    // and the other actor).
    virtual void on_trigger_enter(ScriptContext&, ActorId) {}
    virtual void on_trigger_exit(ScriptContext&, ActorId) {}
};

struct ScriptParamDefault {
    std::string name;
    float number = 0.0f;
    std::string text;
    bool is_text = false;

    ScriptParamDefault(std::string param_name, float value) : name(std::move(param_name)), number(value) {}
    ScriptParamDefault(std::string param_name, const char* value) : name(std::move(param_name)), text(value), is_text(true) {}
};

struct ScriptInfo {
    std::string name;
    std::string description;
    std::vector<ScriptParamDefault> params;
    std::function<std::unique_ptr<Script>()> create;
    std::string module; // "" for the engine's built-ins, else the game module that registered it
};

class ScriptRegistry {
public:
    static ScriptRegistry& instance();

    void add(ScriptInfo info);
    // Scripts added while a module is current are tagged with it, so a game
    // module's scripts can be removed before the module is unloaded.
    void set_current_module(std::string module) { current_module_ = std::move(module); }
    void remove_module(const std::string& module);
    std::size_t count_module(const std::string& module) const;
    const ScriptInfo* find(const std::string& name) const;
    const std::vector<ScriptInfo>& scripts() const { return scripts_; }

    // Resets `component` to `script_name` with its default parameters.
    void assign(ScriptComponent& component, const std::string& script_name) const;
    // Adds any parameters the script declares that `component` lacks (e.g.
    // after a script gained a parameter since the scene was saved).
    void fill_missing_params(ScriptComponent& component) const;

private:
    std::vector<ScriptInfo> scripts_;
    std::string current_module_;
};

// Registers Rotator, Oscillator, PlayerController and FollowCamera. Safe to
// call more than once.
void register_builtin_scripts();

// Drives every ScriptComponent while the game is playing.
class ScriptSystem {
public:
    void begin_play();
    void end_play(Scene& scene);
    // Creates instances for new components, runs on_start once, then
    // on_update for every active scripted actor.
    void update(Scene& scene, const Input& input, PhysicsWorld& physics, float dt, float time);
    enum class ContactCallback {
        Stay,
        CollisionEnter,
        CollisionExit,
        TriggerEnter,
        TriggerExit,
    };
    void dispatch_collision(Scene& scene, const Input& input, PhysicsWorld& physics, ActorId actor, ActorId other, float time,
        ContactCallback callback = ContactCallback::Stay);

private:
    void flush_destroy_queue(Scene& scene);

    bool playing_ = false;
    std::vector<ActorId> destroy_queue_;
};

} // namespace Hollow

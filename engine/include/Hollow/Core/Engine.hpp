// Engine owns the runtime world: the Scene, the AssetManager, and the
// gameplay systems (physics + scripting), plus the render settings saved
// with a scene. The editor shell (App) drives it; a standalone game
// executable could drive the same object without any editor code.
//
// Play mode: begin_play() snapshots nothing itself (the editor keeps the
// edit-time scene so Stop can restore it); update() then advances physics on
// a fixed 60 Hz step and scripts once per frame.
#pragma once

#include "Hollow/Assets/AssetManager.hpp"
#include "Hollow/Physics/PhysicsWorld.hpp"
#include "Hollow/Render/RenderSettings.hpp"
#include "Hollow/Scene/Scene.hpp"
#include "Hollow/Scripting/GameModule.hpp"
#include "Hollow/Scripting/Script.hpp"

namespace Hollow {

class Input;

class Engine {
public:
    Engine();
    // Ends Play first so no script instance outlives the game module's code.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Scene& scene() { return scene_; }
    const Scene& scene() const { return scene_; }
    AssetManager& assets() { return assets_; }
    PhysicsWorld& physics() { return physics_; }
    const PhysicsWorld& physics() const { return physics_; }
    ScriptSystem& scripts() { return scripts_; }
    // Gameplay code in Game.dll (hot-reloaded; see GameModule.hpp).
    GameModuleHost& game_module() { return game_module_; }
    RenderPipelineSettings& render_settings() { return render_settings_; }
    const RenderPipelineSettings& render_settings() const { return render_settings_; }

    void begin_play();
    void end_play();
    bool playing() const { return playing_; }
    bool paused() const { return paused_; }
    void set_paused(bool paused) { paused_ = paused; }
    // Advances exactly one fixed step while paused.
    void request_single_step() { single_step_ = true; }
    float play_time() const { return play_time_; }

    // Per-frame update. Does nothing outside Play mode except keeping world
    // transforms current.
    void update(float frame_dt, const Input& input);

    static constexpr float kFixedTimeStep = 1.0f / 60.0f;

private:
    void fixed_step(const Input& input);

    Scene scene_;
    AssetManager assets_;
    PhysicsWorld physics_;
    ScriptSystem scripts_;
    GameModuleHost game_module_;
    RenderPipelineSettings render_settings_;
    bool playing_ = false;
    bool paused_ = false;
    bool single_step_ = false;
    float accumulator_ = 0.0f;
    float play_time_ = 0.0f;
};

} // namespace Hollow

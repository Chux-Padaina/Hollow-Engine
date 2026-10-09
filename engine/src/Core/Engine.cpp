// Implementation of Engine: Play-mode lifecycle and the fixed-step loop.
#include "Hollow/Core/Engine.hpp"
#include "Hollow/Scene/AnimationSystem.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>

namespace Hollow {

Engine::Engine() {
    register_builtin_scripts();
}

Engine::~Engine() {
    end_play();
    game_module_.unload();
}

void Engine::begin_play() {
    if (playing_) {
        return;
    }
    playing_ = true;
    paused_ = false;
    accumulator_ = 0.0f;
    play_time_ = 0.0f;
    physics_.reset();
    for (const auto& actor : scene_.actors()) {
        if (auto* body = actor->find_component<RigidBodyComponent>()) {
            body->velocity = {};
            body->angular_velocity = {};
            body->pending_impulse = {};
            body->grounded = false;
        }
        if (auto* character = actor->find_component<CharacterControllerComponent>()) {
            character->velocity = {};
            character->grounded = false;
        }
    }
    scripts_.begin_play();
    log_info("Play mode started");
}

void Engine::end_play() {
    if (!playing_) {
        return;
    }
    scripts_.end_play(scene_);
    physics_.reset();
    playing_ = false;
    paused_ = false;
    log_info("Play mode stopped");
}

void Engine::fixed_step(const Input& input) {
    physics_.step(scene_, kFixedTimeStep);
    // Contact events, then ongoing contacts, go to the scripts on both actors.
    using Callback = ScriptSystem::ContactCallback;
    const std::vector<ContactEvent> events = physics_.events();
    for (const ContactEvent& event : events) {
        const Callback callback = event.trigger ? (event.begin ? Callback::TriggerEnter : Callback::TriggerExit)
                                                : (event.begin ? Callback::CollisionEnter : Callback::CollisionExit);
        scripts_.dispatch_collision(scene_, input, physics_, event.a, event.b, play_time_, callback);
        scripts_.dispatch_collision(scene_, input, physics_, event.b, event.a, play_time_, callback);
    }
    const std::vector<Contact> contacts = physics_.contacts();
    for (const Contact& contact : contacts) {
        scripts_.dispatch_collision(scene_, input, physics_, contact.a, contact.b, play_time_);
        scripts_.dispatch_collision(scene_, input, physics_, contact.b, contact.a, play_time_);
    }
}

void Engine::update(float frame_dt, const Input& input) {
    if (!playing_) {
        update_animators(scene_, std::min(frame_dt, 0.1f), false);
        scene_.update_world_transforms();
        return;
    }
    if (paused_) {
        if (single_step_) {
            single_step_ = false;
            play_time_ += kFixedTimeStep;
            scene_.update_world_transforms();
            scripts_.update(scene_, input, physics_, kFixedTimeStep, play_time_);
            fixed_step(input);
            update_animators(scene_, kFixedTimeStep, true);
        }
        scene_.update_world_transforms();
        return;
    }

    // Clamp so a long hitch (breakpoint, window drag) doesn't trigger a
    // burst of catch-up steps.
    const float dt = std::min(frame_dt, 0.1f);
    play_time_ += dt;
    scene_.update_world_transforms();
    scripts_.update(scene_, input, physics_, dt, play_time_);

    accumulator_ += dt;
    int steps = 0;
    while (accumulator_ >= kFixedTimeStep && steps < 8) {
        fixed_step(input);
        accumulator_ -= kFixedTimeStep;
        ++steps;
    }
    if (steps == 8) {
        accumulator_ = 0.0f;
    }
    update_animators(scene_, dt, true);
    scene_.update_world_transforms();
}

} // namespace Hollow

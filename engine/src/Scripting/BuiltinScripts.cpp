// Built-in example scripts. They double as documentation for writing your
// own: derive from Script, read parameters through the ScriptContext, and
// register a ScriptInfo in register_builtin_scripts().
#include "Hollow/Core/Input.hpp"
#include "Hollow/Core/Log.hpp"
#include "Hollow/Physics/PhysicsWorld.hpp"
#include "Hollow/Scripting/Script.hpp"

#include <cmath>
#include <format>

namespace Hollow {
namespace {

// Spins the actor at a constant rate around its local axes.
class Rotator final : public Script {
public:
    void on_update(ScriptContext& ctx, float dt) override {
        ctx.actor.transform.rotation += Vec3{ctx.number("speed_x"), ctx.number("speed_y", 45.0f), ctx.number("speed_z")} * (kDegToRad * dt);
    }
};

// Moves the actor back and forth along an axis around where it started.
class Oscillator final : public Script {
public:
    void on_start(ScriptContext& ctx) override {
        origin_ = ctx.actor.transform.translation;
    }
    void on_update(ScriptContext& ctx, float) override {
        const Vec3 axis = normalize({ctx.number("axis_x"), ctx.number("axis_y", 1.0f), ctx.number("axis_z")});
        const float offset = std::sin(ctx.time * ctx.number("frequency", 0.5f) * 2.0f * kPi) * ctx.number("amplitude", 1.0f);
        ctx.actor.transform.translation = origin_ + axis * offset;
    }

private:
    Vec3 origin_{};
};

// WASD/arrow movement relative to the active camera, Space to jump, Shift to
// sprint. Drives a CharacterController or RigidBody velocity when there is
// one (so it collides), otherwise moves the transform directly.
class PlayerController final : public Script {
public:
    void on_update(ScriptContext& ctx, float dt) override {
        const float input_x = ctx.input.axis("MoveX");
        const float input_z = ctx.input.axis("MoveZ");

        // Camera-relative basis on the ground plane.
        Vec3 forward{0.0f, 0.0f, -1.0f};
        for (const auto& actor : ctx.scene.actors()) {
            const auto* camera = actor->find_component<CameraComponent>();
            if (camera != nullptr && camera->primary && actor->active) {
                const Vec3 f = transform_vector(actor->world_matrix(), {0.0f, 0.0f, -1.0f});
                if (std::abs(f.x) + std::abs(f.z) > 1e-4f) {
                    forward = normalize({f.x, 0.0f, f.z});
                }
                break;
            }
        }
        const Vec3 right = normalize(cross(forward, {0.0f, 1.0f, 0.0f}));
        Vec3 move = forward * input_z + right * input_x;
        if (length(move) > 1.0f) {
            move = normalize(move);
        }
        float speed = ctx.number("speed", 4.0f);
        if (ctx.input.action_down("Sprint")) {
            speed *= ctx.number("sprint_multiplier", 1.8f);
        }

        if (length(move) > 0.01f) {
            // Face the direction of travel (actors look down local -Z).
            ctx.actor.transform.rotation.y = std::atan2(-move.x, -move.z);
        }

        if (auto* character = ctx.actor.find_component<CharacterControllerComponent>()) {
            character->velocity.x = move.x * speed;
            character->velocity.z = move.z * speed;
            if (ctx.input.action_pressed("Jump") && character->grounded) {
                character->velocity.y = ctx.number("jump_speed", 5.5f);
            }
        } else if (auto* body = ctx.actor.find_component<RigidBodyComponent>(); body != nullptr && !body->is_kinematic) {
            body->velocity.x = move.x * speed;
            body->velocity.z = move.z * speed;
            if (ctx.input.action_pressed("Jump") && body->grounded) {
                body->velocity.y = ctx.number("jump_speed", 5.5f);
                body->grounded = false;
            }
        } else {
            ctx.actor.transform.translation += move * (speed * dt);
        }
    }
};

// Smoothly follows a target actor (by name) from behind and above, looking
// at it. Put it on an actor with a CameraComponent.
class FollowCamera final : public Script {
public:
    void on_update(ScriptContext& ctx, float dt) override {
        const Actor* target = ctx.scene.find_by_name(ctx.text("target", "Player"));
        if (target == nullptr) {
            return;
        }
        const float yaw = ctx.number("yaw_degrees", 0.0f) * kDegToRad;
        const float distance = ctx.number("distance", 7.0f);
        const Vec3 offset{std::sin(yaw) * distance, ctx.number("height", 3.5f), std::cos(yaw) * distance};
        const Vec3 focus = target->world_position() + Vec3{0.0f, ctx.number("look_height", 0.8f), 0.0f};
        const Vec3 desired = target->world_position() + offset;

        const Vec3 current = ctx.actor.world_position();
        const float smoothing = ctx.number("smoothing", 6.0f);
        const float blend = smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-smoothing * dt);
        const Vec3 position = lerp(current, desired, blend);
        ctx.scene.set_world_position(ctx.actor.id(), position);
        ctx.actor.transform.rotation = rotation_for_forward(focus - position);
    }
};

// Spins in place and disappears when an actor with the given name touches
// it. Give it a trigger collider.
class Collectible final : public Script {
public:
    void on_update(ScriptContext& ctx, float dt) override {
        ctx.actor.transform.rotation.y += ctx.number("spin_degrees", 90.0f) * kDegToRad * dt;
    }
    void on_trigger_enter(ScriptContext& ctx, ActorId other) override {
        const Actor* other_actor = ctx.scene.find(other);
        if (other_actor != nullptr && other_actor->name() == ctx.text("collector", "Player")) {
            log_info(std::format("{} collected {}", other_actor->name(), ctx.actor.name()));
            ctx.destroy(ctx.actor.id());
        }
    }
};

template <typename T>
std::function<std::unique_ptr<Script>()> factory() {
    return [] { return std::make_unique<T>(); };
}

} // namespace

void register_builtin_scripts() {
    ScriptRegistry& registry = ScriptRegistry::instance();
    registry.add({"Rotator", "Spins the actor (degrees per second per axis).",
        {{"speed_x", 0.0f}, {"speed_y", 45.0f}, {"speed_z", 0.0f}}, factory<Rotator>()});
    registry.add({"Oscillator", "Moves back and forth along an axis.",
        {{"axis_x", 0.0f}, {"axis_y", 1.0f}, {"axis_z", 0.0f}, {"amplitude", 1.0f}, {"frequency", 0.5f}}, factory<Oscillator>()});
    registry.add({"PlayerController", "WASD/arrows to move (camera-relative), Space to jump, Shift to sprint.",
        {{"speed", 4.0f}, {"sprint_multiplier", 1.8f}, {"jump_speed", 5.5f}}, factory<PlayerController>()});
    registry.add({"FollowCamera", "Follows the named target from behind/above. Use on a camera actor.",
        {{"target", "Player"}, {"distance", 7.0f}, {"height", 3.5f}, {"yaw_degrees", 0.0f}, {"look_height", 0.8f}, {"smoothing", 6.0f}}, factory<FollowCamera>()});
    registry.add({"Collectible", "Spins; destroyed when the named collector touches it (use a trigger collider).",
        {{"collector", "Player"}, {"spin_degrees", 90.0f}}, factory<Collectible>()});
}

} // namespace Hollow

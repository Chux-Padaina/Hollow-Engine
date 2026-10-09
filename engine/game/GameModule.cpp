// Sample game module: gameplay scripts compiled into Game.dll, loaded by the
// editor at startup and hot-reloaded when you rebuild the HollowGame target
// (no editor restart). Add your own scripts here, or start a new module
// from this file. See include/Hollow/Scripting/GameModule.hpp.
#include "Hollow/Core/Input.hpp"
#include "Hollow/Core/Log.hpp"
#include "Hollow/Physics/PhysicsWorld.hpp"
#include "Hollow/Scripting/GameModule.hpp"

#include <cmath>
#include <format>

using namespace Hollow;

namespace {

// Launches whatever enters its trigger collider upwards. Put it on an actor
// with a trigger Collider.
class JumpPad final : public Script {
public:
    void on_trigger_enter(ScriptContext& ctx, ActorId other) override {
        Actor* actor = ctx.scene.find(other);
        if (actor == nullptr) {
            return;
        }
        const float speed = ctx.number("launch_speed", 9.0f);
        if (auto* character = actor->find_component<CharacterControllerComponent>()) {
            character->velocity.y = speed;
        } else if (auto* body = actor->find_component<RigidBodyComponent>(); body != nullptr && !body->is_kinematic) {
            body->velocity.y = speed;
        } else {
            return;
        }
        log_info(std::format("{} launched {}", ctx.actor.name(), actor->name()));
    }
};

// Bobs up and down and turns slowly (good on a kinematic platform or a
// pickup). Uses a raycast to keep a fixed height above whatever is below.
class Hover final : public Script {
public:
    void on_update(ScriptContext& ctx, float dt) override {
        ctx.actor.transform.rotation.y += ctx.number("turn_degrees", 25.0f) * kDegToRad * dt;
        const Vec3 position = ctx.actor.world_position();
        RaycastHit hit;
        float ground = position.y - ctx.number("height", 1.5f);
        if (ctx.physics.raycast(ctx.scene, {position, {0.0f, -1.0f, 0.0f}}, 50.0f, hit, ctx.actor.id())) {
            ground = hit.point.y;
        }
        const float bob = std::sin(ctx.time * ctx.number("bob_speed", 2.0f)) * ctx.number("bob_amount", 0.15f);
        const float target = ground + ctx.number("height", 1.5f) + bob;
        const float blend = 1.0f - std::exp(-8.0f * dt);
        ctx.scene.set_world_position(ctx.actor.id(), {position.x, position.y + (target - position.y) * blend, position.z});
    }
};

template <typename T>
std::function<std::unique_ptr<Script>()> factory() {
    return [] { return std::make_unique<T>(); };
}

} // namespace

HOLLOW_GAME_MODULE(registry) {
    registry.add({"JumpPad", "Launches bodies and characters that enter its trigger.", {{"launch_speed", 9.0f}}, factory<JumpPad>()});
    registry.add({"Hover", "Hovers above the ground, bobbing and turning.",
        {{"height", 1.5f}, {"bob_amount", 0.15f}, {"bob_speed", 2.0f}, {"turn_degrees", 25.0f}}, factory<Hover>()});
}

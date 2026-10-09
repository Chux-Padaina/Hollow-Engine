// Helpers that spawn common actors (primitives, lights, cameras, imported
// models) and build the starter scenes.
#pragma once

#include "Hollow/Assets/AssetManager.hpp"
#include "Hollow/Scene/Scene.hpp"

#include <string>

namespace Hollow {

Actor& spawn_primitive(Scene& scene, AssetManager& assets, BuiltinMesh mesh, const std::string& name, const Transform& transform);
Actor& spawn_light(Scene& scene, LightKind kind, const std::string& name, const Transform& transform);
Actor& spawn_camera(Scene& scene, const std::string& name, const Transform& transform);
// One actor with a part per model part, using each part's imported material.
// Skinned models also get an Animator.
Actor& spawn_model(Scene& scene, const ModelHandle& model, const std::string& name, const Transform& transform);
// Actors with one of the world/lighting components and sensible defaults.
Actor& spawn_component_actor(Scene& scene, ComponentType type, const std::string& name, const Transform& transform);

// Sizes a collider to the actor's mesh bounds (actor-local), or a unit
// shape if it has no mesh.
void fit_collider_to_mesh(const Actor& actor, ColliderComponent& collider);

// A few primitives on a ground plane under a physical sky with a sun.
void build_default_scene(Scene& scene, AssetManager& assets);
// Rendering showcase: terrain with grass, water, a lake-side material
// gallery (glass, car paint, velvet, brushed metal, ...), local lights,
// particles, a reflection probe, an irradiance volume and volumetric fog.
void build_showcase_scene(Scene& scene, AssetManager& assets);
// Physics + scripting showcase: a controllable player, a follow camera,
// falling crates, a spinning platform and collectibles.
void build_demo_scene(Scene& scene, AssetManager& assets);

} // namespace Hollow

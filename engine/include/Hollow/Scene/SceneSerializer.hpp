// Scene save/load (.hscene, JSON) and the snapshot format undo/redo and
// Play mode use. A scene file stores every actor (id, name, parent,
// transform, components), plus the active environment map, the editor
// camera, and the render settings. Mesh references are stored by source
// ("builtin" primitive name, or model path + part index) and resolved
// through the AssetManager on load; asset paths are written relative to the
// scene file when possible so a project folder can be moved.
#pragma once

#include "Hollow/Assets/AssetManager.hpp"
#include "Hollow/Core/Json.hpp"
#include "Hollow/Physics/PhysicsWorld.hpp"
#include "Hollow/Render/Camera.hpp"
#include "Hollow/Render/RenderSettings.hpp"
#include "Hollow/Scene/Scene.hpp"

#include <filesystem>
#include <string>

namespace Hollow {

// 2: physical light units, render settings v2 (nested), material extensions.
constexpr int kSceneFormatVersion = 2;

// Everything besides the actors that a scene file carries.
struct SceneDocument {
    std::string environment_path; // UTF-8, absolute; empty = none
    Camera camera;
    RenderPipelineSettings settings;
    PhysicsSettings physics;
};

// `base_dir` is the directory asset paths are made relative to (empty =
// keep absolute paths, which is what in-memory snapshots use).
JsonValue serialize_scene(const Scene& scene, const std::filesystem::path& base_dir = {});
// Replaces the scene's contents. Throws std::runtime_error on a malformed
// document; missing assets are replaced by placeholders and logged.
void deserialize_scene(Scene& scene, const JsonValue& json, AssetManager& assets, const std::filesystem::path& base_dir = {}, int version = kSceneFormatVersion);

JsonValue serialize_render_settings(const RenderPipelineSettings& settings, const std::filesystem::path& base_dir = {});
void deserialize_render_settings(RenderPipelineSettings& settings, const JsonValue& json, const std::filesystem::path& base_dir = {});
// Converts a version-1 render_settings object (and its light units).
void migrate_render_settings_v1(RenderPipelineSettings& settings, const JsonValue& json, bool has_environment);
JsonValue serialize_camera(const Camera& camera);
void deserialize_camera(Camera& camera, const JsonValue& json);

void save_scene_file(const std::filesystem::path& path, const Scene& scene, const SceneDocument& document);
// Loads into `scene` (replacing it) and returns the document extras.
SceneDocument load_scene_file(const std::filesystem::path& path, Scene& scene, AssetManager& assets);

} // namespace Hollow

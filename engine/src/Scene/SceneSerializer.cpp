// Implementation of SceneSerializer.hpp.
#include "Hollow/Scene/SceneSerializer.hpp"

#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/PropertiesJson.hpp"
#include "Hollow/Core/Log.hpp"
#include "Hollow/Scripting/Script.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace Hollow {
namespace {

JsonValue vec3_json(Vec3 v) {
    JsonValue array = JsonValue::array();
    array.push(v.x);
    array.push(v.y);
    array.push(v.z);
    return array;
}

Vec3 json_vec3(const JsonValue& json, Vec3 fallback) {
    if (!json.is_array() || json.size() < 3) {
        return fallback;
    }
    return {json.at(std::size_t{0}).as_float(fallback.x), json.at(std::size_t{1}).as_float(fallback.y), json.at(std::size_t{2}).as_float(fallback.z)};
}

std::string store_path(const std::string& utf8, const std::filesystem::path& base_dir) {
    if (utf8.empty()) {
        return {};
    }
    return make_relative_utf8(path_from_utf8(utf8), base_dir);
}

std::string load_path(const std::string& stored, const std::filesystem::path& base_dir) {
    if (stored.empty()) {
        return {};
    }
    return to_utf8(resolve_utf8(stored, base_dir));
}

JsonValue mesh_reference_json(const MeshHandle& mesh, const std::filesystem::path& base_dir) {
    JsonValue json = JsonValue::object();
    if (!mesh) {
        return json;
    }
    std::string path;
    int part = 0;
    if (mesh->source.rfind("builtin:", 0) == 0) {
        json.set("builtin", mesh->source.substr(8));
    } else if (parse_model_source(mesh->source, path, part)) {
        json.set("model", store_path(path, base_dir));
        json.set("part", part);
    }
    return json;
}

// A visible stand-in for a mesh whose source can't be loaded. It keeps the
// original source string, so saving the scene again doesn't lose the
// reference.
MeshHandle placeholder_mesh(AssetManager& assets, const std::string& source) {
    MeshAsset asset = *assets.builtin_mesh(BuiltinMesh::Cube);
    asset.source = source;
    asset.name = "Missing mesh";
    return make_mesh_asset(std::move(asset));
}

MeshHandle json_mesh_reference(const JsonValue& json, AssetManager& assets, const std::filesystem::path& base_dir) {
    if (const JsonValue* builtin = json.find("builtin")) {
        if (MeshHandle mesh = assets.builtin_mesh(builtin->as_string())) {
            return mesh;
        }
        log_warning(std::format("Unknown built-in mesh '{}'", builtin->as_string()));
        return assets.builtin_mesh(BuiltinMesh::Cube);
    }
    if (const JsonValue* model = json.find("model")) {
        const std::filesystem::path path = resolve_utf8(model->as_string(), base_dir);
        const int part = json.at("part").as_int(0);
        if (MeshHandle mesh = assets.model_mesh(path, part)) {
            return mesh;
        }
        return placeholder_mesh(assets, "model:" + to_utf8(path) + "#" + std::to_string(part));
    }
    return nullptr;
}

JsonValue component_json(const Component& component, const std::filesystem::path& base_dir) {
    JsonValue json = JsonValue::object();
    json.set("type", component_type_key(component.type()));
    switch (component.type()) {
    case ComponentType::StaticMesh: {
        const auto& mesh = static_cast<const StaticMeshComponent&>(component);
        json.set("visible", mesh.visible);
        json.set("cast_shadows", mesh.cast_shadows);
        JsonValue parts = JsonValue::array();
        for (const MeshPart& part : mesh.parts) {
            JsonValue part_json = JsonValue::object();
            part_json.set("name", part.name);
            part_json.set("mesh", mesh_reference_json(part.mesh, base_dir));
            part_json.set("material", properties_to_json(part.material, base_dir));
            parts.push(std::move(part_json));
        }
        json.set("parts", std::move(parts));
        break;
    }
    case ComponentType::Script: {
        const auto& script = static_cast<const ScriptComponent&>(component);
        json.set("script", script.script_name);
        JsonValue params = JsonValue::object();
        for (const ScriptParam& param : script.params) {
            if (param.is_text) {
                params.set(param.name, param.text);
            } else {
                params.set(param.name, param.number);
            }
        }
        json.set("params", std::move(params));
        break;
    }
    default: {
        JsonWriteVisitor writer(json, base_dir);
        const_cast<Component&>(component).describe(writer); // the writer never modifies
        break;
    }
    }
    return json;
}

// v1 scenes used arbitrary light units; v2 lights are physical. With the
// migrated scene's manual exposure of EV100 13 (see migrate_render_settings)
// these factors keep the old look.
constexpr float kLegacyLightScale = 9830.4f; // 1.2 * 2^13
constexpr float kLegacyPointDistanceSq = 2.25f;

void migrate_light_v1(LightComponent& light, const JsonValue& json) {
    const float old_intensity = json.at("intensity").as_float(1.0f);
    if (light.light_kind == LightKind::Directional) {
        light.intensity = old_intensity * kLegacyLightScale;
        light.source_size = 0.53f;
    } else {
        const float candela = old_intensity * kLegacyLightScale * kLegacyPointDistanceSq;
        light.intensity = candela * 4.0f * kPi;
    }
}

std::unique_ptr<Component> json_component(const JsonValue& json, AssetManager& assets, const std::filesystem::path& base_dir, int version) {
    const std::string key = json.at("type").as_string();
    ComponentType type{};
    if (!component_type_from_key(key, type)) {
        log_warning(std::format("Skipping unknown component type '{}'", key));
        return nullptr;
    }
    if (type == ComponentType::StaticMesh) {
        auto mesh = std::make_unique<StaticMeshComponent>();
        mesh->visible = json.at("visible").as_bool(true);
        mesh->cast_shadows = json.at("cast_shadows").as_bool(true);
        for (const JsonValue& part_json : json.at("parts").array_value) {
            MeshPart part;
            part.name = part_json.at("name").as_string();
            part.mesh = json_mesh_reference(part_json.at("mesh"), assets, base_dir);
            properties_from_json(part.material, part_json.at("material"), base_dir);
            if (part.mesh) {
                mesh->parts.push_back(std::move(part));
            }
        }
        return mesh;
    }
    if (type == ComponentType::Script) {
        auto script = std::make_unique<ScriptComponent>();
        script->script_name = json.at("script").as_string();
        for (const auto& [name, value] : json.at("params").object_value) {
            ScriptParam param;
            param.name = name;
            param.is_text = value.is_string();
            param.text = value.as_string();
            param.number = value.as_float();
            script->params.push_back(std::move(param));
        }
        ScriptRegistry::instance().fill_missing_params(*script);
        return script;
    }
    std::unique_ptr<Component> component = create_component(type);
    if (!component) {
        return nullptr;
    }
    JsonReadVisitor reader(json, base_dir);
    component->describe(reader);
    if (version < 2 && type == ComponentType::Light) {
        migrate_light_v1(static_cast<LightComponent&>(*component), json);
    }
    return component;
}

} // namespace

JsonValue serialize_scene(const Scene& scene, const std::filesystem::path& base_dir) {
    JsonValue json = JsonValue::object();
    json.set("selected", scene.selected_actor_id());
    JsonValue actors = JsonValue::array();
    // Write parents before children so loading can attach immediately.
    std::vector<ActorId> order;
    std::vector<ActorId> stack;
    const std::vector<ActorId> roots = scene.root_actors();
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        stack.push_back(*it);
    }
    while (!stack.empty()) {
        const ActorId id = stack.back();
        stack.pop_back();
        order.push_back(id);
        if (const Actor* actor = scene.find(id)) {
            for (auto it = actor->children().rbegin(); it != actor->children().rend(); ++it) {
                stack.push_back(*it);
            }
        }
    }
    for (ActorId id : order) {
        const Actor* actor = scene.find(id);
        if (actor == nullptr) {
            continue;
        }
        JsonValue actor_json = JsonValue::object();
        actor_json.set("id", actor->id());
        actor_json.set("name", actor->name());
        actor_json.set("parent", actor->parent());
        actor_json.set("active", actor->active);
        JsonValue transform = JsonValue::object();
        transform.set("position", vec3_json(actor->transform.translation));
        transform.set("rotation_degrees", vec3_json(actor->transform.rotation * kRadToDeg));
        transform.set("scale", vec3_json(actor->transform.scale));
        actor_json.set("transform", std::move(transform));
        JsonValue components = JsonValue::array();
        for (const auto& component : actor->components()) {
            components.push(component_json(*component, base_dir));
        }
        actor_json.set("components", std::move(components));
        actors.push(std::move(actor_json));
    }
    json.set("actors", std::move(actors));
    return json;
}

void deserialize_scene(Scene& scene, const JsonValue& json, AssetManager& assets, const std::filesystem::path& base_dir, int version) {
    if (!json.is_object() || !json.at("actors").is_array()) {
        throw std::runtime_error("scene data has no 'actors' array");
    }
    Scene loaded;
    std::vector<std::pair<ActorId, ActorId>> parent_links;
    for (const JsonValue& actor_json : json.at("actors").array_value) {
        const int raw_id = actor_json.at("id").as_int(0);
        if (raw_id <= 0 || loaded.find(static_cast<ActorId>(raw_id)) != nullptr) {
            throw std::runtime_error("scene has an actor with a missing or duplicate id");
        }
        Actor& actor = loaded.create_actor_with_id(static_cast<ActorId>(raw_id), actor_json.at("name").as_string("Actor"));
        actor.active = actor_json.at("active").as_bool(true);
        const JsonValue& transform = actor_json.at("transform");
        actor.transform.translation = json_vec3(transform.at("position"), {});
        actor.transform.rotation = json_vec3(transform.at("rotation_degrees"), {}) * kDegToRad;
        actor.transform.scale = json_vec3(transform.at("scale"), Vec3::one());
        for (const JsonValue& component_json_value : actor_json.at("components").array_value) {
            if (auto component = json_component(component_json_value, assets, base_dir, version)) {
                actor.add_component(std::move(component));
            }
        }
        const int parent = actor_json.at("parent").as_int(0);
        if (parent > 0) {
            parent_links.emplace_back(actor.id(), static_cast<ActorId>(parent));
        }
    }
    for (const auto& [child, parent] : parent_links) {
        if (!loaded.set_parent(child, parent, false)) {
            log_warning(std::format("Actor {} has an invalid parent {}; left at the root", child, parent));
        }
    }
    loaded.select_actor(static_cast<ActorId>(std::max(0, json.at("selected").as_int(0))));
    loaded.update_world_transforms();
    scene = std::move(loaded);
}

JsonValue serialize_render_settings(const RenderPipelineSettings& settings, const std::filesystem::path& base_dir) {
    return properties_to_json(settings, base_dir);
}

void deserialize_render_settings(RenderPipelineSettings& settings, const JsonValue& json, const std::filesystem::path& base_dir) {
    properties_from_json(settings, json, base_dir);
}

void migrate_render_settings_v1(RenderPipelineSettings& s, const JsonValue& json, bool has_environment) {
    s = RenderPipelineSettings{};
    s.shadows.enabled = json.at("shadows").as_bool(true);
    const float exposure = std::max(json.at("exposure").as_float(1.0f), 0.01f);
    s.exposure.mode = ExposureMode::Manual;
    s.exposure.ev100 = 13.0f - std::log2(exposure);
    s.gi.ibl = json.at("ibl").as_bool(true);
    s.sky.mode = has_environment ? SkyMode::Hdri : SkyMode::Atmosphere;
    s.sky.hdri_intensity = kLegacyLightScale;
    s.sky.background_intensity = json.at("sky_intensity").as_float(0.85f);
    s.post.bloom = json.at("bloom").as_bool(true);
    s.post.vignette = json.at("vignette").as_bool(true) ? json.at("vignette_intensity").as_float(0.35f) * 0.7f : 0.0f;
    s.post.contrast = json.at("contrast").as_float(1.0f);
    s.post.saturation = json.at("saturation").as_float(1.0f);
    s.post.temperature = json.at("temperature").as_float(0.0f);
    s.gi.ambient_occlusion = json.at("ao").as_bool(true);
    s.post.tonemapper = Tonemapper::Aces;
}

JsonValue serialize_camera(const Camera& camera) {
    JsonValue json = JsonValue::object();
    json.set("target", vec3_json(camera.target));
    json.set("yaw", camera.yaw);
    json.set("pitch", camera.pitch);
    json.set("distance", camera.distance);
    json.set("fov_degrees", camera.fov_y_radians * kRadToDeg);
    return json;
}

void deserialize_camera(Camera& camera, const JsonValue& json) {
    camera.target = json_vec3(json.at("target"), camera.target);
    camera.yaw = json.at("yaw").as_float(camera.yaw);
    camera.pitch = json.at("pitch").as_float(camera.pitch);
    camera.distance = json.at("distance").as_float(camera.distance);
    camera.fov_y_radians = json.at("fov_degrees").as_float(camera.fov_y_radians * kRadToDeg) * kDegToRad;
    camera.clamp();
}

void save_scene_file(const std::filesystem::path& path, const Scene& scene, const SceneDocument& document) {
    const std::filesystem::path base_dir = path.parent_path();
    JsonValue json = JsonValue::object();
    json.set("format", "hollow-scene");
    json.set("version", kSceneFormatVersion);
    json.set("scene", serialize_scene(scene, base_dir));
    json.set("environment", store_path(document.environment_path, base_dir));
    json.set("camera", serialize_camera(document.camera));
    json.set("render_settings", serialize_render_settings(document.settings, base_dir));
    json.set("physics", properties_to_json(document.physics));
    write_text_file(path, write_json(json, true));
}

SceneDocument load_scene_file(const std::filesystem::path& path, Scene& scene, AssetManager& assets) {
    const JsonValue json = parse_json(read_text_file(path));
    if (json.at("format").as_string() != "hollow-scene") {
        throw std::runtime_error("not a Hollow scene file");
    }
    const int version = json.at("version").as_int(0);
    if (version > kSceneFormatVersion) {
        throw std::runtime_error(std::format("scene format version {} is newer than this engine supports ({})", version, kSceneFormatVersion));
    }
    const std::filesystem::path base_dir = path.parent_path();
    SceneDocument document;
    deserialize_scene(scene, json.at("scene"), assets, base_dir, version);
    document.environment_path = load_path(json.at("environment").as_string(), base_dir);
    deserialize_camera(document.camera, json.at("camera"));
    if (version < 2) {
        migrate_render_settings_v1(document.settings, json.at("render_settings"), !document.environment_path.empty());
    } else {
        deserialize_render_settings(document.settings, json.at("render_settings"), base_dir);
    }
    if (json.at("physics").is_object()) {
        properties_from_json(document.physics, json.at("physics"));
    }
    return document;
}

} // namespace Hollow

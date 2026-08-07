// Implementation of the Engine/SystemRegistry declared in Engine.hpp: enum
// name lookups for the editor UI, the placeholder system registration list,
// and the editor tool/transform-space state transitions.
#include "Hollow/Engine.hpp"

namespace Hollow {

const char* name_of(StylePreset style) {
    switch (style) {
    case StylePreset::Character:
        return "Character";
    case StylePreset::Street:
        return "Street";
    case StylePreset::Combat:
        return "Combat";
    }
    return "Unknown";
}

const char* name_of(DebugView view) {
    switch (view) {
    case DebugView::Final:
        return "Final";
    case DebugView::Albedo:
        return "Albedo";
    case DebugView::Normals:
        return "Normals";
    case DebugView::LightingOnly:
        return "Lighting";
    case DebugView::Roughness:
        return "Roughness";
    case DebugView::Metallic:
        return "Metallic";
    case DebugView::Occlusion:
        return "Occlusion";
    case DebugView::Emissive:
        return "Emissive";
    }
    return "Unknown";
}

void SystemRegistry::register_placeholder(std::string name) {
    systems_.push_back(std::move(name));
}

Engine::Engine() {
    systems_.register_placeholder("Asset Registry");
    systems_.register_placeholder("Renderer");
    systems_.register_placeholder("Editor Selection");
    systems_.register_placeholder("Input");
    systems_.register_placeholder("Physics");
    systems_.register_placeholder("Audio");
    systems_.register_placeholder("Animation");
    systems_.register_placeholder("Scripting");
    systems_.register_placeholder("UI");
    systems_.register_placeholder("Post Processing");
    systems_.register_placeholder("Save/Load");
}

void Engine::tick(float) {}

void Engine::toggle_transform_space() {
    transform_space_ = transform_space_ == TransformSpace::World ? TransformSpace::Local : TransformSpace::World;
}

} // namespace Hollow

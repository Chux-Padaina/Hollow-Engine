#include "Forge/Engine.hpp"

namespace Forge {

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

} // namespace Forge

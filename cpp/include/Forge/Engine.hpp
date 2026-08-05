#pragma once

#include <string>
#include <vector>

namespace Forge {

class Scene;

enum class EditorTool {
    Select,
    Move,
    Rotate,
    Scale,
};

enum class TransformSpace {
    Local,
    World,
};

struct RenderPipelineSettings {
    bool draw_world_grid = true;
    bool draw_actor_outlines = true;
    bool draw_editor_gizmos = true;
    bool shadows_enabled = false;
    bool pbr_enabled = false;
};

class SystemRegistry {
public:
    void register_placeholder(std::string name);
    const std::vector<std::string>& systems() const { return systems_; }

private:
    std::vector<std::string> systems_;
};

class Engine {
public:
    Engine();

    void tick(float delta_seconds);

    SystemRegistry& systems() { return systems_; }
    const SystemRegistry& systems() const { return systems_; }

    RenderPipelineSettings& render_settings() { return render_settings_; }
    const RenderPipelineSettings& render_settings() const { return render_settings_; }

    EditorTool active_tool() const { return active_tool_; }
    void set_active_tool(EditorTool tool) { active_tool_ = tool; }

    TransformSpace transform_space() const { return transform_space_; }
    void toggle_transform_space();

private:
    SystemRegistry systems_;
    RenderPipelineSettings render_settings_;
    EditorTool active_tool_ = EditorTool::Move;
    TransformSpace transform_space_ = TransformSpace::World;
};

} // namespace Forge

// Core engine state shared between the editor and the renderer: the active
// editor tool (select/move/rotate/scale) and transform space, plus
// RenderPipelineSettings, the set of renderer toggles/sliders (HDR, bloom,
// AO, shadows, IBL, color grading, FXAA, ...) that the editor UI binds to
// and that Renderer reads each frame to drive the render pipeline.
#pragma once

#include <string>
#include <vector>

namespace Hollow {

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

enum class StylePreset {
    Character,
    Street,
    Combat,
};

enum class DebugView {
    Final,
    Albedo,
    Normals,
    LightingOnly,
    Roughness,
    Metallic,
    Occlusion,
    Emissive,
};

const char* name_of(StylePreset style);
const char* name_of(DebugView view);

struct RenderPipelineSettings {
    bool draw_world_grid = true;
    bool draw_actor_outlines = true;
    bool draw_editor_gizmos = true;
    bool shadows_enabled = true;
    bool hdr_enabled = true;
    float exposure = 1.0f;
    bool pbr_enabled = false;
    // Gates both the skybox draw and IBL sampling in the object shader; has
    // no visible effect until an environment is loaded via
    // Renderer::load_environment (drag-and-drop a .hdr file onto the
    // window), since there's nothing to sample before that.
    bool ibl_enabled = true;
    // Multiplier applied directly to the sampled environment cubemap color
    // in the skybox shader (see skybox_fragment_shader), independent of
    // exposure. Most HDRI environments encode the sun/sky at very high raw
    // radiance, which reads as an overly bright background and feeds a lot
    // of energy into the bloom threshold pass; 0.85 trims that down a
    // notch without dimming the actual scene lighting/exposure, which still
    // comes from `exposure` above. Tune via the "Sky" slider in the
    // toolbar, next to Exposure.
    float sky_intensity = 0.85f;

    // --- Post-processing (milestone 6) --------------------------------
    // All of the fields below only take effect when hdr_enabled is true —
    // bloom, vignette, and color grading are applied in the HDR tonemap
    // pass (Renderer::render_tonemapped_frame), which doesn't run at all
    // with HDR off (see Renderer::end_scene_color_pass).
    bool bloom_enabled = true;
    // Scene-color luminance above this level starts contributing to
    // bloom, with a small soft-knee band above the threshold so the
    // effect fades in instead of clipping on/off between neighboring
    // pixels of similar brightness (see bloom_threshold_fragment_shader).
    // Raised from an initial 1.0, then again from 1.2 — bright HDRI skies
    // and their silhouette edges were still blooming more than intended
    // even after the first pass of tuning.
    float bloom_threshold = 1.35f;
    // Additive blend strength applied when the blurred bloom result is
    // combined back onto the scene in the tonemap pass. Lowered from an
    // initial 0.6 default, then 0.3 — still read as a bit heavy around
    // bright skyboxes/highlights, so trimmed once more to 0.22. Tune live
    // via the "Post FX" toolbar window's Bloom section.
    float bloom_intensity = 0.22f;
    bool vignette_enabled = true;
    float vignette_intensity = 0.35f;
    // Parametric color grading (contrast/saturation/temperature), applied
    // in LDR space after tonemapping and gamma. A texture-based 3D LUT is
    // the more common AAA approach but is deferred — see the milestone 6
    // scoping note in AAA_PBR_ROADMAP.md.
    float color_grade_contrast = 1.0f;
    float color_grade_saturation = 1.0f;
    // -1 (cooler/blue) .. 0 (neutral) .. 1 (warmer/orange).
    float color_grade_temperature = 0.0f;

    // Screen-space ambient occlusion. Independent of HDR — it darkens the
    // ambient/IBL term directly in the object shader (see the `occlusion`
    // variable in fragment_shader, Renderer.cpp), the same place the
    // glTF occlusion-texture term is applied, so it still has an effect
    // with HDR off (unlike bloom/vignette/grading above, which only run in
    // the HDR tonemap pass).
    bool ao_enabled = true;
    // World-space sample radius for the hemisphere kernel — how far out
    // occluders are considered. Should scale roughly with scene size.
    float ao_radius = 0.5f;
    // Depth-comparison bias (world units) that keeps a surface from
    // occluding itself; too small causes "acne"-like self-shadowing on flat
    // surfaces, too large lets real contact shadows disappear.
    float ao_bias = 0.02f;
    // Post-multiplies the raw occlusion estimate; 1.0 is the "physically
    // computed" strength, values above that exaggerate contact shadows for
    // a more stylized look.
    float ao_intensity = 1.0f;

    // FXAA edge antialiasing — a single-pass, luma-edge-directed blend
    // (Renderer::execute_fxaa_pass/fxaa_fragment_shader), not the full
    // NVIDIA FXAA 3.11 reference algorithm (that one adds a per-edge
    // search along the edge's length and a sub-pixel aliasing term, both
    // skipped here — see the milestone 6 note in AAA_PBR_ROADMAP.md).
    // Runs on the tonemapped LDR image, so — like bloom/vignette/grading —
    // it only takes effect with hdr_enabled on.
    bool fxaa_enabled = true;
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

    StylePreset style() const { return style_; }
    void set_style(StylePreset style) { style_ = style; }

    DebugView debug_view() const { return debug_view_; }
    void set_debug_view(DebugView view) { debug_view_ = view; }

    TransformSpace transform_space() const { return transform_space_; }
    void toggle_transform_space();

private:
    SystemRegistry systems_;
    RenderPipelineSettings render_settings_;
    EditorTool active_tool_ = EditorTool::Move;
    TransformSpace transform_space_ = TransformSpace::World;
    StylePreset style_ = StylePreset::Street;
    DebugView debug_view_ = DebugView::Final;
};

} // namespace Hollow

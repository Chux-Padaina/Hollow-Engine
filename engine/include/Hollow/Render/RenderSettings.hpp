// Renderer-facing settings and per-frame view options.
//
// RenderPipelineSettings is the scene's "post-process volume": everything
// about how the scene is lit and finished (shadows, exposure, sky,
// fog, GI, anti-aliasing, post effects, performance). It is saved with the
// scene and edited from the editor's Render panel; both go through the
// describe() functions below (see Core/Properties.hpp), so a new setting
// only needs a field and one describe() line.
//
// Units: lights are physical (lux / lumens, see LightComponent), so the
// camera exposure decides how bright the image is. The default is
// automatic exposure; switch to Manual to lock an EV100 value.
#pragma once

#include "Hollow/Core/Properties.hpp"
#include "Hollow/Scene/Scene.hpp"

#include <string>

namespace Hollow {

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

enum class DebugView {
    Final,
    Albedo,
    Normals,
    DirectLighting,
    Roughness,
    Metallic,
    AmbientOcclusion,
    Emissive,
    IndirectLighting,
    Reflections,
    Velocity,
    LightComplexity,
    ShadowCascades,
    LodLevels,
    Overdraw,
    Count,
};

const char* debug_view_name(DebugView view);

enum class AntiAliasing {
    None,
    Fxaa,
    Taa,
};

enum class Tonemapper {
    Aces,
    AgX,
    AgXPunchy,
    KhronosNeutral,
};

enum class ExposureMode {
    Automatic,
    Manual,
};

enum class SkyMode {
    Atmosphere, // physically based sky driven by the first directional light
    Hdri,       // the loaded .hdr environment
    Color,      // a flat color (uniform ambient)
};

enum class ShadowFilter {
    Pcf,  // fixed-width soft edges
    Pcss, // contact-hardening: sharp near the caster, softer far away
};

enum class TransparencyMode {
    Sorted,      // back-to-front per object
    WeightedOit, // order-independent (weighted blended)
};

struct ShadowSettings {
    bool enabled = true;
    int cascade_resolution = 2048;
    float distance = 80.0f;      // directional shadow range from the camera
    float split_lambda = 0.78f;  // 0 uniform .. 1 logarithmic cascade splits
    float cascade_blend = 0.12f; // fraction of each cascade blended into the next
    int atlas_resolution = 4096; // spot/point shadow atlas
    int max_shadowed_lights = 12;
    ShadowFilter filter = ShadowFilter::Pcss;
    float softness = 1.0f; // scales every light's source size
    float depth_bias = 1.0f;
    float normal_bias = 1.0f;
    bool contact_shadows = true;
    float contact_length = 0.3f; // world units
};

struct ExposureSettings {
    ExposureMode mode = ExposureMode::Automatic;
    float ev100 = 13.0f;       // manual exposure
    float compensation = 0.0f; // EV stops, applied in both modes
    float min_ev100 = -2.0f;   // automatic range
    float max_ev100 = 17.0f;
    float speed_up = 3.0f;     // adaptation speed (stops/second-ish)
    float speed_down = 1.5f;
};

struct SkySettings {
    SkyMode mode = SkyMode::Atmosphere;
    // HDRI: luminance (cd/m^2) of a pixel value of 1.0. Most .hdr files are
    // relative, so this calibrates them against the physical lights.
    float hdri_intensity = 12000.0f;
    float hdri_rotation = 0.0f; // radians around Y
    // Background-only multiplier (lighting unaffected).
    float background_intensity = 1.0f;
    Vec3 color{0.45f, 0.55f, 0.7f};
    float color_intensity = 8000.0f; // Color mode luminance
    // Atmosphere
    float haze = 1.0f;           // Mie (aerosol) density multiplier
    float air_density = 1.0f;    // Rayleigh density multiplier
    float ozone = 1.0f;
    Vec3 ground_albedo{0.3f, 0.3f, 0.3f};
    bool sun_disk = true;
};

struct FogSettings {
    bool enabled = false;
    bool volumetric = true;  // froxel fog with light shafts; off = analytic height fog
    float density = 0.015f;  // extinction at base height (1/m)
    float height_falloff = 0.15f;
    float base_height = 0.0f;
    Vec3 albedo{0.9f, 0.92f, 1.0f};
    float anisotropy = 0.6f; // phase g: forward scattering
    float max_distance = 120.0f;
    float ambient = 1.0f;    // sky ambient contribution
    float noise = 0.5f;      // 3D noise variation
    Vec3 wind{0.6f, 0.0f, 0.25f};
};

struct GiSettings {
    bool ibl = true;          // sky/HDRI + reflection probes + irradiance volumes
    bool ambient_occlusion = true;
    float ao_radius = 1.2f;   // world units
    float ao_intensity = 1.0f;
    int ao_slices = 2;        // GTAO directions per pixel
    bool reflection_probes = true;
    bool irradiance_volumes = true;
    bool ssr = true;
    float ssr_max_roughness = 0.65f;
    float ssr_thickness = 0.25f;
    int ssr_steps = 48;
    float specular_occlusion = 1.0f;
};

struct AntiAliasingSettings {
    AntiAliasing method = AntiAliasing::Taa;
    // Internal resolution. Below 1 the TAA pass upsamples (temporal
    // upscaling); with FXAA/None the image is upscaled bilinearly.
    float render_scale = 1.0f;
    float sharpness = 0.35f;   // contrast-adaptive sharpening after TAA
    float taa_history = 0.9f;  // history weight
    bool specular_aa = true;   // geometric specular anti-aliasing
};

struct PostSettings {
    Tonemapper tonemapper = Tonemapper::AgX;
    bool bloom = true;
    float bloom_intensity = 0.04f;
    float bloom_radius = 0.75f;
    float lens_dirt = 0.0f;

    bool depth_of_field = false;
    float focus_distance = 8.0f; // meters
    float f_stop = 2.8f;         // focal length follows the camera FOV (full-frame sensor)
    bool autofocus = false;      // focus on the screen center

    bool motion_blur = false;
    float shutter_angle = 180.0f; // degrees; 360 = full frame time
    int motion_blur_samples = 10;

    float chromatic_aberration = 0.0f;
    float vignette = 0.25f;
    float film_grain = 0.0f;

    // Grading (applied in a 3D LUT together with the tonemapper).
    float temperature = 0.0f; // -1 cool .. 1 warm
    float tint = 0.0f;        // -1 green .. 1 magenta
    float contrast = 1.0f;
    float saturation = 1.0f;
    Vec3 lift{0.0f, 0.0f, 0.0f};
    Vec3 gamma{1.0f, 1.0f, 1.0f};
    Vec3 gain{1.0f, 1.0f, 1.0f};
    std::string lut_path;       // optional .cube LUT applied after grading
    float lut_contribution = 1.0f;
};

struct TransparencySettings {
    TransparencyMode mode = TransparencyMode::WeightedOit;
    bool refraction = true; // transmissive materials refract the scene behind
};

struct PerformanceSettings {
    bool gpu_culling = true;       // compute-shader culling + indirect draws
    bool occlusion_culling = true; // two-phase Hi-Z occlusion
    bool frustum_culling = true;
    bool lods = true;
    float lod_bias = 1.0f;         // >1 keeps detailed LODs longer
    int max_lights = 1024;
    bool async_textures = true;    // decode textures on a worker thread
    bool texture_compression = true; // BC7 (cached on disk)
};

struct RenderPipelineSettings {
    ShadowSettings shadows;
    ExposureSettings exposure;
    SkySettings sky;
    FogSettings fog;
    GiSettings gi;
    AntiAliasingSettings aa;
    PostSettings post;
    TransparencySettings transparency;
    PerformanceSettings performance;
};

// Property descriptions (serialization + the editor's Render panel).
void describe(ShadowSettings& settings, PropertyVisitor& v);
void describe(ExposureSettings& settings, PropertyVisitor& v);
void describe(SkySettings& settings, PropertyVisitor& v);
void describe(FogSettings& settings, PropertyVisitor& v);
void describe(GiSettings& settings, PropertyVisitor& v);
void describe(AntiAliasingSettings& settings, PropertyVisitor& v);
void describe(PostSettings& settings, PropertyVisitor& v);
void describe(TransparencySettings& settings, PropertyVisitor& v);
void describe(PerformanceSettings& settings, PropertyVisitor& v);
void describe(RenderPipelineSettings& settings, PropertyVisitor& v);

// Editor overlay state for one frame.
struct RenderView {
    DebugView debug_view = DebugView::Final;
    EditorTool tool = EditorTool::Move;
    TransformSpace space = TransformSpace::World;
    ActorId selected = kNoActor;
    ActorId hovered_gizmo_axis = 0; // 0 none, 1 x, 2 y, 3 z, 4 center
    bool draw_grid = true;
    bool draw_gizmos = true;
    bool draw_outlines = true;
    bool draw_icons = true;      // light/camera/probe markers
    bool draw_colliders = false;         // every collider
    bool draw_selection_collider = true; // just the selected actor's
    bool editor = true; // false in Play mode with the game camera
};

} // namespace Hollow

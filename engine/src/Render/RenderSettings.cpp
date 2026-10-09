// Property descriptions for RenderPipelineSettings (see RenderSettings.hpp).
// The JSON keys here are the scene file format; keep them stable.
#include "Hollow/Render/RenderSettings.hpp"

#include <array>

namespace Hollow {

const char* debug_view_name(DebugView view) {
    switch (view) {
    case DebugView::Final: return "Final";
    case DebugView::Albedo: return "Albedo";
    case DebugView::Normals: return "Normals";
    case DebugView::DirectLighting: return "Direct lighting";
    case DebugView::Roughness: return "Roughness";
    case DebugView::Metallic: return "Metallic";
    case DebugView::AmbientOcclusion: return "Ambient occlusion";
    case DebugView::Emissive: return "Emissive";
    case DebugView::IndirectLighting: return "Indirect lighting";
    case DebugView::Reflections: return "Reflections";
    case DebugView::Velocity: return "Motion vectors";
    case DebugView::LightComplexity: return "Light complexity";
    case DebugView::ShadowCascades: return "Shadow cascades";
    case DebugView::LodLevels: return "LOD levels";
    case DebugView::Overdraw: return "Instances";
    case DebugView::Count: break;
    }
    return "Unknown";
}

namespace {
constexpr std::array<const char*, 3> kAntiAliasingNames{"None", "FXAA", "TAA"};
constexpr std::array<const char*, 4> kTonemapperNames{"ACES", "AgX", "AgX Punchy", "Khronos Neutral"};
constexpr std::array<const char*, 2> kExposureNames{"Automatic", "Manual"};
constexpr std::array<const char*, 3> kSkyNames{"Atmosphere", "HDRI", "Color"};
constexpr std::array<const char*, 2> kShadowFilterNames{"PCF", "PCSS"};
constexpr std::array<const char*, 2> kTransparencyNames{"Sorted", "Weighted OIT"};
} // namespace

void describe(ShadowSettings& s, PropertyVisitor& v) {
    v.boolean("enabled", "Shadows", s.enabled);
    if (!v.show(s.enabled)) {
        return;
    }
    v.enumeration("filter", "Filter", s.filter, kShadowFilterNames, "PCSS: contact-hardening soft shadows sized by each light's source radius.");
    v.number("softness", "Softness", s.softness, {0.0f, 4.0f, 0.01f, "%.2f"}, "Scales every light's source size.");
    v.number("distance", "Distance", s.distance, {10.0f, 500.0f, 1.0f, "%.0f m"}, "How far from the camera the sun casts shadows.");
    v.integer("cascade_resolution", "Cascade size", s.cascade_resolution, 512, 4096, "Pixels per cascade (4 cascades).");
    v.number("split_lambda", "Split curve", s.split_lambda, {0.0f, 1.0f, 0.01f, "%.2f"}, "0 = even cascade splits, 1 = logarithmic.");
    v.number("cascade_blend", "Cascade blend", s.cascade_blend, {0.0f, 0.4f, 0.01f, "%.2f"});
    v.integer("atlas_resolution", "Atlas size", s.atlas_resolution, 1024, 8192, "Shadow atlas shared by spot and point lights.");
    v.integer("max_shadowed_lights", "Shadowed lights", s.max_shadowed_lights, 0, 64, "Most important spot/point lights that get atlas tiles.");
    v.number("depth_bias", "Depth bias", s.depth_bias, {0.0f, 5.0f, 0.01f, "%.2f"});
    v.number("normal_bias", "Normal bias", s.normal_bias, {0.0f, 5.0f, 0.01f, "%.2f"});
    v.boolean("contact_shadows", "Contact shadows", s.contact_shadows, "Screen-space ray march toward the sun for small-scale shadowing.");
    if (v.show(s.contact_shadows)) {
        v.number("contact_length", "Contact length", s.contact_length, {0.02f, 2.0f, 0.01f, "%.2f m"});
    }
}

void describe(ExposureSettings& s, PropertyVisitor& v) {
    v.enumeration("mode", "Mode", s.mode, kExposureNames, "Automatic adapts to the scene's luminance histogram.");
    if (v.show(s.mode == ExposureMode::Manual)) {
        v.number("ev100", "EV100", s.ev100, {-6.0f, 20.0f, 0.05f, "%.2f"}, "Sunny day ~15, overcast ~12, indoors ~7, night ~0.");
    }
    v.number("compensation", "Compensation", s.compensation, {-5.0f, 5.0f, 0.05f, "%+.2f EV"});
    if (v.show(s.mode == ExposureMode::Automatic)) {
        v.number("min_ev100", "Min EV100", s.min_ev100, {-6.0f, 20.0f, 0.1f, "%.1f"});
        v.number("max_ev100", "Max EV100", s.max_ev100, {-6.0f, 20.0f, 0.1f, "%.1f"});
        v.number("speed_up", "Adapt (brighter)", s.speed_up, {0.1f, 10.0f, 0.1f, "%.1f"});
        v.number("speed_down", "Adapt (darker)", s.speed_down, {0.1f, 10.0f, 0.1f, "%.1f"});
    }
}

void describe(SkySettings& s, PropertyVisitor& v) {
    v.enumeration("mode", "Sky", s.mode, kSkyNames, "Atmosphere is lit by the first directional light (the sun).");
    if (v.show(s.mode == SkyMode::Hdri)) {
        v.number("hdri_intensity", "HDRI luminance", s.hdri_intensity, {100.0f, 100000.0f, 50.0f, "%.0f nits", true, true}, "Luminance of a pixel value of 1.0 in the .hdr.");
        v.angle("hdri_rotation_degrees", "HDRI rotation", s.hdri_rotation, -180.0f, 180.0f);
    }
    if (v.show(s.mode == SkyMode::Color)) {
        v.color("color", "Color", s.color);
        v.number("color_intensity", "Luminance", s.color_intensity, {10.0f, 100000.0f, 10.0f, "%.0f nits", true, true});
    }
    if (v.show(s.mode == SkyMode::Atmosphere)) {
        v.number("haze", "Haze", s.haze, {0.0f, 10.0f, 0.01f, "%.2f"}, "Aerosol (Mie) density.");
        v.number("air_density", "Air density", s.air_density, {0.0f, 5.0f, 0.01f, "%.2f"}, "Rayleigh scattering (the blue).");
        v.number("ozone", "Ozone", s.ozone, {0.0f, 5.0f, 0.01f, "%.2f"});
        v.color("ground_albedo", "Ground albedo", s.ground_albedo);
        v.boolean("sun_disk", "Sun disk", s.sun_disk);
    }
    v.number("background_intensity", "Background", s.background_intensity, {0.0f, 4.0f, 0.01f, "%.2f"}, "Background brightness only; lighting is unchanged.");
}

void describe(FogSettings& s, PropertyVisitor& v) {
    v.boolean("enabled", "Fog", s.enabled);
    if (!v.show(s.enabled)) {
        return;
    }
    v.boolean("volumetric", "Volumetric", s.volumetric, "Froxel fog: light shafts from the sun and local lights.");
    v.number("density", "Density", s.density, {0.0f, 0.3f, 0.0005f, "%.4f", true, true});
    v.number("height_falloff", "Height falloff", s.height_falloff, {0.0f, 2.0f, 0.005f, "%.3f"});
    v.number("base_height", "Base height", s.base_height, {-100.0f, 100.0f, 0.1f, "%.1f m", false});
    v.color("albedo", "Albedo", s.albedo);
    v.number("anisotropy", "Anisotropy", s.anisotropy, {-0.9f, 0.95f, 0.01f, "%.2f"}, "Positive = forward scattering (bright toward the sun).");
    v.number("max_distance", "Max distance", s.max_distance, {10.0f, 500.0f, 1.0f, "%.0f m"});
    v.number("ambient", "Ambient", s.ambient, {0.0f, 4.0f, 0.01f, "%.2f"});
    if (v.show(s.volumetric)) {
        v.number("noise", "Noise", s.noise, {0.0f, 1.0f, 0.01f, "%.2f"});
        v.vec3("wind", "Wind", s.wind, {-10.0f, 10.0f, 0.01f, "%.2f", false});
    }
}

void describe(GiSettings& s, PropertyVisitor& v) {
    v.boolean("ibl", "Image-based lighting", s.ibl, "Sky/HDRI ambient + reflections, probes and irradiance volumes.");
    v.boolean("reflection_probes", "Reflection probes", s.reflection_probes);
    v.boolean("irradiance_volumes", "Irradiance volumes", s.irradiance_volumes);
    v.boolean("ambient_occlusion", "GTAO", s.ambient_occlusion, "Ground-truth ambient occlusion.");
    if (v.show(s.ambient_occlusion)) {
        v.number("ao_radius", "AO radius", s.ao_radius, {0.05f, 5.0f, 0.01f, "%.2f m"});
        v.number("ao_intensity", "AO intensity", s.ao_intensity, {0.0f, 3.0f, 0.01f, "%.2f"});
        v.integer("ao_slices", "AO slices", s.ao_slices, 1, 4);
        v.number("specular_occlusion", "Specular occlusion", s.specular_occlusion, {0.0f, 1.0f, 0.01f, "%.2f"});
    }
    v.boolean("ssr", "Screen-space reflections", s.ssr);
    if (v.show(s.ssr)) {
        v.number("ssr_max_roughness", "SSR max roughness", s.ssr_max_roughness, {0.05f, 1.0f, 0.01f, "%.2f"});
        v.number("ssr_thickness", "SSR thickness", s.ssr_thickness, {0.01f, 2.0f, 0.01f, "%.2f m"});
        v.integer("ssr_steps", "SSR steps", s.ssr_steps, 8, 128);
    }
}

void describe(AntiAliasingSettings& s, PropertyVisitor& v) {
    v.enumeration("method", "Anti-aliasing", s.method, kAntiAliasingNames);
    v.number("render_scale", "Render scale", s.render_scale, {0.5f, 1.0f, 0.01f, "%.2f"}, "Internal resolution. With TAA this is temporal upscaling.");
    if (v.show(s.method == AntiAliasing::Taa)) {
        v.number("sharpness", "Sharpness", s.sharpness, {0.0f, 1.0f, 0.01f, "%.2f"});
        v.number("taa_history", "History weight", s.taa_history, {0.5f, 0.98f, 0.005f, "%.3f"});
    }
    v.boolean("specular_aa", "Specular AA", s.specular_aa, "Widens roughness where normals vary quickly (less sparkle).");
}

void describe(PostSettings& s, PropertyVisitor& v) {
    v.enumeration("tonemapper", "Tonemapper", s.tonemapper, kTonemapperNames);
    if (PropertyGroup group{v, nullptr, "Bloom"}) {
        v.boolean("bloom", "Bloom", s.bloom);
        if (v.show(s.bloom)) {
            v.number("bloom_intensity", "Intensity", s.bloom_intensity, {0.0f, 0.5f, 0.001f, "%.3f"});
            v.number("bloom_radius", "Radius", s.bloom_radius, {0.0f, 1.0f, 0.01f, "%.2f"});
            v.number("lens_dirt", "Lens dirt", s.lens_dirt, {0.0f, 2.0f, 0.01f, "%.2f"});
        }
    }
    if (PropertyGroup group{v, nullptr, "Depth of field"}) {
        v.boolean("depth_of_field", "Depth of field", s.depth_of_field);
        if (v.show(s.depth_of_field)) {
            v.boolean("autofocus", "Autofocus", s.autofocus, "Focus on whatever is under the screen center.");
            if (v.show(!s.autofocus)) {
                v.number("focus_distance", "Focus distance", s.focus_distance, {0.1f, 200.0f, 0.05f, "%.2f m", true, true});
            }
            v.number("f_stop", "Aperture (f/)", s.f_stop, {0.95f, 22.0f, 0.05f, "%.1f", true, true}, "Lower = shallower focus. Focal length comes from the camera's field of view.");
        }
    }
    if (PropertyGroup group{v, nullptr, "Motion blur"}) {
        v.boolean("motion_blur", "Motion blur", s.motion_blur);
        if (v.show(s.motion_blur)) {
            v.number("shutter_angle", "Shutter angle", s.shutter_angle, {0.0f, 360.0f, 1.0f, "%.0f deg"});
            v.integer("motion_blur_samples", "Samples", s.motion_blur_samples, 4, 32);
        }
    }
    if (PropertyGroup group{v, nullptr, "Lens"}) {
        v.number("vignette", "Vignette", s.vignette, {0.0f, 1.0f, 0.01f, "%.2f"});
        v.number("chromatic_aberration", "Chromatic aberration", s.chromatic_aberration, {0.0f, 1.0f, 0.01f, "%.2f"});
        v.number("film_grain", "Film grain", s.film_grain, {0.0f, 1.0f, 0.01f, "%.2f"});
    }
    if (PropertyGroup group{v, nullptr, "Color grading"}) {
        v.number("temperature", "Temperature", s.temperature, {-1.0f, 1.0f, 0.01f, "%.2f"});
        v.number("tint", "Tint", s.tint, {-1.0f, 1.0f, 0.01f, "%.2f"});
        v.number("contrast", "Contrast", s.contrast, {0.5f, 1.5f, 0.01f, "%.2f"});
        v.number("saturation", "Saturation", s.saturation, {0.0f, 2.0f, 0.01f, "%.2f"});
        v.vec3("lift", "Lift", s.lift, {-0.2f, 0.2f, 0.001f, "%.3f", false});
        v.vec3("gamma", "Gamma", s.gamma, {0.5f, 2.0f, 0.005f, "%.3f", false});
        v.vec3("gain", "Gain", s.gain, {0.5f, 2.0f, 0.005f, "%.3f", false});
        v.path("lut", "LUT (.cube)", s.lut_path, "Adobe Cube LUT (*.cube)");
        if (v.show(!s.lut_path.empty())) {
            v.number("lut_contribution", "LUT amount", s.lut_contribution, {0.0f, 1.0f, 0.01f, "%.2f"});
        }
    }
}

void describe(TransparencySettings& s, PropertyVisitor& v) {
    v.enumeration("mode", "Transparency", s.mode, kTransparencyNames, "Weighted OIT needs no sorting; Sorted is exact for non-overlapping objects.");
    v.boolean("refraction", "Refraction", s.refraction, "Transmissive materials bend and blur what's behind them.");
}

void describe(PerformanceSettings& s, PropertyVisitor& v) {
    v.boolean("gpu_culling", "GPU-driven rendering", s.gpu_culling, "Compute culling + multi-draw indirect. Off = CPU culling, one draw per mesh part.");
    v.boolean("frustum_culling", "Frustum culling", s.frustum_culling);
    if (v.show(s.gpu_culling)) {
        v.boolean("occlusion_culling", "Occlusion culling", s.occlusion_culling, "Two-phase Hi-Z occlusion culling.");
    }
    v.boolean("lods", "LODs", s.lods);
    if (v.show(s.lods)) {
        v.number("lod_bias", "LOD bias", s.lod_bias, {0.25f, 4.0f, 0.01f, "%.2f"});
    }
    v.integer("max_lights", "Max lights", s.max_lights, 16, 8192);
    v.boolean("async_textures", "Async texture loading", s.async_textures);
    v.boolean("texture_compression", "Texture compression", s.texture_compression, "BC7 textures, cached on disk after the first load.");
}

void describe(RenderPipelineSettings& s, PropertyVisitor& v) {
    if (PropertyGroup g{v, "exposure", "Exposure"}) {
        describe(s.exposure, v);
    }
    if (PropertyGroup g{v, "sky", "Sky & environment"}) {
        describe(s.sky, v);
    }
    if (PropertyGroup g{v, "shadows", "Shadows"}) {
        describe(s.shadows, v);
    }
    if (PropertyGroup g{v, "gi", "Global illumination"}) {
        describe(s.gi, v);
    }
    if (PropertyGroup g{v, "fog", "Fog"}) {
        describe(s.fog, v);
    }
    if (PropertyGroup g{v, "anti_aliasing", "Anti-aliasing"}) {
        describe(s.aa, v);
    }
    if (PropertyGroup g{v, "post", "Post processing"}) {
        describe(s.post, v);
    }
    if (PropertyGroup g{v, "transparency", "Transparency"}) {
        describe(s.transparency, v);
    }
    if (PropertyGroup g{v, "performance", "Performance"}) {
        describe(s.performance, v);
    }
}

} // namespace Hollow

// OpenGL 4.5 renderer.
//
// A GPU-driven, clustered forward+ pipeline built on a frame render graph:
//
//   scene upload -> GPU culling (frustum + two-phase Hi-Z occlusion, LOD
//   selection) -> shadow cascades + shadow atlas -> depth/normal/velocity
//   prepass -> light clustering -> GTAO, contact shadows, SSR ->
//   volumetric fog -> forward shading (PBR with clearcoat/sheen/
//   transmission/anisotropy, IBL from sky/HDRI, reflection probes and
//   irradiance volumes) -> sky -> transparency (weighted OIT + refraction)
//   + particles + water -> TAA/upscaling -> depth of field, motion blur,
//   bloom, auto exposure -> tonemap + 3D LUT grading -> editor overlays.
//
// The final image is a texture (output_texture()) the editor shows in its
// viewport; present() blits it to the window instead.
//
// Implementation: src/Render/Renderer*.cpp (one file per stage), shaders
// in engine/shaders (hot reloaded), render graph in RenderGraph.cpp.
#pragma once

#include "Hollow/Render/Camera.hpp"
#include "Hollow/Render/RenderSettings.hpp"
#include "Hollow/Scene/Scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Hollow {

struct RenderPassStat {
    std::string name;
    double gpu_ms = 0.0;
    bool culled = false;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

struct RenderResourceStat {
    std::string name;
    int width = 0;
    int height = 0;
    int depth = 1;
    std::string format;
    bool imported = false;
    bool buffer = false;
    int pool_slot = -1; // transient textures sharing a slot alias memory
};

struct RenderStats {
    int draw_calls = 0;        // CPU draw calls (multi-draws count once)
    int instances = 0;         // mesh parts submitted
    int visible_instances = 0; // after frustum + occlusion culling (main view)
    int occluded_instances = 0;
    int shadow_views = 0;
    int lights = 0;
    int shadowed_lights = 0;
    int batches = 0;
    std::size_t triangles = 0; // main view, at the chosen LODs
    std::size_t gpu_meshes = 0;
    std::size_t mesh_pool_bytes = 0;
    std::size_t textures = 0;
    std::size_t texture_bytes = 0;
    std::size_t pending_textures = 0;
    std::size_t transient_textures = 0;
    std::size_t transient_bytes = 0;
    int particles = 0;
    double gpu_ms = 0.0;
    float exposure_ev100 = 0.0f;
    int render_width = 0;
    int render_height = 0;
    bool gpu_driven = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Needs a current OpenGL 4.5 context. Returns false (see error()) if a
    // required feature is missing or a shader fails to compile; the error
    // is also logged.
    bool initialize();
    bool ready() const;
    const std::string& error() const;
    void shutdown();

    // Output (viewport) size in pixels.
    void resize(int width, int height);
    int width() const;
    int height() const;

    void render(const Scene& scene, const Camera& camera, const RenderPipelineSettings& settings, const RenderView& view, float delta_seconds);
    // The finished frame (RGBA8, display-encoded), for the editor viewport.
    unsigned int output_texture() const;
    // Blits the output to the default framebuffer at the given rectangle.
    void present(int x, int y, int width, int height) const;

    // World point -> output pixel coordinates (y down).
    Vec2 project(Vec3 world, const Camera& camera) const;

    const RenderStats& stats() const;
    std::vector<RenderPassStat> pass_stats() const;
    std::vector<RenderResourceStat> resource_stats() const;

    // --- Environment (HDRI) ----------------------------------------------
    // Loads a Radiance .hdr and bakes it (cached by path, so reloading is a
    // handle swap). Used when the sky mode is HDRI. Throws
    // std::runtime_error on failure, leaving the previous environment.
    void load_environment(const std::string& hdr_path);
    void preload_environment(const std::string& hdr_path);
    bool activate_environment(const std::string& hdr_path);
    bool is_environment_cached(const std::string& hdr_path) const;
    void clear_environment();
    bool has_environment() const;
    const std::string& environment_path() const;
    std::size_t cached_environment_count() const;

    // --- Baked lighting -------------------------------------------------
    // Re-captures every reflection probe and irradiance volume (spread
    // over the next frames). Probes also bake automatically when added.
    void request_bake();
    // 0..1 while baking, 1 when idle.
    float bake_progress() const;
    bool baking() const;

    // Forces every shader to recompile (they also hot reload on save).
    void reload_shaders();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace Hollow

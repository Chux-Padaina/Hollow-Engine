// Reflection probe and irradiance volume baking.
//
// Bakes run after the frame, a few captures per frame so the editor stays
// responsive. A capture renders the opaque scene (forward, PASS_CAPTURE:
// every light, no screen-space effects) plus the global environment into a
// cubemap from the probe position:
//   - reflection probes: 6 faces at 128 px, then GGX-prefiltered into the
//     probe's layer of the probe cubemap array;
//   - irradiance volumes: 6 faces at 32 px per grid probe, projected to L1
//     SH on the GPU and written into the volume atlas. Bounce N > 0 is
//     captured with the volume itself lit by bounce N - 1.
#include "Render/RendererImpl.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace Hollow {

namespace {

constexpr int kProbeCaptureSize = 128;
constexpr int kVolumeCaptureSize = 32;
constexpr int kVolumeProbesPerFrame = 6;
constexpr float kCaptureNear = 0.05f;

Vec4 v4(Vec3 v, float w) {
    return {v.x, v.y, v.z, w};
}

// OpenGL cubemap face orientation (+X, -X, +Y, -Y, +Z, -Z).
constexpr Vec3 kFaceForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
constexpr Vec3 kFaceUp[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

} // namespace

void Renderer::Impl::ensure_volume_atlas() {
    // Layout: every volume gets a z-slab; the atlas is as wide/tall as the
    // largest grid.
    int width = 1;
    int height = 1;
    int depth = 0;
    bool layout_changed = false;
    for (VolumeRecord& r : volume_records) {
        const Actor* actor = frame.scene->find(r.actor);
        const auto* volume = actor != nullptr ? actor->find_component<IrradianceVolumeComponent>() : nullptr;
        if (volume == nullptr) {
            continue;
        }
        const int nx = std::clamp(volume->probes_x, 1, 64);
        const int ny = std::clamp(volume->probes_y, 1, 64);
        const int nz = std::clamp(volume->probes_z, 1, 64);
        if (r.nx != nx || r.ny != ny || r.nz != nz || r.z_offset != depth) {
            layout_changed = true;
        }
        r.nx = nx;
        r.ny = ny;
        r.nz = nz;
        r.z_offset = depth;
        width = std::max(width, nx);
        height = std::max(height, ny);
        depth += nz;
    }
    depth = std::max(depth, 1);
    const gfx::TextureDesc& current = volume_sh[0].desc();
    if (!layout_changed && current.width == width && current.height == height && current.depth == depth) {
        return;
    }
    for (gfx::Texture& t : volume_sh) {
        t = gfx::Texture(gfx::tex3d(GL_RGBA16F, width, height, depth), "IrradianceVolumeSH");
        const float zero[4] = {0, 0, 0, 0};
        glClearTexImage(t.id(), 0, GL_RGBA, GL_FLOAT, zero);
    }
    volume_atlas_depth = depth;
    // Everything moved: re-bake every volume (extraction re-queues them).
    for (VolumeRecord& r : volume_records) {
        r.baked = false;
    }
    for (BakeJob& job : bake_queue) {
        if (job.kind == BakeJob::Kind::Volume) {
            job.next_item = 0;
        }
    }
}

void Renderer::Impl::render_capture_face(GLuint cubemap, GLuint depth, int size, int face, Vec3 position, bool use_volumes) {
    const FrameState& f = frame;
    const Mat4 view = Mat4::look_at(position, position + kFaceForward[face], kFaceUp[face]);
    const Mat4 projection = Mat4::perspective_reversed_z(3.14159265f * 0.5f, 1.0f, kCaptureNear);
    const Mat4 view_projection = projection * view;
    gpu::FrameUniforms u = frame_uniforms;
    u.view = view;
    u.projection = projection;
    u.view_projection = view_projection;
    u.inv_view = inverse(view);
    u.inv_projection = inverse(projection);
    u.inv_view_projection = inverse(view_projection);
    u.unjittered_view_projection = view_projection;
    u.prev_view_projection = view_projection;
    u.prev_inv_view_projection = u.inv_view_projection;
    u.camera_position = v4(position, kCaptureNear);
    u.camera_forward = v4(kFaceForward[face], static_cast<float>(time));
    u.prev_camera_position = u.camera_position;
    const auto fs = static_cast<float>(size);
    u.render_size = {fs, fs, 1.0f / fs, 1.0f / fs};
    u.output_size = u.render_size;
    u.jitter = {};
    u.flags[0] = 0; // no debug view
    using namespace gpu::feature;
    int bits = f.feature_bits & ~(kSsr | kAo | kContactShadows | kFog | kVolumetricFog | kEditor | kDecals | kProbes);
    if (!use_volumes) {
        bits &= ~kVolumes;
    }
    u.flags[1] = bits;
    u.counts.x = 0.0f; // probes
    if (!use_volumes) {
        u.counts.y = 0.0f;
    }
    capture_ubo.upload(&u, sizeof(u));
    glBindBufferBase(GL_UNIFORM_BUFFER, gpu::binding::kFrame, capture_ubo.id());

    glNamedFramebufferTextureLayer(capture_fbo, GL_COLOR_ATTACHMENT0, cubemap, 0, face);
    glNamedFramebufferTexture(capture_fbo, GL_DEPTH_ATTACHMENT, depth, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
    glViewport(0, 0, size, size);
    const float clear_color[4] = {0, 0, 0, 1};
    const float clear_depth = 0.0f;
    glClearNamedFramebufferfv(capture_fbo, GL_COLOR, 0, clear_color);
    glClearNamedFramebufferfv(capture_fbo, GL_DEPTH, 0, &clear_depth);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);

    bind_scene_buffers();
    glBindBufferBase(GL_UNIFORM_BUFFER, gpu::binding::kFrame, capture_ubo.id());
    bind_global_textures(0, 0, 0, 0, 0, 0);
    const ProgramId opaque = mesh_program("PASS_CAPTURE", false);
    const ProgramId masked = mesh_program("PASS_CAPTURE", true);
    draw_batches(0, {BatchCategory::Opaque, BatchCategory::Masked},
        [opaque, masked](const Batch& b) { return b.category == BatchCategory::Masked ? masked : opaque; }, true, capture_commands.id());

    // Environment behind the geometry.
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    shaders.use(shaders.graphics("fullscreen.vert", "capture_sky.frag"));
    gfx::draw_fullscreen_triangle();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GREATER);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, gpu::binding::kFrame, frame_ubo.id());
}

void Renderer::Impl::process_bakes() {
    if (bake_queue.empty()) {
        bake_total = 0;
        bake_done = 0;
        return;
    }
    if (!capture_ubo) {
        capture_ubo = gfx::Buffer(sizeof(gpu::FrameUniforms), GL_DYNAMIC_STORAGE_BIT, nullptr, "CaptureFrame");
        volume_capture = gfx::Texture(gfx::cubemap(GL_RGBA16F, kVolumeCaptureSize), "VolumeCapture");
        volume_capture_depth = gfx::Texture(gfx::tex2d(GL_DEPTH_COMPONENT32F, kVolumeCaptureSize, kVolumeCaptureSize), "VolumeCaptureDepth");
        glCreateFramebuffers(1, &capture_fbo);
    }
    ensure_volume_atlas();
    if (instances.empty()) {
        return;
    }

    // Every instance, LOD 0 (LOD 1 when available for the tiny volume captures).
    std::vector<gpu::DrawCommand> commands(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const gpu::MeshInfo& info = pool.info_shadow[instances[i].mesh];
        gpu::DrawCommand& c = commands[i];
        c.count = info.lod_count[0];
        c.instance_count = 1;
        c.first_index = info.lod_first[0];
        c.base_vertex = instances[i].base_vertex;
        c.base_instance = static_cast<std::uint32_t>(i);
    }
    capture_commands.reserve(commands.size() * sizeof(gpu::DrawCommand), GL_DYNAMIC_STORAGE_BIT, "CaptureCommands");
    capture_commands.upload(commands.data(), commands.size() * sizeof(gpu::DrawCommand));
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    const float inv_exposure = 1.0f / std::max(frame.exposure, 1e-12f);
    const Scene& scene = *frame.scene;
    while (!bake_queue.empty()) {
        BakeJob& job = bake_queue.front();
        const Actor* actor = scene.find(job.actor);
        if (job.kind == BakeJob::Kind::Probe) {
            auto record = std::find_if(probe_records.begin(), probe_records.end(), [&](const ProbeRecord& r) { return r.actor == job.actor; });
            if (actor == nullptr || record == probe_records.end()) {
                bake_queue.erase(bake_queue.begin());
                continue;
            }
            const Vec3 position = actor->world_position();
            for (int face = 0; face < 6; ++face) {
                render_capture_face(capture_color.id(), capture_depth.id(), kProbeCaptureSize, face, position, !volumes.empty());
            }
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
            glGenerateTextureMipmap(capture_color.id());
            filter_cubemap_into(capture_color.id(), kProbeCaptureSize, record->layer, 0.0f, inv_exposure);
            record->baked = true;
            record->baked_position = position;
            bake_done += 6;
            bake_queue.erase(bake_queue.begin());
            break; // one probe per frame
        }

        auto record = std::find_if(volume_records.begin(), volume_records.end(), [&](const VolumeRecord& r) { return r.actor == job.actor; });
        const auto* volume = actor != nullptr ? actor->find_component<IrradianceVolumeComponent>() : nullptr;
        if (volume == nullptr || record == volume_records.end()) {
            bake_queue.erase(bake_queue.begin());
            continue;
        }
        const int count = record->nx * record->ny * record->nz;
        const int bounces = std::clamp(volume->bounces, 1, 4);
        const Vec3 center = actor->world_position();
        const Vec3 box_min = center - volume->extents;
        const Vec3 size = volume->extents * 2.0f;
        const ProgramId project = shaders.compute("sh_project.comp");
        for (int step = 0; step < kVolumeProbesPerFrame && job.next_item < count * bounces; ++step) {
            const int index = job.next_item % count;
            const int bounce = job.next_item / count;
            const int ix = index % record->nx;
            const int iy = (index / record->nx) % record->ny;
            const int iz = index / (record->nx * record->ny);
            auto axis = [](int i, int n) { return n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.5f; };
            const Vec3 position{box_min.x + size.x * axis(ix, record->nx), box_min.y + size.y * axis(iy, record->ny), box_min.z + size.z * axis(iz, record->nz)};
            for (int face = 0; face < 6; ++face) {
                render_capture_face(volume_capture.id(), volume_capture_depth.id(), kVolumeCaptureSize, face, position, bounce > 0);
            }
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
            shaders.use(project);
            gfx::bind_texture(0, volume_capture.id(), gfx::SamplerKind::LinearClamp);
            glUniform1i(shaders.uniform(project, "u_size"), kVolumeCaptureSize);
            glUniform1f(shaders.uniform(project, "u_scale"), inv_exposure);
            glUniform3i(shaders.uniform(project, "u_texel"), ix, iy, iz + record->z_offset);
            for (int i = 0; i < 3; ++i) {
                glBindImageTexture(static_cast<GLuint>(i), volume_sh[static_cast<std::size_t>(i)].id(), 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            }
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            ++job.next_item;
            ++bake_done;
            if (job.next_item == count) {
                // First bounce done: the volume starts lighting the scene
                // (and the next bounce's captures).
                record->baked = true;
            }
        }
        if (job.next_item >= count * bounces) {
            record->baked = true;
            record->baked_position = center;
            bake_queue.erase(bake_queue.begin());
        }
        break;
    }
    if (bake_queue.empty()) {
        log_info("Lighting bake finished");
    }
    // Restore the frame's bindings.
    glBindBufferBase(GL_UNIFORM_BUFFER, gpu::binding::kFrame, frame_ubo.id());
    glViewport(0, 0, frame.output_width, frame.output_height);
}

} // namespace Hollow

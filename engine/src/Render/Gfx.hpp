// Thin, RAII OpenGL 4.5 helpers (direct state access throughout): textures,
// buffers, samplers, a framebuffer cache, GPU timer queries and the debug
// message hook. Private to the renderer. Everything here needs the GL
// context created by Win32Window.
#pragma once

#include <glad/gl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Hollow::gfx {

int mip_count(int width, int height, int depth = 1);

// --- Textures ----------------------------------------------------------------

struct TextureDesc {
    GLenum target = GL_TEXTURE_2D;
    GLenum format = GL_RGBA8; // sized internal format
    int width = 1;
    int height = 1;
    int depth = 1; // layers for arrays (cube arrays: cubes), depth for 3D
    int mips = 1;

    bool operator==(const TextureDesc&) const = default;
};

class Texture {
public:
    Texture() = default;
    explicit Texture(const TextureDesc& desc, const char* label = nullptr);
    ~Texture();
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    GLuint id() const { return id_; }
    const TextureDesc& desc() const { return desc_; }
    int width() const { return desc_.width; }
    int height() const { return desc_.height; }
    explicit operator bool() const { return id_ != 0; }
    void reset();
    // Recreates the texture only if the description changed.
    bool ensure(const TextureDesc& desc, const char* label = nullptr);

private:
    GLuint id_ = 0;
    TextureDesc desc_;
};

TextureDesc tex2d(GLenum format, int width, int height, int mips = 1);
TextureDesc tex2d_array(GLenum format, int width, int height, int layers, int mips = 1);
TextureDesc tex3d(GLenum format, int width, int height, int depth);
TextureDesc cubemap(GLenum format, int size, int mips = 1);
TextureDesc cubemap_array(GLenum format, int size, int cubes, int mips = 1);
bool is_depth_format(GLenum format);
// Bytes per texel (0 for compressed formats).
std::size_t bytes_per_texel(GLenum format);

// --- Buffers ------------------------------------------------------------------

class Buffer {
public:
    Buffer() = default;
    explicit Buffer(std::size_t size, GLbitfield flags = GL_DYNAMIC_STORAGE_BIT, const void* data = nullptr, const char* label = nullptr);
    ~Buffer();
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    GLuint id() const { return id_; }
    std::size_t size() const { return size_; }
    explicit operator bool() const { return id_ != 0; }
    void reset();
    // Grows (never shrinks) to at least `size`, discarding contents.
    void reserve(std::size_t size, GLbitfield flags = GL_DYNAMIC_STORAGE_BIT, const char* label = nullptr);
    // Grows preserving contents (GPU copy).
    void grow_preserve(std::size_t size, const char* label = nullptr);
    void upload(const void* data, std::size_t size, std::size_t offset = 0);
    template <typename T>
    void upload(std::span<const T> items, std::size_t offset = 0) {
        upload(items.data(), items.size_bytes(), offset);
    }
    template <typename T>
    void upload_vector(const std::vector<T>& items) {
        if (!items.empty()) {
            reserve(items.size() * sizeof(T), flags_);
            upload(items.data(), items.size() * sizeof(T));
        }
    }
    void clear_u32(std::uint32_t value = 0);

private:
    GLuint id_ = 0;
    std::size_t size_ = 0;
    GLbitfield flags_ = GL_DYNAMIC_STORAGE_BIT;
};

// --- Samplers -------------------------------------------------------------------

enum class SamplerKind {
    LinearClamp,
    LinearRepeat,
    NearestClamp,
    LinearMipClamp,
    LinearMipRepeatAniso, // material textures
    ShadowCompare,        // depth compare (reversed-Z: GEQUAL)
    LinearMipClampBorderBlack,
    NearestMipClamp, // texelFetch across mips (Hi-Z)
    Count,
};

void create_samplers(float max_anisotropy);
void destroy_samplers();
GLuint sampler(SamplerKind kind);

// Binds texture + sampler to a unit in one call.
void bind_texture(GLuint unit, GLuint texture, SamplerKind kind = SamplerKind::LinearClamp);
void bind_texture(GLuint unit, const Texture& texture, SamplerKind kind = SamplerKind::LinearClamp);
void bind_image(GLuint unit, const Texture& texture, GLenum access, int level = 0, int layer = -1);

// --- Framebuffers -----------------------------------------------------------------

struct Attachment {
    GLuint texture = 0;
    int level = 0;
    int layer = -1; // -1: whole texture (layered for arrays/cubes)
    bool operator==(const Attachment&) const = default;
};

// Caches FBOs by their attachment set. Textures are recreated through the
// render graph's pool, so entries referencing a deleted texture are purged
// with forget_texture().
class FramebufferCache {
public:
    ~FramebufferCache();
    GLuint get(std::span<const Attachment> colors, Attachment depth);
    void forget_texture(GLuint texture);
    void clear();
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::vector<Attachment> colors;
        Attachment depth;
        GLuint fbo = 0;
    };
    std::vector<Entry> entries_;
};

// --- GPU timing ---------------------------------------------------------------------

// Timestamp queries per named scope, read back a few frames late so the CPU
// never waits on the GPU.
class GpuProfiler {
public:
    static constexpr int kLatency = 4;
    struct Result {
        std::string name;
        double milliseconds = 0.0;
        int depth = 0;
    };

    ~GpuProfiler();
    void begin_frame();
    void begin(const char* name);
    void end();
    // Results of the newest frame whose queries are complete.
    const std::vector<Result>& results() const { return results_; }
    double frame_milliseconds() const { return frame_ms_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

private:
    struct Scope {
        std::string name;
        GLuint begin_query = 0;
        GLuint end_query = 0;
        int depth = 0;
    };
    struct Frame {
        std::vector<Scope> scopes;
        bool pending = false;
    };
    GLuint acquire_query();
    void collect(Frame& frame);

    std::array<Frame, kLatency> frames_{};
    int frame_ = 0;
    int depth_ = 0;
    std::vector<int> open_;
    std::vector<GLuint> free_queries_;
    std::vector<Result> results_;
    double frame_ms_ = 0.0;
    bool enabled_ = true;
};

// RAII debug group + timer scope.
class ScopedPass {
public:
    ScopedPass(GpuProfiler* profiler, const char* name);
    ~ScopedPass();
    ScopedPass(const ScopedPass&) = delete;
    ScopedPass& operator=(const ScopedPass&) = delete;

private:
    GpuProfiler* profiler_;
};

// --- Misc ---------------------------------------------------------------------------

// Routes KHR_debug messages (errors, performance warnings) to the engine log.
void install_debug_output();
std::string gl_string(GLenum name);
bool has_extension(const char* name);
void set_label(GLenum identifier, GLuint name, const char* label);

// Fullscreen triangle (no vertex buffer needed: gl_VertexID based).
void draw_fullscreen_triangle();
GLuint empty_vertex_array();
void destroy_shared_objects();

inline GLuint groups(int size, int local) {
    return static_cast<GLuint>((size + local - 1) / local);
}

} // namespace Hollow::gfx

// Implementation of Gfx.hpp.
#include "Render/Gfx.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace Hollow::gfx {

int mip_count(int width, int height, int depth) {
    int size = std::max({width, height, depth, 1});
    int count = 1;
    while (size > 1) {
        size >>= 1;
        ++count;
    }
    return count;
}

void set_label(GLenum identifier, GLuint name, const char* label) {
    if (label != nullptr && glObjectLabel != nullptr) {
        glObjectLabel(identifier, name, -1, label);
    }
}

// --- Texture -------------------------------------------------------------------

bool is_depth_format(GLenum format) {
    return format == GL_DEPTH_COMPONENT32F || format == GL_DEPTH_COMPONENT24 || format == GL_DEPTH_COMPONENT16 || format == GL_DEPTH32F_STENCIL8 || format == GL_DEPTH24_STENCIL8;
}

std::size_t bytes_per_texel(GLenum format) {
    switch (format) {
    case GL_R8:
    case GL_R8UI:
        return 1;
    case GL_RG8:
    case GL_R16F:
    case GL_R16UI:
    case GL_DEPTH_COMPONENT16:
        return 2;
    case GL_RGBA8:
    case GL_SRGB8_ALPHA8:
    case GL_RG16F:
    case GL_R32F:
    case GL_R32UI:
    case GL_RGB10_A2:
    case GL_R11F_G11F_B10F:
    case GL_DEPTH_COMPONENT32F:
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH24_STENCIL8:
    case GL_RG16:
    case GL_RG16_SNORM:
        return 4;
    case GL_RGBA16F:
    case GL_RG32F:
    case GL_RG32UI:
    case GL_DEPTH32F_STENCIL8:
    case GL_RGBA16:
        return 8;
    case GL_RGBA32F:
    case GL_RGBA32UI:
        return 16;
    default:
        return 0;
    }
}

TextureDesc tex2d(GLenum format, int width, int height, int mips) {
    return {GL_TEXTURE_2D, format, std::max(1, width), std::max(1, height), 1, std::max(1, mips)};
}

TextureDesc tex2d_array(GLenum format, int width, int height, int layers, int mips) {
    return {GL_TEXTURE_2D_ARRAY, format, std::max(1, width), std::max(1, height), std::max(1, layers), std::max(1, mips)};
}

TextureDesc tex3d(GLenum format, int width, int height, int depth) {
    return {GL_TEXTURE_3D, format, std::max(1, width), std::max(1, height), std::max(1, depth), 1};
}

TextureDesc cubemap(GLenum format, int size, int mips) {
    return {GL_TEXTURE_CUBE_MAP, format, std::max(1, size), std::max(1, size), 1, std::max(1, mips)};
}

TextureDesc cubemap_array(GLenum format, int size, int cubes, int mips) {
    return {GL_TEXTURE_CUBE_MAP_ARRAY, format, std::max(1, size), std::max(1, size), std::max(1, cubes), std::max(1, mips)};
}

Texture::Texture(const TextureDesc& desc, const char* label) : desc_(desc) {
    glCreateTextures(desc.target, 1, &id_);
    switch (desc.target) {
    case GL_TEXTURE_2D:
    case GL_TEXTURE_CUBE_MAP:
        glTextureStorage2D(id_, desc.mips, desc.format, desc.width, desc.height);
        break;
    case GL_TEXTURE_2D_ARRAY:
    case GL_TEXTURE_3D:
        glTextureStorage3D(id_, desc.mips, desc.format, desc.width, desc.height, desc.depth);
        break;
    case GL_TEXTURE_CUBE_MAP_ARRAY:
        glTextureStorage3D(id_, desc.mips, desc.format, desc.width, desc.height, desc.depth * 6);
        break;
    default:
        break;
    }
    // Sensible defaults; samplers override these when bound.
    const bool mipmapped = desc.mips > 1;
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_MAX_LEVEL, desc.mips - 1);
    set_label(GL_TEXTURE, id_, label);
}

Texture::~Texture() {
    reset();
}

Texture::Texture(Texture&& other) noexcept : id_(std::exchange(other.id_, 0)), desc_(other.desc_) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = std::exchange(other.id_, 0);
        desc_ = other.desc_;
    }
    return *this;
}

void Texture::reset() {
    if (id_ != 0) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
}

bool Texture::ensure(const TextureDesc& desc, const char* label) {
    if (id_ != 0 && desc_ == desc) {
        return false;
    }
    *this = Texture(desc, label);
    return true;
}

// --- Buffer --------------------------------------------------------------------

Buffer::Buffer(std::size_t size, GLbitfield flags, const void* data, const char* label) : size_(std::max<std::size_t>(size, 16)), flags_(flags) {
    glCreateBuffers(1, &id_);
    glNamedBufferStorage(id_, static_cast<GLsizeiptr>(size_), data, flags);
    set_label(GL_BUFFER, id_, label);
}

Buffer::~Buffer() {
    reset();
}

Buffer::Buffer(Buffer&& other) noexcept : id_(std::exchange(other.id_, 0)), size_(std::exchange(other.size_, 0)), flags_(other.flags_) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = std::exchange(other.id_, 0);
        size_ = std::exchange(other.size_, 0);
        flags_ = other.flags_;
    }
    return *this;
}

void Buffer::reset() {
    if (id_ != 0) {
        glDeleteBuffers(1, &id_);
        id_ = 0;
        size_ = 0;
    }
}

void Buffer::reserve(std::size_t size, GLbitfield flags, const char* label) {
    if (id_ != 0 && size <= size_) {
        return;
    }
    std::size_t capacity = std::max<std::size_t>(size_ == 0 ? 256 : size_, 256);
    while (capacity < size) {
        capacity *= 2;
    }
    *this = Buffer(capacity, flags, nullptr, label);
}

void Buffer::grow_preserve(std::size_t size, const char* label) {
    if (id_ != 0 && size <= size_) {
        return;
    }
    std::size_t capacity = std::max<std::size_t>(size_ == 0 ? 256 : size_, 256);
    while (capacity < size) {
        capacity *= 2;
    }
    Buffer bigger(capacity, flags_, nullptr, label);
    if (id_ != 0 && size_ > 0) {
        glCopyNamedBufferSubData(id_, bigger.id(), 0, 0, static_cast<GLsizeiptr>(size_));
    }
    *this = std::move(bigger);
}

void Buffer::upload(const void* data, std::size_t size, std::size_t offset) {
    if (size == 0 || id_ == 0) {
        return;
    }
    glNamedBufferSubData(id_, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(std::min(size, size_ - std::min(offset, size_))), data);
}

void Buffer::clear_u32(std::uint32_t value) {
    if (id_ != 0) {
        glClearNamedBufferData(id_, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    }
}

// --- Samplers -------------------------------------------------------------------

namespace {
std::array<GLuint, static_cast<std::size_t>(SamplerKind::Count)> g_samplers{};
GLuint g_empty_vao = 0;
} // namespace

void create_samplers(float max_anisotropy) {
    destroy_samplers();
    glCreateSamplers(static_cast<GLsizei>(g_samplers.size()), g_samplers.data());
    auto setup = [](SamplerKind kind, GLenum min_filter, GLenum mag_filter, GLenum wrap) {
        const GLuint s = g_samplers[static_cast<std::size_t>(kind)];
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min_filter));
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(mag_filter));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrap));
        return s;
    };
    setup(SamplerKind::LinearClamp, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
    setup(SamplerKind::LinearRepeat, GL_LINEAR, GL_LINEAR, GL_REPEAT);
    setup(SamplerKind::NearestClamp, GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE);
    setup(SamplerKind::LinearMipClamp, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
    const GLuint material = setup(SamplerKind::LinearMipRepeatAniso, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT);
    if (max_anisotropy > 1.0f) {
        glSamplerParameterf(material, GL_TEXTURE_MAX_ANISOTROPY, max_anisotropy);
    }
    const GLuint shadow = setup(SamplerKind::ShadowCompare, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(shadow, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(shadow, GL_TEXTURE_COMPARE_FUNC, GL_GEQUAL); // reversed-Z
    const GLuint border = setup(SamplerKind::LinearMipClampBorderBlack, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_BORDER);
    const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glSamplerParameterfv(border, GL_TEXTURE_BORDER_COLOR, black);
    setup(SamplerKind::NearestMipClamp, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE);
}

void destroy_samplers() {
    if (g_samplers[0] != 0) {
        glDeleteSamplers(static_cast<GLsizei>(g_samplers.size()), g_samplers.data());
        g_samplers.fill(0);
    }
}

GLuint sampler(SamplerKind kind) {
    return g_samplers[static_cast<std::size_t>(kind)];
}

void bind_texture(GLuint unit, GLuint texture, SamplerKind kind) {
    glBindTextureUnit(unit, texture);
    glBindSampler(unit, sampler(kind));
}

void bind_texture(GLuint unit, const Texture& texture, SamplerKind kind) {
    bind_texture(unit, texture.id(), kind);
}

void bind_image(GLuint unit, const Texture& texture, GLenum access, int level, int layer) {
    const bool layered = layer < 0 && (texture.desc().target != GL_TEXTURE_2D);
    glBindImageTexture(unit, texture.id(), level, layered ? GL_TRUE : GL_FALSE, std::max(layer, 0), access, texture.desc().format);
}

// --- Framebuffer cache ---------------------------------------------------------

FramebufferCache::~FramebufferCache() {
    clear();
}

GLuint FramebufferCache::get(std::span<const Attachment> colors, Attachment depth) {
    for (const Entry& entry : entries_) {
        if (entry.depth == depth && entry.colors.size() == colors.size() && std::equal(colors.begin(), colors.end(), entry.colors.begin())) {
            return entry.fbo;
        }
    }
    Entry entry;
    entry.colors.assign(colors.begin(), colors.end());
    entry.depth = depth;
    glCreateFramebuffers(1, &entry.fbo);
    std::array<GLenum, 8> draw_buffers{};
    for (std::size_t i = 0; i < colors.size() && i < draw_buffers.size(); ++i) {
        const Attachment& a = colors[i];
        const GLenum point = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        if (a.layer >= 0) {
            glNamedFramebufferTextureLayer(entry.fbo, point, a.texture, a.level, a.layer);
        } else {
            glNamedFramebufferTexture(entry.fbo, point, a.texture, a.level);
        }
        draw_buffers[i] = point;
    }
    if (depth.texture != 0) {
        if (depth.layer >= 0) {
            glNamedFramebufferTextureLayer(entry.fbo, GL_DEPTH_ATTACHMENT, depth.texture, depth.level, depth.layer);
        } else {
            glNamedFramebufferTexture(entry.fbo, GL_DEPTH_ATTACHMENT, depth.texture, depth.level);
        }
    }
    if (colors.empty()) {
        glNamedFramebufferDrawBuffer(entry.fbo, GL_NONE);
        glNamedFramebufferReadBuffer(entry.fbo, GL_NONE);
    } else {
        glNamedFramebufferDrawBuffers(entry.fbo, static_cast<GLsizei>(colors.size()), draw_buffers.data());
    }
    const GLenum status = glCheckNamedFramebufferStatus(entry.fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log_error(std::format("Framebuffer incomplete (0x{:X})", status));
    }
    entries_.push_back(std::move(entry));
    return entries_.back().fbo;
}

void FramebufferCache::forget_texture(GLuint texture) {
    for (std::size_t i = 0; i < entries_.size();) {
        Entry& entry = entries_[i];
        const bool uses = entry.depth.texture == texture ||
                          std::any_of(entry.colors.begin(), entry.colors.end(), [texture](const Attachment& a) { return a.texture == texture; });
        if (uses) {
            glDeleteFramebuffers(1, &entry.fbo);
            entries_[i] = std::move(entries_.back());
            entries_.pop_back();
        } else {
            ++i;
        }
    }
}

void FramebufferCache::clear() {
    for (Entry& entry : entries_) {
        glDeleteFramebuffers(1, &entry.fbo);
    }
    entries_.clear();
}

// --- GPU profiler ----------------------------------------------------------------

GpuProfiler::~GpuProfiler() {
    for (Frame& frame : frames_) {
        for (Scope& scope : frame.scopes) {
            free_queries_.push_back(scope.begin_query);
            free_queries_.push_back(scope.end_query);
        }
        frame.scopes.clear();
    }
    if (!free_queries_.empty() && glDeleteQueries != nullptr) {
        glDeleteQueries(static_cast<GLsizei>(free_queries_.size()), free_queries_.data());
    }
}

GLuint GpuProfiler::acquire_query() {
    if (free_queries_.empty()) {
        std::array<GLuint, 32> ids{};
        glGenQueries(static_cast<GLsizei>(ids.size()), ids.data());
        free_queries_.insert(free_queries_.end(), ids.begin(), ids.end());
    }
    const GLuint id = free_queries_.back();
    free_queries_.pop_back();
    return id;
}

void GpuProfiler::collect(Frame& frame) {
    if (!frame.pending) {
        return;
    }
    // Only read if the last query of the frame is available (never stall).
    if (!frame.scopes.empty()) {
        GLint available = 0;
        glGetQueryObjectiv(frame.scopes.front().end_query, GL_QUERY_RESULT_AVAILABLE, &available);
        GLint last_available = 0;
        glGetQueryObjectiv(frame.scopes.back().end_query, GL_QUERY_RESULT_AVAILABLE, &last_available);
        if (!available || !last_available) {
            return;
        }
    }
    results_.clear();
    GLuint64 first = 0;
    GLuint64 last = 0;
    for (std::size_t i = 0; i < frame.scopes.size(); ++i) {
        const Scope& scope = frame.scopes[i];
        GLuint64 begin = 0;
        GLuint64 end = 0;
        glGetQueryObjectui64v(scope.begin_query, GL_QUERY_RESULT, &begin);
        glGetQueryObjectui64v(scope.end_query, GL_QUERY_RESULT, &end);
        if (i == 0) {
            first = begin;
        }
        last = std::max(last, end);
        results_.push_back({scope.name, static_cast<double>(end - std::min(begin, end)) / 1.0e6, scope.depth});
        free_queries_.push_back(scope.begin_query);
        free_queries_.push_back(scope.end_query);
    }
    frame_ms_ = frame.scopes.empty() ? 0.0 : static_cast<double>(last - std::min(first, last)) / 1.0e6;
    frame.scopes.clear();
    frame.pending = false;
}

void GpuProfiler::begin_frame() {
    frame_ = (frame_ + 1) % kLatency;
    Frame& frame = frames_[static_cast<std::size_t>(frame_)];
    if (frame.pending) {
        // Still not available after kLatency frames: wait (rare).
        for (Scope& scope : frame.scopes) {
            free_queries_.push_back(scope.begin_query);
            free_queries_.push_back(scope.end_query);
        }
        frame.scopes.clear();
        frame.pending = false;
    }
    // Collect the oldest pending frame.
    for (int i = 1; i < kLatency; ++i) {
        collect(frames_[static_cast<std::size_t>((frame_ + i) % kLatency)]);
    }
    open_.clear();
    depth_ = 0;
}

void GpuProfiler::begin(const char* name) {
    if (!enabled_) {
        return;
    }
    Frame& frame = frames_[static_cast<std::size_t>(frame_)];
    Scope scope;
    scope.name = name;
    scope.depth = depth_++;
    scope.begin_query = acquire_query();
    scope.end_query = acquire_query();
    glQueryCounter(scope.begin_query, GL_TIMESTAMP);
    open_.push_back(static_cast<int>(frame.scopes.size()));
    frame.scopes.push_back(std::move(scope));
    frame.pending = true;
}

void GpuProfiler::end() {
    if (!enabled_ || open_.empty()) {
        return;
    }
    Frame& frame = frames_[static_cast<std::size_t>(frame_)];
    glQueryCounter(frame.scopes[static_cast<std::size_t>(open_.back())].end_query, GL_TIMESTAMP);
    open_.pop_back();
    --depth_;
}

ScopedPass::ScopedPass(GpuProfiler* profiler, const char* name) : profiler_(profiler) {
    if (glPushDebugGroup != nullptr) {
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
    }
    if (profiler_ != nullptr) {
        profiler_->begin(name);
    }
}

ScopedPass::~ScopedPass() {
    if (profiler_ != nullptr) {
        profiler_->end();
    }
    if (glPopDebugGroup != nullptr) {
        glPopDebugGroup();
    }
}

// --- Misc ------------------------------------------------------------------------

namespace {

void GLAD_API_PTR debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
    (void)source;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || type == GL_DEBUG_TYPE_PUSH_GROUP || type == GL_DEBUG_TYPE_POP_GROUP) {
        return;
    }
    // Known-noisy IDs (NVIDIA buffer-placement info, shader recompiles).
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204) {
        return;
    }
    static int reported = 0;
    if (reported > 200) {
        return; // don't flood the console
    }
    ++reported;
    const std::string text = std::format("GL: {}", message);
    if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) {
        log_error(text);
    } else {
        log_warning(text);
    }
}

} // namespace

void install_debug_output() {
    if (glDebugMessageCallback == nullptr) {
        return;
    }
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(debug_callback, nullptr);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
}

std::string gl_string(GLenum name) {
    const auto* text = reinterpret_cast<const char*>(glGetString(name));
    return text != nullptr ? text : "";
}

bool has_extension(const char* name) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const auto* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (ext != nullptr && std::strcmp(ext, name) == 0) {
            return true;
        }
    }
    return false;
}

GLuint empty_vertex_array() {
    if (g_empty_vao == 0) {
        glCreateVertexArrays(1, &g_empty_vao);
    }
    return g_empty_vao;
}

void draw_fullscreen_triangle() {
    glBindVertexArray(empty_vertex_array());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void destroy_shared_objects() {
    if (g_empty_vao != 0) {
        glDeleteVertexArrays(1, &g_empty_vao);
        g_empty_vao = 0;
    }
    destroy_samplers();
}

} // namespace Hollow::gfx

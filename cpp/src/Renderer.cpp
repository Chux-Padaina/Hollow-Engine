#include "Forge/Renderer.hpp"
#include "Forge/TextureLoader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include <Windows.h>
#include <gl/GL.h>

namespace Forge {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

constexpr unsigned int kArrayBuffer = 0x8892;
constexpr unsigned int kElementArrayBuffer = 0x8893;
constexpr unsigned int kStaticDraw = 0x88E4;
constexpr unsigned int kFloat = 0x1406;
constexpr unsigned int kFalse = 0;
constexpr unsigned int kTriangles = 0x0004;
constexpr unsigned int kUnsignedInt = 0x1405;
constexpr unsigned int kUnsignedByte = 0x1401;
constexpr unsigned int kVertexShader = 0x8B31;
constexpr unsigned int kFragmentShader = 0x8B30;
constexpr unsigned int kCompileStatus = 0x8B81;
constexpr unsigned int kLinkStatus = 0x8B82;
constexpr unsigned int kInfoLogLength = 0x8B84;
constexpr unsigned int kTexture2D = 0x0DE1;
constexpr unsigned int kRgba = 0x1908;
constexpr unsigned int kLinear = 0x2601;
constexpr unsigned int kRepeat = 0x2901;
constexpr unsigned int kTextureMinFilter = 0x2801;
constexpr unsigned int kTextureMagFilter = 0x2800;
constexpr unsigned int kTextureWrapS = 0x2802;
constexpr unsigned int kTextureWrapT = 0x2803;
constexpr unsigned int kUnpackAlignment = 0x0CF5;
constexpr unsigned int kTexture0 = 0x84C0;
constexpr unsigned int kTexture1 = 0x84C1;
constexpr unsigned int kTexture2 = 0x84C2;
constexpr unsigned int kTexture3 = 0x84C3;
constexpr unsigned int kTexture4 = 0x84C4;
constexpr unsigned int kTexture5 = 0x84C5;
constexpr unsigned int kTexture6 = 0x84C6;
constexpr unsigned int kTexture7 = 0x84C7;
constexpr unsigned int kTexture8 = 0x84C8;
constexpr unsigned int kTexture9 = 0x84C9;
constexpr unsigned int kDepthComponent = 0x1902;
constexpr unsigned int kRgba16f = 0x881A;
constexpr unsigned int kNearest = 0x2600;
constexpr unsigned int kClampToEdge = 0x812F;
constexpr unsigned int kFramebuffer = 0x8D40;
constexpr unsigned int kColorAttachment0 = 0x8CE0;
constexpr unsigned int kDepthAttachment = 0x8D00;
constexpr unsigned int kFramebufferComplete = 0x8CD5;
constexpr unsigned int kNone = 0;
constexpr unsigned int kBack = 0x0405;

using GlCreateShader = unsigned int(APIENTRY*)(unsigned int shader_type);
using GlShaderSource = void(APIENTRY*)(unsigned int shader, int count, const char* const* string, const int* length);
using GlCompileShader = void(APIENTRY*)(unsigned int shader);
using GlGetShaderiv = void(APIENTRY*)(unsigned int shader, unsigned int pname, int* params);
using GlGetShaderInfoLog = void(APIENTRY*)(unsigned int shader, int max_length, int* length, char* info_log);
using GlDeleteShader = void(APIENTRY*)(unsigned int shader);
using GlCreateProgram = unsigned int(APIENTRY*)();
using GlAttachShader = void(APIENTRY*)(unsigned int program, unsigned int shader);
using GlBindAttribLocation = void(APIENTRY*)(unsigned int program, unsigned int index, const char* name);
using GlLinkProgram = void(APIENTRY*)(unsigned int program);
using GlGetProgramiv = void(APIENTRY*)(unsigned int program, unsigned int pname, int* params);
using GlGetProgramInfoLog = void(APIENTRY*)(unsigned int program, int max_length, int* length, char* info_log);
using GlDeleteProgram = void(APIENTRY*)(unsigned int program);
using GlUseProgram = void(APIENTRY*)(unsigned int program);
using GlGetUniformLocation = int(APIENTRY*)(unsigned int program, const char* name);
using GlUniformMatrix4fv = void(APIENTRY*)(int location, int count, unsigned char transpose, const float* value);
using GlUniform3f = void(APIENTRY*)(int location, float v0, float v1, float v2);
using GlUniform1f = void(APIENTRY*)(int location, float v0);
using GlUniform1i = void(APIENTRY*)(int location, int v0);
using GlActiveTexture = void(APIENTRY*)(unsigned int texture);
using GlGenBuffers = void(APIENTRY*)(int count, unsigned int* buffers);
using GlBindBuffer = void(APIENTRY*)(unsigned int target, unsigned int buffer);
using GlBufferData = void(APIENTRY*)(unsigned int target, std::ptrdiff_t size, const void* data, unsigned int usage);
using GlDeleteBuffers = void(APIENTRY*)(int count, const unsigned int* buffers);
using GlEnableVertexAttribArray = void(APIENTRY*)(unsigned int index);
using GlDisableVertexAttribArray = void(APIENTRY*)(unsigned int index);
using GlVertexAttribPointer = void(APIENTRY*)(unsigned int index, int size, unsigned int type, unsigned char normalized, int stride, const void* pointer);
using GlGenVertexArrays = void(APIENTRY*)(int count, unsigned int* arrays);
using GlBindVertexArray = void(APIENTRY*)(unsigned int array);
using GlDeleteVertexArrays = void(APIENTRY*)(int count, const unsigned int* arrays);
using GlGenFramebuffers = void(APIENTRY*)(int count, unsigned int* framebuffers);
using GlBindFramebuffer = void(APIENTRY*)(unsigned int target, unsigned int framebuffer);
using GlFramebufferTexture2D = void(APIENTRY*)(unsigned int target, unsigned int attachment, unsigned int textarget, unsigned int texture, int level);
using GlCheckFramebufferStatus = unsigned int(APIENTRY*)(unsigned int target);
using GlDeleteFramebuffers = void(APIENTRY*)(int count, const unsigned int* framebuffers);

struct ModernGl {
    GlCreateShader create_shader = nullptr;
    GlShaderSource shader_source = nullptr;
    GlCompileShader compile_shader = nullptr;
    GlGetShaderiv get_shader_iv = nullptr;
    GlGetShaderInfoLog get_shader_info_log = nullptr;
    GlDeleteShader delete_shader = nullptr;
    GlCreateProgram create_program = nullptr;
    GlAttachShader attach_shader = nullptr;
    GlBindAttribLocation bind_attrib_location = nullptr;
    GlLinkProgram link_program = nullptr;
    GlGetProgramiv get_program_iv = nullptr;
    GlGetProgramInfoLog get_program_info_log = nullptr;
    GlDeleteProgram delete_program = nullptr;
    GlUseProgram use_program = nullptr;
    GlGetUniformLocation get_uniform_location = nullptr;
    GlUniformMatrix4fv uniform_matrix_4fv = nullptr;
    GlUniform3f uniform_3f = nullptr;
    GlUniform1f uniform_1f = nullptr;
    GlUniform1i uniform_1i = nullptr;
    GlActiveTexture active_texture = nullptr;
    GlGenBuffers gen_buffers = nullptr;
    GlBindBuffer bind_buffer = nullptr;
    GlBufferData buffer_data = nullptr;
    GlDeleteBuffers delete_buffers = nullptr;
    GlEnableVertexAttribArray enable_vertex_attrib_array = nullptr;
    GlDisableVertexAttribArray disable_vertex_attrib_array = nullptr;
    GlVertexAttribPointer vertex_attrib_pointer = nullptr;
    GlGenVertexArrays gen_vertex_arrays = nullptr;
    GlBindVertexArray bind_vertex_array = nullptr;
    GlDeleteVertexArrays delete_vertex_arrays = nullptr;
    GlGenFramebuffers gen_framebuffers = nullptr;
    GlBindFramebuffer bind_framebuffer = nullptr;
    GlFramebufferTexture2D framebuffer_texture_2d = nullptr;
    GlCheckFramebufferStatus check_framebuffer_status = nullptr;
    GlDeleteFramebuffers delete_framebuffers = nullptr;

    bool ready() const {
        return create_shader != nullptr
            && shader_source != nullptr
            && compile_shader != nullptr
            && get_shader_iv != nullptr
            && get_shader_info_log != nullptr
            && delete_shader != nullptr
            && create_program != nullptr
            && attach_shader != nullptr
            && bind_attrib_location != nullptr
            && link_program != nullptr
            && get_program_iv != nullptr
            && get_program_info_log != nullptr
            && delete_program != nullptr
            && use_program != nullptr
            && get_uniform_location != nullptr
            && uniform_matrix_4fv != nullptr
            && uniform_3f != nullptr
            && uniform_1f != nullptr
            && uniform_1i != nullptr
            && active_texture != nullptr
            && gen_buffers != nullptr
            && bind_buffer != nullptr
            && buffer_data != nullptr
            && delete_buffers != nullptr
            && enable_vertex_attrib_array != nullptr
            && disable_vertex_attrib_array != nullptr
            && vertex_attrib_pointer != nullptr
            && gen_framebuffers != nullptr
            && bind_framebuffer != nullptr
            && framebuffer_texture_2d != nullptr
            && check_framebuffer_status != nullptr
            && delete_framebuffers != nullptr;
    }
};

ModernGl glx;

template <typename T>
T load_gl_function(const char* name) {
    void* proc = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (proc == nullptr || proc == reinterpret_cast<void*>(0x1) || proc == reinterpret_cast<void*>(0x2) || proc == reinterpret_cast<void*>(0x3) || proc == reinterpret_cast<void*>(-1)) {
        proc = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name));
    }
    return reinterpret_cast<T>(proc);
}

bool load_modern_gl() {
    glx.create_shader = load_gl_function<GlCreateShader>("glCreateShader");
    glx.shader_source = load_gl_function<GlShaderSource>("glShaderSource");
    glx.compile_shader = load_gl_function<GlCompileShader>("glCompileShader");
    glx.get_shader_iv = load_gl_function<GlGetShaderiv>("glGetShaderiv");
    glx.get_shader_info_log = load_gl_function<GlGetShaderInfoLog>("glGetShaderInfoLog");
    glx.delete_shader = load_gl_function<GlDeleteShader>("glDeleteShader");
    glx.create_program = load_gl_function<GlCreateProgram>("glCreateProgram");
    glx.attach_shader = load_gl_function<GlAttachShader>("glAttachShader");
    glx.bind_attrib_location = load_gl_function<GlBindAttribLocation>("glBindAttribLocation");
    glx.link_program = load_gl_function<GlLinkProgram>("glLinkProgram");
    glx.get_program_iv = load_gl_function<GlGetProgramiv>("glGetProgramiv");
    glx.get_program_info_log = load_gl_function<GlGetProgramInfoLog>("glGetProgramInfoLog");
    glx.delete_program = load_gl_function<GlDeleteProgram>("glDeleteProgram");
    glx.use_program = load_gl_function<GlUseProgram>("glUseProgram");
    glx.get_uniform_location = load_gl_function<GlGetUniformLocation>("glGetUniformLocation");
    glx.uniform_matrix_4fv = load_gl_function<GlUniformMatrix4fv>("glUniformMatrix4fv");
    glx.uniform_3f = load_gl_function<GlUniform3f>("glUniform3f");
    glx.uniform_1f = load_gl_function<GlUniform1f>("glUniform1f");
    glx.uniform_1i = load_gl_function<GlUniform1i>("glUniform1i");
    glx.active_texture = load_gl_function<GlActiveTexture>("glActiveTexture");
    glx.gen_buffers = load_gl_function<GlGenBuffers>("glGenBuffers");
    glx.bind_buffer = load_gl_function<GlBindBuffer>("glBindBuffer");
    glx.buffer_data = load_gl_function<GlBufferData>("glBufferData");
    glx.delete_buffers = load_gl_function<GlDeleteBuffers>("glDeleteBuffers");
    glx.enable_vertex_attrib_array = load_gl_function<GlEnableVertexAttribArray>("glEnableVertexAttribArray");
    glx.disable_vertex_attrib_array = load_gl_function<GlDisableVertexAttribArray>("glDisableVertexAttribArray");
    glx.vertex_attrib_pointer = load_gl_function<GlVertexAttribPointer>("glVertexAttribPointer");
    glx.gen_vertex_arrays = load_gl_function<GlGenVertexArrays>("glGenVertexArrays");
    glx.bind_vertex_array = load_gl_function<GlBindVertexArray>("glBindVertexArray");
    glx.delete_vertex_arrays = load_gl_function<GlDeleteVertexArrays>("glDeleteVertexArrays");
    glx.gen_framebuffers = load_gl_function<GlGenFramebuffers>("glGenFramebuffers");
    glx.bind_framebuffer = load_gl_function<GlBindFramebuffer>("glBindFramebuffer");
    glx.framebuffer_texture_2d = load_gl_function<GlFramebufferTexture2D>("glFramebufferTexture2D");
    glx.check_framebuffer_status = load_gl_function<GlCheckFramebufferStatus>("glCheckFramebufferStatus");
    glx.delete_framebuffers = load_gl_function<GlDeleteFramebuffers>("glDeleteFramebuffers");
    return glx.ready();
}

std::string shader_log(unsigned int shader) {
    int length = 0;
    glx.get_shader_iv(shader, kInfoLogLength, &length);
    std::string log(std::max(1, length), '\0');
    int written = 0;
    glx.get_shader_info_log(shader, static_cast<int>(log.size()), &written, log.data());
    log.resize(std::max(0, written));
    return log;
}

std::string program_log(unsigned int program) {
    int length = 0;
    glx.get_program_iv(program, kInfoLogLength, &length);
    std::string log(std::max(1, length), '\0');
    int written = 0;
    glx.get_program_info_log(program, static_cast<int>(log.size()), &written, log.data());
    log.resize(std::max(0, written));
    return log;
}

unsigned int compile_shader(unsigned int shader_type, const char* source) {
    const unsigned int shader = glx.create_shader(shader_type);
    glx.shader_source(shader, 1, &source, nullptr);
    glx.compile_shader(shader);
    int ok = 0;
    glx.get_shader_iv(shader, kCompileStatus, &ok);
    if (ok == 0) {
        const std::string log = shader_log(shader);
        glx.delete_shader(shader);
        throw std::runtime_error("shader compile failed: " + log);
    }
    return shader;
}

unsigned int create_program(const char* vertex_source, const char* fragment_source) {
    const unsigned int vertex = compile_shader(kVertexShader, vertex_source);
    const unsigned int fragment = compile_shader(kFragmentShader, fragment_source);
    const unsigned int program = glx.create_program();
    glx.attach_shader(program, vertex);
    glx.attach_shader(program, fragment);
    glx.bind_attrib_location(program, 0, "aPosition");
    glx.bind_attrib_location(program, 1, "aNormal");
    glx.bind_attrib_location(program, 2, "aColor");
    glx.bind_attrib_location(program, 3, "aUv");
    glx.bind_attrib_location(program, 4, "aTangent");
    glx.bind_attrib_location(program, 5, "aTangentSign");
    glx.link_program(program);
    glx.delete_shader(vertex);
    glx.delete_shader(fragment);

    int ok = 0;
    glx.get_program_iv(program, kLinkStatus, &ok);
    if (ok == 0) {
        const std::string log = program_log(program);
        glx.delete_program(program);
        throw std::runtime_error("shader link failed: " + log);
    }
    return program;
}

void set_color(Vec3 color, float alpha = 1.0f) {
    glColor4f(color.x, color.y, color.z, alpha);
}

void vertex(Vec3 value) {
    glVertex3f(value.x, value.y, value.z);
}

Vec3 shade(const Material& material, Vec3 normal) {
    const Vec3 light_dir = normalize(Vec3{-0.35f, -0.9f, -0.28f});
    const float ndotl = std::max(0.0f, dot(normalize(normal), light_dir * -1.0f));
    const float wrapped = 0.32f + ndotl * 0.68f;
    return material.base_color * wrapped;
}

std::vector<RenderVertex> cube_vertices() {
    auto t = [](Vec3 normal) {
        if (std::abs(normal.y) > 0.9f) {
            return Vec3{1.0f, 0.0f, 0.0f};
        }
        return normalize(cross(Vec3{0.0f, 1.0f, 0.0f}, normal));
    };
    auto v = [&t](Vec3 p, Vec3 n, Vec2 uv) {
        return RenderVertex{p, n, {1.0f, 1.0f, 1.0f}, uv, t(n), 1.0f};
    };
    return {
        v({-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0, 1}), v({0.5f, -0.5f, 0.5f}, {0, 0, 1}, {1, 1}), v({0.5f, 0.5f, 0.5f}, {0, 0, 1}, {1, 0}), v({-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0, 0}),
        v({0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}), v({-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}), v({-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0}), v({0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0, 0}),
        v({-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0, 1}), v({-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {1, 1}), v({-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {1, 0}), v({-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {0, 0}),
        v({0.5f, -0.5f, 0.5f}, {1, 0, 0}, {0, 1}), v({0.5f, -0.5f, -0.5f}, {1, 0, 0}, {1, 1}), v({0.5f, 0.5f, -0.5f}, {1, 0, 0}, {1, 0}), v({0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0, 0}),
        v({-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0, 1}), v({0.5f, 0.5f, 0.5f}, {0, 1, 0}, {1, 1}), v({0.5f, 0.5f, -0.5f}, {0, 1, 0}, {1, 0}), v({-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0, 0}),
        v({-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 1}), v({0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 1}), v({0.5f, -0.5f, 0.5f}, {0, -1, 0}, {1, 0}), v({-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0, 0}),
    };
}

std::vector<std::uint32_t> quad_face_indices() {
    std::vector<std::uint32_t> indices;
    for (std::uint32_t face = 0; face < 6; ++face) {
        const std::uint32_t base = face * 4;
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return indices;
}

void draw_quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 color) {
    set_color(color);
    glBegin(GL_QUADS);
    vertex(a);
    vertex(b);
    vertex(c);
    vertex(d);
    glEnd();
}

void draw_line(Vec3 a, Vec3 b, Vec3 color, float width = 1.0f) {
    glLineWidth(width);
    set_color(color);
    glBegin(GL_LINES);
    vertex(a);
    vertex(b);
    glEnd();
    glLineWidth(1.0f);
}

} // namespace

Vec3 Camera::position() const {
    const float cp = std::cos(pitch);
    return {
        target.x + std::sin(yaw) * cp * distance,
        target.y + std::sin(pitch) * distance,
        target.z + std::cos(yaw) * cp * distance,
    };
}

Vec3 Camera::forward() const {
    return normalize(target - position());
}

Vec3 Camera::right() const {
    return normalize(cross(forward(), {0.0f, 1.0f, 0.0f}));
}

Vec3 Camera::up() const {
    return normalize(cross(right(), forward()));
}

void Renderer::initialize(HDC device_context) {
    (void)device_context;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.72f, 0.74f, 0.73f, 1.0f);
    try {
        initialize_modern_pipeline();
    } catch (const std::exception&) {
        object_program_ = 0;
        outline_program_ = 0;
        cube_mesh_ = {};
        plane_mesh_ = {};
        sphere_mesh_ = {};
    }
}

void Renderer::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void Renderer::shutdown() {
    for (auto& [part, mesh] : custom_mesh_cache_) {
        (void)part;
        destroy_gpu_mesh(mesh);
    }
    custom_mesh_cache_.clear();
    destroy_gpu_mesh(cube_mesh_);
    destroy_gpu_mesh(plane_mesh_);
    destroy_gpu_mesh(sphere_mesh_);
    for (auto& [path, texture] : texture_cache_) {
        (void)path;
        destroy_texture(texture);
    }
    texture_cache_.clear();
    if (white_texture_ != 0) {
        glDeleteTextures(1, &white_texture_);
        white_texture_ = 0;
    }
    if (object_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(object_program_);
        object_program_ = 0;
    }
    if (outline_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(outline_program_);
        outline_program_ = 0;
    }
    if (depth_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(depth_program_);
        depth_program_ = 0;
    }
    if (tonemap_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(tonemap_program_);
        tonemap_program_ = 0;
    }
    destroy_hdr_framebuffer();
    if (shadow_framebuffer_ != 0 && glx.delete_framebuffers != nullptr) {
        glx.delete_framebuffers(1, &shadow_framebuffer_);
        shadow_framebuffer_ = 0;
    }
    if (shadow_depth_texture_ != 0) {
        glDeleteTextures(1, &shadow_depth_texture_);
        shadow_depth_texture_ = 0;
    }
    if (point_shadow_framebuffer_ != 0 && glx.delete_framebuffers != nullptr) {
        glx.delete_framebuffers(1, &point_shadow_framebuffer_);
        point_shadow_framebuffer_ = 0;
    }
    for (unsigned int& texture : point_shadow_depth_textures_) {
        if (texture != 0) {
            glDeleteTextures(1, &texture);
            texture = 0;
        }
    }
}

Vec2 Renderer::project(Vec3 world, const Camera& camera) const {
    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    const Mat4 vp = Mat4::perspective(camera.fov_y_radians, aspect, 0.05f, 500.0f) * Mat4::look_at(camera.position(), camera.target, {0.0f, 1.0f, 0.0f});
    const float x = world.x * vp.m[0] + world.y * vp.m[4] + world.z * vp.m[8] + vp.m[12];
    const float y = world.x * vp.m[1] + world.y * vp.m[5] + world.z * vp.m[9] + vp.m[13];
    const float w = world.x * vp.m[3] + world.y * vp.m[7] + world.z * vp.m[11] + vp.m[15];
    if (std::abs(w) < 0.0001f) {
        return {-10000.0f, -10000.0f};
    }
    const float ndc_x = x / w;
    const float ndc_y = y / w;
    return {(ndc_x * 0.5f + 0.5f) * width_, (1.0f - (ndc_y * 0.5f + 0.5f)) * height_};
}

void Renderer::render(const Scene& scene, const Camera& camera, const Engine& engine) {
    sync_custom_mesh_cache(scene);
    resize(width_, height_);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 projection = Mat4::perspective(camera.fov_y_radians, aspect, 0.05f, 500.0f);
    const Mat4 view = Mat4::look_at(camera.position(), camera.target, {0.0f, 1.0f, 0.0f});
    const Mat4 view_projection = projection * view;
    ObjectUniforms object_uniforms = collect_object_uniforms(scene, camera, engine);
    object_uniforms.hdr_output_enabled = engine.render_settings().hdr_enabled && tonemap_program_ != 0;

    if (object_uniforms.shadows_enabled) {
        render_shadow_map(scene, object_uniforms);
    }
    if (object_uniforms.point_shadows_enabled) {
        render_point_shadow_maps(scene, object_uniforms);
    }

    const bool use_hdr = object_uniforms.hdr_output_enabled;
    if (use_hdr) {
        ensure_hdr_framebuffer();
        glx.bind_framebuffer(kFramebuffer, hdr_framebuffer_);
    } else if (glx.ready()) {
        glx.bind_framebuffer(kFramebuffer, 0);
        glDrawBuffer(kBack);
        glReadBuffer(kBack);
    }

    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m.data());
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m.data());

    if (engine.render_settings().draw_world_grid) {
        draw_grid(camera);
    }

    if (engine.render_settings().draw_actor_outlines && outline_program_ != 0) {
        for (const auto& actor : scene.actors()) {
            draw_actor_outline(*actor, view_projection);
        }
    }

    for (const auto& actor : scene.actors()) {
        draw_actor(*actor, actor->id() == scene.selected_actor_id(), view_projection, object_uniforms);
    }

    if (use_hdr) {
        glx.bind_framebuffer(kFramebuffer, 0);
        glDrawBuffer(kBack);
        glReadBuffer(kBack);
        glViewport(0, 0, width_, height_);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        render_tonemapped_frame(engine.render_settings().exposure, object_uniforms.debug_view);
        glDrawBuffer(kBack);
        glReadBuffer(kBack);
        glClear(GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(projection.m.data());
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view.m.data());
    }

    if (const Actor* selected = scene.selected_actor()) {
        if (engine.render_settings().draw_editor_gizmos) {
            glClear(GL_DEPTH_BUFFER_BIT);
            draw_gizmo(*selected, engine.active_tool(), engine.transform_space());
        }
    }
}

void Renderer::draw_grid(const Camera& camera) const {
    glDisable(GL_CULL_FACE);
    const Vec3 minor{0.30f, 0.45f, 0.43f};
    const Vec3 major{0.88f, 0.80f, 0.10f};
    constexpr int half = 80;
    const Vec3 camera_position = camera.position();
    const float origin_x = std::round(camera_position.x / 10.0f) * 10.0f;
    const float origin_z = std::round(camera_position.z / 10.0f) * 10.0f;
    for (int i = -half; i <= half; ++i) {
        const bool axis = i == 0;
        const float width = axis ? 2.0f : 1.0f;
        const float f = static_cast<float>(i);
        draw_line({origin_x + f, 0.0f, origin_z - half}, {origin_x + f, 0.0f, origin_z + half}, axis ? major : minor, width);
        draw_line({origin_x - half, 0.0f, origin_z + f}, {origin_x + half, 0.0f, origin_z + f}, axis ? major : minor, width);
    }
    glEnable(GL_CULL_FACE);
}

bool Renderer::initialize_modern_pipeline() {
    if (!load_modern_gl()) {
        return false;
    }

    constexpr const char* vertex_shader = R"(
#version 120
attribute vec3 aPosition;
attribute vec3 aNormal;
attribute vec3 aColor;
attribute vec2 aUv;
attribute vec3 aTangent;
attribute float aTangentSign;
uniform mat4 uModel;
uniform mat4 uMvp;
uniform mat4 uLightViewProjection;
varying vec3 vWorldPosition;
varying vec3 vNormal;
varying vec3 vColor;
varying vec2 vUv;
varying vec3 vTangent;
varying float vTangentSign;
varying vec4 vShadowPosition;
void main() {
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vColor = aColor;
    vUv = aUv;
    vTangent = normalize(mat3(uModel) * aTangent);
    vTangentSign = aTangentSign;
    vShadowPosition = uLightViewProjection * worldPosition;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    constexpr const char* fragment_shader = R"(
#version 120
uniform vec3 uBaseColor;
uniform float uRoughness;
uniform float uMetallic;
uniform float uNormalStrength;
uniform vec3 uCameraPosition;
uniform vec3 uDirectionalDirection;
uniform vec3 uDirectionalColor;
uniform float uDirectionalIntensity;
uniform vec3 uPointPosition;
uniform vec3 uPointColor;
uniform float uPointIntensity;
uniform float uPointRange;
uniform int uDebugView;
uniform sampler2D uBaseColorTexture;
uniform int uHasBaseColorTexture;
uniform sampler2D uMetallicRoughnessTexture;
uniform int uHasMetallicRoughnessTexture;
uniform sampler2D uNormalTexture;
uniform int uHasNormalTexture;
uniform sampler2D uShadowMap;
uniform int uShadowsEnabled;
uniform int uHdrOutputEnabled;
uniform sampler2D uPointShadowMap0;
uniform sampler2D uPointShadowMap1;
uniform sampler2D uPointShadowMap2;
uniform sampler2D uPointShadowMap3;
uniform sampler2D uPointShadowMap4;
uniform sampler2D uPointShadowMap5;
uniform mat4 uPointLightViewProjection0;
uniform mat4 uPointLightViewProjection1;
uniform mat4 uPointLightViewProjection2;
uniform mat4 uPointLightViewProjection3;
uniform mat4 uPointLightViewProjection4;
uniform mat4 uPointLightViewProjection5;
uniform int uPointShadowsEnabled;
varying vec3 vWorldPosition;
varying vec3 vNormal;
varying vec3 vColor;
varying vec2 vUv;
varying vec3 vTangent;
varying float vTangentSign;
varying vec4 vShadowPosition;

const float PI = 3.14159265359;

float distribution_ggx(vec3 n, vec3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(n, h), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denom = (ndoth2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0001);
}

float geometry_schlick_ggx(float ndotv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float geometry_smith(vec3 n, vec3 v, vec3 l, float roughness) {
    return geometry_schlick_ggx(max(dot(n, v), 0.0), roughness)
        * geometry_schlick_ggx(max(dot(n, l), 0.0), roughness);
}

vec3 fresnel_schlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 pbr_light(vec3 albedo, float metallic, float roughness, vec3 n, vec3 v, vec3 l, vec3 radiance) {
    vec3 h = normalize(v + l);
    float ndotl = max(dot(n, l), 0.0);
    float ndotv = max(dot(n, v), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    float d = distribution_ggx(n, h, roughness);
    float g = geometry_smith(n, v, l, roughness);
    vec3 f = fresnel_schlick(max(dot(h, v), 0.0), f0);

    vec3 specular = (d * g * f) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;
    return (diffuse + specular) * radiance * ndotl;
}

float directional_shadow(vec3 n, vec3 light_dir) {
    if (uShadowsEnabled == 0) {
        return 1.0;
    }
    vec3 projected = vShadowPosition.xyz / max(vShadowPosition.w, 0.0001);
    projected = projected * 0.5 + 0.5;
    if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0 || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    float bias = max(0.0015 * (1.0 - dot(n, light_dir)), 0.00045);
    float shadow = 0.0;
    float texel = 1.0 / 2048.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = texture2D(uShadowMap, projected.xy + vec2(float(x), float(y)) * texel).r;
            shadow += projected.z - bias > closest ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    return mix(0.38, 1.0, shadow);
}

float sample_point_shadow_map(int face, vec2 uv) {
    if (face == 0) {
        return texture2D(uPointShadowMap0, uv).r;
    } else if (face == 1) {
        return texture2D(uPointShadowMap1, uv).r;
    } else if (face == 2) {
        return texture2D(uPointShadowMap2, uv).r;
    } else if (face == 3) {
        return texture2D(uPointShadowMap3, uv).r;
    } else if (face == 4) {
        return texture2D(uPointShadowMap4, uv).r;
    }
    return texture2D(uPointShadowMap5, uv).r;
}

vec4 point_shadow_position(int face, vec3 world_position) {
    if (face == 0) {
        return uPointLightViewProjection0 * vec4(world_position, 1.0);
    } else if (face == 1) {
        return uPointLightViewProjection1 * vec4(world_position, 1.0);
    } else if (face == 2) {
        return uPointLightViewProjection2 * vec4(world_position, 1.0);
    } else if (face == 3) {
        return uPointLightViewProjection3 * vec4(world_position, 1.0);
    } else if (face == 4) {
        return uPointLightViewProjection4 * vec4(world_position, 1.0);
    }
    return uPointLightViewProjection5 * vec4(world_position, 1.0);
}

float point_shadow(vec3 n, vec3 light_dir) {
    if (uPointShadowsEnabled == 0) {
        return 1.0;
    }

    vec3 fromLight = vWorldPosition - uPointPosition;
    vec3 absFromLight = abs(fromLight);
    int face = 0;
    if (absFromLight.x >= absFromLight.y && absFromLight.x >= absFromLight.z) {
        face = fromLight.x >= 0.0 ? 0 : 1;
    } else if (absFromLight.y >= absFromLight.x && absFromLight.y >= absFromLight.z) {
        face = fromLight.y >= 0.0 ? 2 : 3;
    } else {
        face = fromLight.z >= 0.0 ? 4 : 5;
    }

    vec4 shadowPosition = point_shadow_position(face, vWorldPosition);
    vec3 projected = shadowPosition.xyz / max(shadowPosition.w, 0.0001);
    projected = projected * 0.5 + 0.5;
    if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0 || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    float bias = max(0.0035 * (1.0 - dot(n, light_dir)), 0.0012);
    float shadow = 0.0;
    float texel = 1.0 / 1024.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = sample_point_shadow_map(face, projected.xy + vec2(float(x), float(y)) * texel);
            shadow += projected.z - bias > closest ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    return mix(0.30, 1.0, shadow);
}

void main() {
    vec3 n = normalize(vNormal);
    vec3 t = normalize(vTangent - n * dot(n, vTangent));
    vec3 b = normalize(cross(n, t) * vTangentSign);
    if (uHasNormalTexture == 1) {
        vec3 tangentNormal = texture2D(uNormalTexture, vUv).xyz * 2.0 - 1.0;
        tangentNormal.xy *= uNormalStrength;
        tangentNormal = normalize(tangentNormal);
        n = normalize(t * tangentNormal.x + b * tangentNormal.y + n * tangentNormal.z);
    }
    vec3 v = normalize(uCameraPosition - vWorldPosition);
    vec3 sampledBaseColor = uHasBaseColorTexture == 1 ? texture2D(uBaseColorTexture, vUv).rgb : vec3(1.0);
    vec3 albedo = clamp(uBaseColor * vColor * sampledBaseColor, 0.0, 1.0);
    vec4 sampledMetallicRoughness = uHasMetallicRoughnessTexture == 1 ? texture2D(uMetallicRoughnessTexture, vUv) : vec4(1.0);
    float roughness = clamp(uRoughness * sampledMetallicRoughness.g, 0.04, 1.0);
    float metallic = clamp(uMetallic * sampledMetallicRoughness.b, 0.0, 1.0);

    vec3 direct = vec3(0.0);
    vec3 directionalLight = normalize(uDirectionalDirection);
    float shadow = directional_shadow(n, directionalLight);
    direct += pbr_light(albedo, metallic, roughness, n, v, directionalLight, uDirectionalColor * uDirectionalIntensity) * shadow;

    vec3 pointVector = uPointPosition - vWorldPosition;
    float pointDistance = length(pointVector);
    if (uPointIntensity > 0.0 && pointDistance < uPointRange) {
        vec3 pointL = pointVector / max(pointDistance, 0.0001);
        float attenuation = pow(clamp(1.0 - pointDistance / max(uPointRange, 0.0001), 0.0, 1.0), 2.0);
        float pointShadow = point_shadow(n, pointL);
        direct += pbr_light(albedo, metallic, roughness, n, v, pointL, uPointColor * uPointIntensity * attenuation) * pointShadow;
    }

    vec3 ambient = albedo * mix(0.035, 0.075, 1.0 - roughness);
    vec3 hdrColor = ambient + direct;
    vec3 ldrColor = hdrColor / (hdrColor + vec3(1.0));
    ldrColor = pow(ldrColor, vec3(1.0 / 2.2));

    if (uDebugView == 1) {
        gl_FragColor = vec4(albedo, 1.0);
    } else if (uDebugView == 2) {
        gl_FragColor = vec4(n * 0.5 + 0.5, 1.0);
    } else if (uDebugView == 3) {
        vec3 lightingOnly = direct / max(albedo, vec3(0.04));
        gl_FragColor = vec4(lightingOnly / (lightingOnly + vec3(1.0)), 1.0);
    } else if (uDebugView == 4) {
        gl_FragColor = vec4(vec3(roughness), 1.0);
    } else if (uDebugView == 5) {
        gl_FragColor = vec4(vec3(metallic), 1.0);
    } else {
        gl_FragColor = vec4(uHdrOutputEnabled == 1 ? hdrColor : ldrColor, 1.0);
    }
}
)";

    constexpr const char* outline_vertex_shader = R"(
#version 120
attribute vec3 aPosition;
attribute vec3 aNormal;
uniform mat4 uMvp;
uniform float uOutlineWidth;
void main() {
    vec3 normal = normalize(aNormal);
    gl_Position = uMvp * vec4(aPosition + normal * uOutlineWidth, 1.0);
}
)";

    constexpr const char* outline_fragment_shader = R"(
#version 120
uniform vec3 uOutlineColor;
void main() {
    gl_FragColor = vec4(uOutlineColor, 1.0);
}
)";

    constexpr const char* depth_vertex_shader = R"(
#version 120
attribute vec3 aPosition;
uniform mat4 uMvp;
void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    constexpr const char* depth_fragment_shader = R"(
#version 120
void main() {
}
)";

    constexpr const char* tonemap_vertex_shader = R"(
#version 120
varying vec2 vUv;
void main() {
    vUv = gl_MultiTexCoord0.xy;
    gl_Position = gl_Vertex;
}
)";

    constexpr const char* tonemap_fragment_shader = R"(
#version 120
uniform sampler2D uSceneColor;
uniform float uExposure;
uniform int uDebugView;
varying vec2 vUv;

vec3 aces_fitted(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdrColor = texture2D(uSceneColor, vUv).rgb;
    vec3 exposed = hdrColor * max(uExposure, 0.0);
    vec3 mapped = uDebugView == 0 ? aces_fitted(exposed) : clamp(hdrColor, 0.0, 1.0);
    vec3 outputColor = uDebugView == 0 ? pow(mapped, vec3(1.0 / 2.2)) : mapped;
    gl_FragColor = vec4(outputColor, 1.0);
}
)";

    object_program_ = create_program(vertex_shader, fragment_shader);
    outline_program_ = create_program(outline_vertex_shader, outline_fragment_shader);
    depth_program_ = create_program(depth_vertex_shader, depth_fragment_shader);
    tonemap_program_ = create_program(tonemap_vertex_shader, tonemap_fragment_shader);

    glGenTextures(1, &shadow_depth_texture_);
    glBindTexture(kTexture2D, shadow_depth_texture_);
    glTexImage2D(kTexture2D, 0, static_cast<int>(kDepthComponent), shadow_resolution_, shadow_resolution_, 0, kDepthComponent, kFloat, nullptr);
    glTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
    glTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
    glTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
    glTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);

    glx.gen_framebuffers(1, &shadow_framebuffer_);
    glx.bind_framebuffer(kFramebuffer, shadow_framebuffer_);
    glx.framebuffer_texture_2d(kFramebuffer, kDepthAttachment, kTexture2D, shadow_depth_texture_, 0);
    glDrawBuffer(kNone);
    glReadBuffer(kNone);
    if (glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
        glx.bind_framebuffer(kFramebuffer, 0);
        throw std::runtime_error("shadow framebuffer is incomplete");
    }
    glx.bind_framebuffer(kFramebuffer, 0);

    glx.gen_framebuffers(1, &point_shadow_framebuffer_);
    for (unsigned int& texture : point_shadow_depth_textures_) {
        glGenTextures(1, &texture);
        glBindTexture(kTexture2D, texture);
        glTexImage2D(kTexture2D, 0, static_cast<int>(kDepthComponent), point_shadow_resolution_, point_shadow_resolution_, 0, kDepthComponent, kFloat, nullptr);
        glTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        glTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        glTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
        glTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
    }
    glx.bind_framebuffer(kFramebuffer, point_shadow_framebuffer_);
    glDrawBuffer(kNone);
    glReadBuffer(kNone);
    for (unsigned int texture : point_shadow_depth_textures_) {
        glx.framebuffer_texture_2d(kFramebuffer, kDepthAttachment, kTexture2D, texture, 0);
        if (glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
            glx.bind_framebuffer(kFramebuffer, 0);
            throw std::runtime_error("point shadow framebuffer is incomplete");
        }
    }
    glx.bind_framebuffer(kFramebuffer, 0);

    const unsigned char white_pixel[] = {255, 255, 255, 255};
    glGenTextures(1, &white_texture_);
    glBindTexture(kTexture2D, white_texture_);
    glTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
    glTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
    glTexParameteri(kTexture2D, kTextureWrapS, kRepeat);
    glTexParameteri(kTexture2D, kTextureWrapT, kRepeat);
    glTexImage2D(kTexture2D, 0, static_cast<int>(kRgba), 1, 1, 0, kRgba, kUnsignedByte, white_pixel);
    glBindTexture(kTexture2D, 0);
    cube_mesh_ = create_cube_mesh();
    plane_mesh_ = create_plane_mesh();
    sphere_mesh_ = create_sphere_mesh();
    return true;
}

GpuMesh Renderer::create_gpu_mesh(const std::vector<RenderVertex>& vertices, const std::vector<std::uint32_t>& indices) const {
    GpuMesh mesh;
    if (!glx.ready() || vertices.empty() || indices.empty()) {
        return mesh;
    }

    if (glx.gen_vertex_arrays != nullptr && glx.bind_vertex_array != nullptr) {
        glx.gen_vertex_arrays(1, &mesh.vertex_array);
        glx.bind_vertex_array(mesh.vertex_array);
    }

    glx.gen_buffers(1, &mesh.vertex_buffer);
    glx.bind_buffer(kArrayBuffer, mesh.vertex_buffer);
    glx.buffer_data(kArrayBuffer, static_cast<std::ptrdiff_t>(vertices.size() * sizeof(RenderVertex)), vertices.data(), kStaticDraw);

    glx.gen_buffers(1, &mesh.index_buffer);
    glx.bind_buffer(kElementArrayBuffer, mesh.index_buffer);
    glx.buffer_data(kElementArrayBuffer, static_cast<std::ptrdiff_t>(indices.size() * sizeof(std::uint32_t)), indices.data(), kStaticDraw);

    glx.enable_vertex_attrib_array(0);
    glx.vertex_attrib_pointer(0, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, position)));
    glx.enable_vertex_attrib_array(1);
    glx.vertex_attrib_pointer(1, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, normal)));
    glx.enable_vertex_attrib_array(2);
    glx.vertex_attrib_pointer(2, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, color)));
    glx.enable_vertex_attrib_array(3);
    glx.vertex_attrib_pointer(3, 2, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, uv)));
    glx.enable_vertex_attrib_array(4);
    glx.vertex_attrib_pointer(4, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, tangent)));
    glx.enable_vertex_attrib_array(5);
    glx.vertex_attrib_pointer(5, 1, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, tangent_sign)));

    if (glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(0);
    }
    glx.bind_buffer(kArrayBuffer, 0);
    glx.bind_buffer(kElementArrayBuffer, 0);
    mesh.index_count = static_cast<int>(indices.size());
    return mesh;
}

ObjectUniforms Renderer::collect_object_uniforms(const Scene& scene, const Camera& camera, const Engine& engine) const {
    ObjectUniforms uniforms;
    uniforms.camera_position = camera.position();
    uniforms.shadows_enabled = engine.render_settings().shadows_enabled;

    switch (engine.debug_view()) {
    case DebugView::Final:
        uniforms.debug_view = 0;
        break;
    case DebugView::Albedo:
        uniforms.debug_view = 1;
        break;
    case DebugView::Normals:
        uniforms.debug_view = 2;
        break;
    case DebugView::LightingOnly:
        uniforms.debug_view = 3;
        break;
    case DebugView::Roughness:
        uniforms.debug_view = 4;
        break;
    case DebugView::Metallic:
        uniforms.debug_view = 5;
        break;
    }

    bool found_directional = false;
    bool found_point = false;
    for (const auto& actor : scene.actors()) {
        const auto* light = actor->find_component<LightComponent>();
        if (light == nullptr || !light->enabled) {
            continue;
        }

        const Transform& transform = actor->root().local_transform;
        if (light->light_kind == LightKind::Directional && !found_directional) {
            const Mat4 rotation = Mat4::rotation_xyz(transform.rotation);
            uniforms.directional_direction = normalize({
                0.0f * rotation.m[0] + -1.0f * rotation.m[4] + 0.0f * rotation.m[8],
                0.0f * rotation.m[1] + -1.0f * rotation.m[5] + 0.0f * rotation.m[9],
                0.0f * rotation.m[2] + -1.0f * rotation.m[6] + 0.0f * rotation.m[10],
            }) * -1.0f;
            uniforms.directional_color = light->color;
            uniforms.directional_intensity = light->intensity;
            found_directional = true;
        } else if (light->light_kind == LightKind::Point && !found_point) {
            uniforms.point_position = transform.translation;
            uniforms.point_color = light->color;
            uniforms.point_intensity = light->intensity;
            uniforms.point_range = light->range;
            found_point = true;
        }
    }

    if (!found_directional) {
        uniforms.directional_intensity = 0.0f;
    }
    const Vec3 light_direction = length(uniforms.directional_direction) > 0.0001f ? normalize(uniforms.directional_direction) : Vec3{0.35f, 0.9f, 0.28f};
    const Vec3 light_target = camera.target;
    const Vec3 light_position = light_target + light_direction * 18.0f;
    const Mat4 light_view = Mat4::look_at(light_position, light_target, {0.0f, 1.0f, 0.0f});
    const Mat4 light_projection = Mat4::orthographic(-12.0f, 12.0f, -12.0f, 12.0f, 0.1f, 45.0f);
    uniforms.light_view_projection = light_projection * light_view;
    uniforms.shadows_enabled = uniforms.shadows_enabled && found_directional && shadow_framebuffer_ != 0 && shadow_depth_texture_ != 0 && depth_program_ != 0;
    if (found_point) {
        const float far_plane = std::max(1.0f, uniforms.point_range);
        const Mat4 point_projection = Mat4::perspective(1.57079632679f, 1.0f, 0.05f, far_plane);
        const std::array<Vec3, 6> directions{
            Vec3{1.0f, 0.0f, 0.0f},
            Vec3{-1.0f, 0.0f, 0.0f},
            Vec3{0.0f, 1.0f, 0.0f},
            Vec3{0.0f, -1.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f},
            Vec3{0.0f, 0.0f, -1.0f},
        };
        const std::array<Vec3, 6> up_vectors{
            Vec3{0.0f, -1.0f, 0.0f},
            Vec3{0.0f, -1.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f},
            Vec3{0.0f, 0.0f, -1.0f},
            Vec3{0.0f, -1.0f, 0.0f},
            Vec3{0.0f, -1.0f, 0.0f},
        };
        for (std::size_t index = 0; index < uniforms.point_light_view_projections.size(); ++index) {
            const Mat4 point_view = Mat4::look_at(uniforms.point_position, uniforms.point_position + directions[index], up_vectors[index]);
            uniforms.point_light_view_projections[index] = point_projection * point_view;
        }
    }
    uniforms.point_shadows_enabled = engine.render_settings().shadows_enabled
        && found_point
        && point_shadow_framebuffer_ != 0
        && point_shadow_depth_textures_[0] != 0
        && depth_program_ != 0;
    return uniforms;
}

void Renderer::sync_custom_mesh_cache(const Scene& scene) {
    if (object_program_ == 0) {
        return;
    }

    std::unordered_set<const MeshPart*> active_parts;
    for (const auto& actor : scene.actors()) {
        const auto* mesh = actor->find_component<StaticMeshComponent>();
        if (mesh == nullptr || !mesh->visible) {
            continue;
        }
        for (const MeshPart& part : mesh->parts) {
            if (part.primitive == MeshPrimitive::Custom) {
                active_parts.insert(&part);
                if (!custom_mesh_cache_.contains(&part)) {
                    custom_mesh_cache_.emplace(&part, create_custom_mesh(part));
                }
            }
        }
    }

    for (auto it = custom_mesh_cache_.begin(); it != custom_mesh_cache_.end();) {
        if (active_parts.contains(it->first)) {
            ++it;
        } else {
            destroy_gpu_mesh(it->second);
            it = custom_mesh_cache_.erase(it);
        }
    }
}

GpuMesh Renderer::create_custom_mesh(const MeshPart& part) const {
    std::vector<RenderVertex> vertices;
    vertices.reserve(part.vertices.size());
    for (const Vertex& vertex : part.vertices) {
        vertices.push_back({vertex.position, vertex.normal, vertex.color, vertex.uv, vertex.tangent, vertex.tangent_sign});
    }
    return create_gpu_mesh(vertices, part.indices);
}

void Renderer::destroy_gpu_mesh(GpuMesh& mesh) const {
    if (!glx.ready()) {
        mesh = {};
        return;
    }
    if (mesh.vertex_buffer != 0) {
        glx.delete_buffers(1, &mesh.vertex_buffer);
    }
    if (mesh.index_buffer != 0) {
        glx.delete_buffers(1, &mesh.index_buffer);
    }
    if (mesh.vertex_array != 0 && glx.delete_vertex_arrays != nullptr) {
        glx.delete_vertex_arrays(1, &mesh.vertex_array);
    }
    mesh = {};
}

GpuTexture Renderer::create_texture_from_file(const std::string& path) const {
    GpuTexture texture;
    try {
        const TextureImage image = load_texture_image(std::filesystem::path(path));
        if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
            texture.failed = true;
            return texture;
        }

        glGenTextures(1, &texture.id);
        glBindTexture(kTexture2D, texture.id);
        glPixelStorei(kUnpackAlignment, 1);
        glTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
        glTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
        glTexParameteri(kTexture2D, kTextureWrapS, kRepeat);
        glTexParameteri(kTexture2D, kTextureWrapT, kRepeat);
        glTexImage2D(kTexture2D, 0, static_cast<int>(kRgba), image.width, image.height, 0, kRgba, kUnsignedByte, image.rgba.data());
        glBindTexture(kTexture2D, 0);
        texture.width = image.width;
        texture.height = image.height;
    } catch (const std::exception&) {
        texture.failed = true;
    }
    return texture;
}

unsigned int Renderer::resolve_base_color_texture(const Material& material) const {
    if (material.base_color_texture_path.empty()) {
        return white_texture_;
    }

    auto it = texture_cache_.find(material.base_color_texture_path);
    if (it == texture_cache_.end()) {
        it = texture_cache_.emplace(material.base_color_texture_path, create_texture_from_file(material.base_color_texture_path)).first;
    }
    if (it->second.failed || it->second.id == 0) {
        return white_texture_;
    }
    return it->second.id;
}

unsigned int Renderer::resolve_metallic_roughness_texture(const Material& material) const {
    if (material.metallic_roughness_texture_path.empty()) {
        return white_texture_;
    }

    auto it = texture_cache_.find(material.metallic_roughness_texture_path);
    if (it == texture_cache_.end()) {
        it = texture_cache_.emplace(material.metallic_roughness_texture_path, create_texture_from_file(material.metallic_roughness_texture_path)).first;
    }
    if (it->second.failed || it->second.id == 0) {
        return white_texture_;
    }
    return it->second.id;
}

unsigned int Renderer::resolve_normal_texture(const Material& material) const {
    if (material.normal_texture_path.empty()) {
        return white_texture_;
    }

    auto it = texture_cache_.find(material.normal_texture_path);
    if (it == texture_cache_.end()) {
        it = texture_cache_.emplace(material.normal_texture_path, create_texture_from_file(material.normal_texture_path)).first;
    }
    if (it->second.failed || it->second.id == 0) {
        return white_texture_;
    }
    return it->second.id;
}

void Renderer::destroy_texture(GpuTexture& texture) const {
    if (texture.id != 0) {
        glDeleteTextures(1, &texture.id);
    }
    texture = {};
}

void Renderer::ensure_hdr_framebuffer() {
    if (hdr_framebuffer_ != 0 && hdr_width_ == width_ && hdr_height_ == height_) {
        return;
    }

    destroy_hdr_framebuffer();
    hdr_width_ = width_;
    hdr_height_ = height_;

    glGenTextures(1, &hdr_color_texture_);
    glBindTexture(kTexture2D, hdr_color_texture_);
    glTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
    glTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
    glTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
    glTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
    glTexImage2D(kTexture2D, 0, static_cast<int>(kRgba16f), hdr_width_, hdr_height_, 0, kRgba, kFloat, nullptr);

    glGenTextures(1, &hdr_depth_texture_);
    glBindTexture(kTexture2D, hdr_depth_texture_);
    glTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
    glTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
    glTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
    glTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
    glTexImage2D(kTexture2D, 0, static_cast<int>(kDepthComponent), hdr_width_, hdr_height_, 0, kDepthComponent, kFloat, nullptr);

    glx.gen_framebuffers(1, &hdr_framebuffer_);
    glx.bind_framebuffer(kFramebuffer, hdr_framebuffer_);
    glx.framebuffer_texture_2d(kFramebuffer, kColorAttachment0, kTexture2D, hdr_color_texture_, 0);
    glx.framebuffer_texture_2d(kFramebuffer, kDepthAttachment, kTexture2D, hdr_depth_texture_, 0);
    glDrawBuffer(kColorAttachment0);
    glReadBuffer(kColorAttachment0);
    if (glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
        glx.bind_framebuffer(kFramebuffer, 0);
        throw std::runtime_error("HDR framebuffer is incomplete");
    }
    glx.bind_framebuffer(kFramebuffer, 0);
}

void Renderer::destroy_hdr_framebuffer() {
    if (hdr_framebuffer_ != 0 && glx.delete_framebuffers != nullptr) {
        glx.delete_framebuffers(1, &hdr_framebuffer_);
        hdr_framebuffer_ = 0;
    }
    if (hdr_color_texture_ != 0) {
        glDeleteTextures(1, &hdr_color_texture_);
        hdr_color_texture_ = 0;
    }
    if (hdr_depth_texture_ != 0) {
        glDeleteTextures(1, &hdr_depth_texture_);
        hdr_depth_texture_ = 0;
    }
    hdr_width_ = 0;
    hdr_height_ = 0;
}

void Renderer::render_tonemapped_frame(float exposure, int debug_view) const {
    if (tonemap_program_ == 0 || hdr_color_texture_ == 0) {
        return;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glx.use_program(tonemap_program_);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uSceneColor"), 0);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uExposure"), exposure);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uDebugView"), debug_view);
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, hdr_color_texture_);

    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(1.0f, -1.0f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(-1.0f, 1.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(1.0f, 1.0f);
    glEnd();

    glBindTexture(kTexture2D, 0);
    glx.use_program(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::draw_gpu_mesh(const GpuMesh& mesh, const Material& material, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms) {
    if (object_program_ == 0 || mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

    const Mat4 model = transform.matrix();
    const Mat4 mvp = view_projection * model;
    glx.use_program(object_program_);
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uModel"), 1, kFalse, model.m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uMvp"), 1, kFalse, mvp.m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uLightViewProjection"), 1, kFalse, uniforms.light_view_projection.m.data());
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uBaseColor"), material.base_color.x, material.base_color.y, material.base_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uRoughness"), material.roughness);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uMetallic"), material.metallic);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uNormalStrength"), material.normal_strength);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uCameraPosition"), uniforms.camera_position.x, uniforms.camera_position.y, uniforms.camera_position.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uDirectionalDirection"), uniforms.directional_direction.x, uniforms.directional_direction.y, uniforms.directional_direction.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uDirectionalColor"), uniforms.directional_color.x, uniforms.directional_color.y, uniforms.directional_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uDirectionalIntensity"), uniforms.directional_intensity);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uPointPosition"), uniforms.point_position.x, uniforms.point_position.y, uniforms.point_position.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uPointColor"), uniforms.point_color.x, uniforms.point_color.y, uniforms.point_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uPointIntensity"), uniforms.point_intensity);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uPointRange"), uniforms.point_range);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uDebugView"), uniforms.debug_view);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uBaseColorTexture"), 0);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasBaseColorTexture"), material.base_color_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uMetallicRoughnessTexture"), 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasMetallicRoughnessTexture"), material.metallic_roughness_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uNormalTexture"), 2);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasNormalTexture"), material.normal_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uShadowMap"), 3);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uShadowsEnabled"), uniforms.shadows_enabled ? 1 : 0);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHdrOutputEnabled"), uniforms.hdr_output_enabled ? 1 : 0);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowMap0"), 4);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowMap1"), 5);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowMap2"), 6);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowMap3"), 7);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowMap4"), 8);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowMap5"), 9);
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uPointLightViewProjection0"), 1, kFalse, uniforms.point_light_view_projections[0].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uPointLightViewProjection1"), 1, kFalse, uniforms.point_light_view_projections[1].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uPointLightViewProjection2"), 1, kFalse, uniforms.point_light_view_projections[2].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uPointLightViewProjection3"), 1, kFalse, uniforms.point_light_view_projections[3].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uPointLightViewProjection4"), 1, kFalse, uniforms.point_light_view_projections[4].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uPointLightViewProjection5"), 1, kFalse, uniforms.point_light_view_projections[5].m.data());
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPointShadowsEnabled"), uniforms.point_shadows_enabled ? 1 : 0);
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, resolve_base_color_texture(material));
    glx.active_texture(kTexture1);
    glBindTexture(kTexture2D, resolve_metallic_roughness_texture(material));
    glx.active_texture(kTexture2);
    glBindTexture(kTexture2D, resolve_normal_texture(material));
    glx.active_texture(kTexture3);
    glBindTexture(kTexture2D, shadow_depth_texture_);
    const std::array<unsigned int, 6> point_texture_units{kTexture4, kTexture5, kTexture6, kTexture7, kTexture8, kTexture9};
    for (std::size_t index = 0; index < point_shadow_depth_textures_.size(); ++index) {
        glx.active_texture(point_texture_units[index]);
        glBindTexture(kTexture2D, point_shadow_depth_textures_[index]);
    }
    glx.active_texture(kTexture0);

    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(mesh.vertex_array);
    } else {
        glx.bind_buffer(kArrayBuffer, mesh.vertex_buffer);
        glx.enable_vertex_attrib_array(0);
        glx.vertex_attrib_pointer(0, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, position)));
        glx.enable_vertex_attrib_array(1);
        glx.vertex_attrib_pointer(1, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, normal)));
        glx.enable_vertex_attrib_array(2);
        glx.vertex_attrib_pointer(2, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, color)));
        glx.enable_vertex_attrib_array(3);
        glx.vertex_attrib_pointer(3, 2, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, uv)));
        glx.enable_vertex_attrib_array(4);
        glx.vertex_attrib_pointer(4, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, tangent)));
        glx.enable_vertex_attrib_array(5);
        glx.vertex_attrib_pointer(5, 1, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, tangent_sign)));
    }
    glx.bind_buffer(kElementArrayBuffer, mesh.index_buffer);
    glDrawElements(kTriangles, mesh.index_count, kUnsignedInt, nullptr);

    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(0);
    } else {
        glx.disable_vertex_attrib_array(0);
        glx.disable_vertex_attrib_array(1);
        glx.disable_vertex_attrib_array(2);
        glx.disable_vertex_attrib_array(3);
        glx.disable_vertex_attrib_array(4);
        glx.disable_vertex_attrib_array(5);
    }
    glx.bind_buffer(kArrayBuffer, 0);
    glx.bind_buffer(kElementArrayBuffer, 0);
    for (std::size_t index = 0; index < point_texture_units.size(); ++index) {
        glx.active_texture(point_texture_units[index]);
        glBindTexture(kTexture2D, 0);
    }
    glx.active_texture(kTexture3);
    glBindTexture(kTexture2D, 0);
    glx.active_texture(kTexture2);
    glBindTexture(kTexture2D, 0);
    glx.active_texture(kTexture1);
    glBindTexture(kTexture2D, 0);
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, 0);
    glx.use_program(0);
}

void Renderer::render_shadow_map(const Scene& scene, const ObjectUniforms& uniforms) {
    if (!uniforms.shadows_enabled) {
        return;
    }

    glx.bind_framebuffer(kFramebuffer, shadow_framebuffer_);
    glViewport(0, 0, shadow_resolution_, shadow_resolution_);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);

    for (const auto& actor : scene.actors()) {
        draw_depth_actor(*actor, uniforms.light_view_projection);
    }

    glCullFace(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
}

void Renderer::render_point_shadow_maps(const Scene& scene, const ObjectUniforms& uniforms) {
    if (!uniforms.point_shadows_enabled) {
        return;
    }

    glx.bind_framebuffer(kFramebuffer, point_shadow_framebuffer_);
    glViewport(0, 0, point_shadow_resolution_, point_shadow_resolution_);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glCullFace(GL_FRONT);

    for (std::size_t face = 0; face < point_shadow_depth_textures_.size(); ++face) {
        glx.framebuffer_texture_2d(kFramebuffer, kDepthAttachment, kTexture2D, point_shadow_depth_textures_[face], 0);
        glClear(GL_DEPTH_BUFFER_BIT);
        for (const auto& actor : scene.actors()) {
            draw_depth_actor(*actor, uniforms.point_light_view_projections[face]);
        }
    }

    glCullFace(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
}

void Renderer::draw_depth_actor(const Actor& actor, const Mat4& light_view_projection) {
    const auto* mesh = actor.find_component<StaticMeshComponent>();
    if (mesh == nullptr || !mesh->visible) {
        return;
    }

    for (const auto& part : mesh->parts) {
        draw_depth_mesh_part(part, actor.root().local_transform, light_view_projection);
    }
}

void Renderer::draw_depth_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& light_view_projection) {
    switch (part.primitive) {
    case MeshPrimitive::Cube:
        draw_depth_gpu_mesh(cube_mesh_, transform, light_view_projection);
        break;
    case MeshPrimitive::Plane:
        draw_depth_gpu_mesh(plane_mesh_, transform, light_view_projection);
        break;
    case MeshPrimitive::Sphere:
        draw_depth_gpu_mesh(sphere_mesh_, transform, light_view_projection);
        break;
    case MeshPrimitive::Custom: {
        auto it = custom_mesh_cache_.find(&part);
        if (it == custom_mesh_cache_.end()) {
            it = custom_mesh_cache_.emplace(&part, create_custom_mesh(part)).first;
        }
        draw_depth_gpu_mesh(it->second, transform, light_view_projection);
        break;
    }
    }
}

void Renderer::draw_depth_gpu_mesh(const GpuMesh& mesh, const Transform& transform, const Mat4& light_view_projection) const {
    if (depth_program_ == 0 || mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

    const Mat4 mvp = light_view_projection * transform.matrix();
    glx.use_program(depth_program_);
    glx.uniform_matrix_4fv(glx.get_uniform_location(depth_program_, "uMvp"), 1, kFalse, mvp.m.data());

    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(mesh.vertex_array);
    } else {
        glx.bind_buffer(kArrayBuffer, mesh.vertex_buffer);
        glx.enable_vertex_attrib_array(0);
        glx.vertex_attrib_pointer(0, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, position)));
    }
    glx.bind_buffer(kElementArrayBuffer, mesh.index_buffer);
    glDrawElements(kTriangles, mesh.index_count, kUnsignedInt, nullptr);

    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(0);
    } else {
        glx.disable_vertex_attrib_array(0);
    }
    glx.bind_buffer(kArrayBuffer, 0);
    glx.bind_buffer(kElementArrayBuffer, 0);
    glx.use_program(0);
}

void Renderer::draw_gpu_mesh_outline(const GpuMesh& mesh, const Transform& transform, const Mat4& view_projection, float outline_width) const {
    if (outline_program_ == 0 || mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

    const Mat4 model = transform.matrix();
    const Mat4 mvp = view_projection * model;

    glx.use_program(outline_program_);
    glx.uniform_matrix_4fv(glx.get_uniform_location(outline_program_, "uMvp"), 1, kFalse, mvp.m.data());
    glx.uniform_1f(glx.get_uniform_location(outline_program_, "uOutlineWidth"), outline_width);
    glx.uniform_3f(glx.get_uniform_location(outline_program_, "uOutlineColor"), 0.015f, 0.018f, 0.016f);

    glCullFace(GL_FRONT);
    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(mesh.vertex_array);
    } else {
        glx.bind_buffer(kArrayBuffer, mesh.vertex_buffer);
        glx.enable_vertex_attrib_array(0);
        glx.vertex_attrib_pointer(0, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, position)));
        glx.enable_vertex_attrib_array(1);
        glx.vertex_attrib_pointer(1, 3, kFloat, kFalse, sizeof(RenderVertex), reinterpret_cast<const void*>(offsetof(RenderVertex, normal)));
    }
    glx.bind_buffer(kElementArrayBuffer, mesh.index_buffer);
    glDrawElements(kTriangles, mesh.index_count, kUnsignedInt, nullptr);

    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(0);
    } else {
        glx.disable_vertex_attrib_array(0);
        glx.disable_vertex_attrib_array(1);
    }
    glx.bind_buffer(kArrayBuffer, 0);
    glx.bind_buffer(kElementArrayBuffer, 0);
    glCullFace(GL_BACK);
    glx.use_program(0);
}

GpuMesh Renderer::create_cube_mesh() const {
    return create_gpu_mesh(cube_vertices(), quad_face_indices());
}

GpuMesh Renderer::create_plane_mesh() const {
    std::vector<RenderVertex> vertices{
        {{-0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 1, 1}, {0, 1}, {1, 0, 0}, 1.0f},
        {{0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 1, 1}, {1, 1}, {1, 0, 0}, 1.0f},
        {{0.5f, 0.0f, 0.5f}, {0, 1, 0}, {1, 1, 1}, {1, 0}, {1, 0, 0}, 1.0f},
        {{-0.5f, 0.0f, 0.5f}, {0, 1, 0}, {1, 1, 1}, {0, 0}, {1, 0, 0}, 1.0f},
    };
    std::vector<std::uint32_t> indices{0, 1, 2, 0, 2, 3};
    return create_gpu_mesh(vertices, indices);
}

GpuMesh Renderer::create_sphere_mesh() const {
    constexpr int rings = 20;
    constexpr int segments = 32;
    std::vector<RenderVertex> vertices;
    std::vector<std::uint32_t> indices;
    for (int y = 0; y <= rings; ++y) {
        const float v = static_cast<float>(y) / rings;
        const float phi = (v - 0.5f) * kPi;
        for (int x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / segments;
            const float theta = u * kPi * 2.0f;
            Vec3 normal{std::cos(phi) * std::cos(theta), std::sin(phi), std::cos(phi) * std::sin(theta)};
            const Vec3 tangent = normalize(Vec3{-std::sin(theta), 0.0f, std::cos(theta)});
            vertices.push_back({normal * 0.5f, normal, {1, 1, 1}, {u, 1.0f - v}, tangent, 1.0f});
        }
    }
    for (int y = 0; y < rings; ++y) {
        for (int x = 0; x < segments; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(y * (segments + 1) + x);
            const std::uint32_t b = a + 1;
            const std::uint32_t c = static_cast<std::uint32_t>((y + 1) * (segments + 1) + x);
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    return create_gpu_mesh(vertices, indices);
}

void Renderer::draw_custom_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms) {
    auto it = custom_mesh_cache_.find(&part);
    if (it == custom_mesh_cache_.end()) {
        it = custom_mesh_cache_.emplace(&part, create_custom_mesh(part)).first;
    }
    draw_gpu_mesh(it->second, part.material, transform, view_projection, uniforms);
}

void Renderer::draw_actor_outline(const Actor& actor, const Mat4& view_projection) {
    const auto* mesh = actor.find_component<StaticMeshComponent>();
    if (mesh == nullptr || !mesh->visible) {
        return;
    }

    for (const auto& part : mesh->parts) {
        draw_mesh_part_outline(part, actor.root().local_transform, view_projection);
    }
}

void Renderer::draw_mesh_part_outline(const MeshPart& part, const Transform& transform, const Mat4& view_projection) {
    if (outline_program_ == 0 || part.primitive == MeshPrimitive::Plane) {
        return;
    }

    const Vec3 scale = transform.scale;
    const float largest_axis = std::max({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z), 0.001f});
    const float outline_width = 0.025f / largest_axis;

    switch (part.primitive) {
    case MeshPrimitive::Cube:
        draw_gpu_mesh_outline(cube_mesh_, transform, view_projection, outline_width);
        break;
    case MeshPrimitive::Sphere:
        draw_gpu_mesh_outline(sphere_mesh_, transform, view_projection, outline_width);
        break;
    case MeshPrimitive::Custom: {
        auto it = custom_mesh_cache_.find(&part);
        if (it == custom_mesh_cache_.end()) {
            it = custom_mesh_cache_.emplace(&part, create_custom_mesh(part)).first;
        }
        draw_gpu_mesh_outline(it->second, transform, view_projection, outline_width);
        break;
    }
    case MeshPrimitive::Plane:
        break;
    }
}

void Renderer::draw_actor(const Actor& actor, bool selected, const Mat4& view_projection, const ObjectUniforms& uniforms) {
    if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
        if (mesh->visible) {
            for (const auto& part : mesh->parts) {
                draw_mesh_part(part, actor.root().local_transform, view_projection, uniforms);
            }
        }
    }
    if (const auto* light = actor.find_component<LightComponent>()) {
        draw_light_icon(actor, *light, selected);
    }
}

void Renderer::draw_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms) {
    if (object_program_ == 0) {
        glPushMatrix();
        const Mat4 model = transform.matrix();
        glMultMatrixf(model.m.data());
        switch (part.primitive) {
        case MeshPrimitive::Cube:
            draw_cube(part.material);
            break;
        case MeshPrimitive::Plane:
            draw_plane();
            break;
        case MeshPrimitive::Sphere:
            draw_sphere();
            break;
        case MeshPrimitive::Custom:
            glBegin(GL_TRIANGLES);
            for (std::uint32_t index : part.indices) {
                if (index >= part.vertices.size()) {
                    continue;
                }
                const Vertex& v = part.vertices[index];
                set_color(shade(part.material, v.normal));
                vertex(v.position);
            }
            glEnd();
            break;
        }
        glPopMatrix();
        return;
    }

    switch (part.primitive) {
    case MeshPrimitive::Cube:
        draw_gpu_mesh(cube_mesh_, part.material, transform, view_projection, uniforms);
        break;
    case MeshPrimitive::Plane:
        draw_gpu_mesh(plane_mesh_, part.material, transform, view_projection, uniforms);
        break;
    case MeshPrimitive::Sphere:
        draw_gpu_mesh(sphere_mesh_, part.material, transform, view_projection, uniforms);
        break;
    case MeshPrimitive::Custom:
        draw_custom_mesh_part(part, transform, view_projection, uniforms);
        break;
    }
}

void Renderer::draw_cube(const Material& material) const {
    draw_quad({-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, shade(material, {0, 0, 1}));
    draw_quad({0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, shade(material, {0, 0, -1}));
    draw_quad({-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}, shade(material, {-1, 0, 0}));
    draw_quad({0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, shade(material, {1, 0, 0}));
    draw_quad({-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, shade(material, {0, 1, 0}));
    draw_quad({-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}, shade(material, {0, -1, 0}));
}

void Renderer::draw_plane() const {
    glDisable(GL_CULL_FACE);
    draw_quad({-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f}, {0.48f, 0.50f, 0.48f});
    glEnable(GL_CULL_FACE);
}

void Renderer::draw_sphere() const {
    constexpr int rings = 16;
    constexpr int segments = 24;
    glBegin(GL_TRIANGLES);
    for (int y = 0; y < rings; ++y) {
        const float v0 = static_cast<float>(y) / rings;
        const float v1 = static_cast<float>(y + 1) / rings;
        const float phi0 = (v0 - 0.5f) * kPi;
        const float phi1 = (v1 - 0.5f) * kPi;
        for (int x = 0; x < segments; ++x) {
            const float u0 = static_cast<float>(x) / segments;
            const float u1 = static_cast<float>(x + 1) / segments;
            const float theta0 = u0 * kPi * 2.0f;
            const float theta1 = u1 * kPi * 2.0f;
            const Vec3 p00{std::cos(phi0) * std::cos(theta0) * 0.5f, std::sin(phi0) * 0.5f, std::cos(phi0) * std::sin(theta0) * 0.5f};
            const Vec3 p10{std::cos(phi0) * std::cos(theta1) * 0.5f, std::sin(phi0) * 0.5f, std::cos(phi0) * std::sin(theta1) * 0.5f};
            const Vec3 p01{std::cos(phi1) * std::cos(theta0) * 0.5f, std::sin(phi1) * 0.5f, std::cos(phi1) * std::sin(theta0) * 0.5f};
            const Vec3 p11{std::cos(phi1) * std::cos(theta1) * 0.5f, std::sin(phi1) * 0.5f, std::cos(phi1) * std::sin(theta1) * 0.5f};
            set_color({0.55f, 0.49f, 0.30f});
            vertex(p00);
            vertex(p10);
            vertex(p11);
            vertex(p00);
            vertex(p11);
            vertex(p01);
        }
    }
    glEnd();
}

void Renderer::draw_light_icon(const Actor& actor, const LightComponent& light, bool selected) const {
    glPushMatrix();
    const Transform& transform = actor.root().local_transform;
    const Mat4 model = Mat4::translation(transform.translation) * Mat4::scale({0.18f, 0.18f, 0.18f});
    glMultMatrixf(model.m.data());
    set_color(selected ? Vec3{1.0f, 0.9f, 0.35f} : light.color);
    if (light.light_kind == LightKind::Directional) {
        Material marker;
        marker.base_color = light.color;
        draw_cube(marker);
        draw_line({0, 0, 0}, {0, -1.5f, 0}, light.color, 3.0f);
    } else {
        draw_sphere();
    }
    glPopMatrix();
}

void Renderer::draw_gizmo(const Actor& actor, EditorTool tool, TransformSpace space) const {
    const Vec3 origin = actor.root().local_transform.translation;
    const float scale = tool == EditorTool::Scale ? 1.15f : 1.35f;
    const bool scale_tool = tool == EditorTool::Scale;
    const Mat4 rotation = (space == TransformSpace::Local || scale_tool) ? Mat4::rotation_xyz(actor.root().local_transform.rotation) : Mat4::identity();
    auto axis = [&rotation](Vec3 a) {
        return normalize({
            a.x * rotation.m[0] + a.y * rotation.m[4] + a.z * rotation.m[8],
            a.x * rotation.m[1] + a.y * rotation.m[5] + a.z * rotation.m[9],
            a.x * rotation.m[2] + a.y * rotation.m[6] + a.z * rotation.m[10],
        });
    };
    const Vec3 x_axis = axis({1.0f, 0.0f, 0.0f});
    const Vec3 y_axis = axis({0.0f, 1.0f, 0.0f});
    const Vec3 z_axis = axis({0.0f, 0.0f, 1.0f});

    if (tool == EditorTool::Rotate) {
        glDisable(GL_CULL_FACE);
        glLineWidth(3.0f);
        constexpr int segments = 96;
        auto ring = [&](Vec3 a, Vec3 b, Vec3 color) {
            set_color(color);
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < segments; ++i) {
                const float t = static_cast<float>(i) / segments * kPi * 2.0f;
                vertex(origin + a * (std::cos(t) * scale) + b * (std::sin(t) * scale));
            }
            glEnd();
        };
        ring(y_axis, z_axis, {0.85f, 0.2f, 0.16f});
        ring(x_axis, z_axis, {0.25f, 0.68f, 0.31f});
        ring(x_axis, y_axis, {0.22f, 0.48f, 0.78f});
        glLineWidth(1.0f);
        glEnable(GL_CULL_FACE);
    } else {
        draw_line(origin, origin + x_axis * scale, {0.85f, 0.2f, 0.16f}, 5.0f);
        draw_line(origin, origin + y_axis * scale, {0.25f, 0.68f, 0.31f}, 5.0f);
        draw_line(origin, origin + z_axis * scale, {0.22f, 0.48f, 0.78f}, 5.0f);

        const Vec3 endpoints[] = {origin + x_axis * scale, origin + y_axis * scale, origin + z_axis * scale};
        const Vec3 colors[] = {{0.85f, 0.2f, 0.16f}, {0.25f, 0.68f, 0.31f}, {0.22f, 0.48f, 0.78f}};
        for (int i = 0; i < 3; ++i) {
            glPushMatrix();
            glTranslatef(endpoints[i].x, endpoints[i].y, endpoints[i].z);
            glScalef(0.16f, 0.16f, 0.16f);
            Material handle;
            handle.base_color = colors[i];
            draw_cube(handle);
            glPopMatrix();
        }
    }

    glPushMatrix();
    glTranslatef(origin.x, origin.y, origin.z);
    glScalef(0.14f, 0.14f, 0.14f);
    Material center;
    center.base_color = {0.72f, 0.66f, 0.28f};
    draw_cube(center);
    glPopMatrix();
}

} // namespace Forge

// Renderer implementation: the engine's OpenGL PBR forward-plus-ish render
// pipeline.
//
// This is the largest and most central module in the engine. Each frame,
// App calls Renderer::render(Scene, Camera, Engine) once, and this file
// drives the whole GPU-side pipeline for that call:
//   1. Shadow maps      - cascaded directional (CSM), point (cube), spot.
//   2. SSAO              - screen-space ambient occlusion from a normal/depth
//                           prepass.
//   3. IBL / environment - diffuse irradiance + prefiltered specular
//                           convolution of a loaded HDR environment map
//                           (baked on load_environment, not every frame).
//   4. Main object pass   - opaque and transparent PBR-shaded geometry.
//   5. Outlines / grid / gizmos - editor-only selection outlines, ground
//                           grid, and transform gizmos.
//   6. Post-processing    - HDR tonemap, bloom, vignette, color grading, FXAA.
//
// Render targets are managed through the named registry declared in
// Renderer.hpp (acquire_render_target) rather than one-off framebuffer
// members. Scene supplies the actors/lights to draw; Engine supplies
// frame-global settings (RenderPipelineSettings); EditorUi reads back
// describe_passes()/render_target_info() for its debug "Render Graph" panel.
// GLSL shader sources live inline as raw string literals throughout this
// file (kept under MSVC's raw-string length limit; see individual pass
// setup functions).
#include "Hollow/Renderer.hpp"
#include "Hollow/HdrLoader.hpp"
#include "Hollow/TextureLoader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <gl/GL.h>

namespace Hollow {
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
constexpr unsigned int kTexture10 = 0x84CA;
constexpr unsigned int kTexture11 = 0x84CB;
constexpr unsigned int kTexture12 = 0x84CC;
constexpr unsigned int kTexture13 = 0x84CD;
constexpr unsigned int kTexture14 = 0x84CE;
constexpr unsigned int kTexture15 = 0x84CF;
constexpr unsigned int kTexture16 = 0x84D0;
constexpr unsigned int kTexture17 = 0x84D1;
constexpr unsigned int kTexture18 = 0x84D2;
constexpr unsigned int kTexture19 = 0x84D3;
constexpr unsigned int kDepthComponent = 0x1902;
constexpr unsigned int kRgb = 0x1907;
constexpr unsigned int kRgba16f = 0x881A;
constexpr unsigned int kNearest = 0x2600;
constexpr unsigned int kLinearMipmapLinear = 0x2703;
constexpr unsigned int kClampToEdge = 0x812F;
constexpr unsigned int kTextureWrapR = 0x8072;
constexpr unsigned int kTextureCubeMap = 0x8513;
constexpr unsigned int kTextureCubeMapPositiveX = 0x8515;
constexpr unsigned int kTextureMaxLevel = 0x813D;
constexpr unsigned int kFramebuffer = 0x8D40;
constexpr unsigned int kColorAttachment0 = 0x8CE0;
constexpr unsigned int kDepthAttachment = 0x8D00;
constexpr unsigned int kFramebufferComplete = 0x8CD5;
constexpr unsigned int kNone = 0;
constexpr unsigned int kBack = 0x0405;
// Core in GL 3.2+, and available as GL_ARB_seamless_cube_map on every GPU
// this engine targets even under an older context: without it, textureCube
// lookups near a cube face's edge sample only that face (GL_CLAMP_TO_EDGE
// stops at the face boundary instead of blending across it), which shows up
// as a visible seam/blocky discontinuity in specular IBL reflections on
// curved surfaces, worse the more contrast the environment has near that
// seam. No function pointer needed — it's a plain capability enum, not an
// entry point, so a bare glEnable/glIsEnabled call is enough.
constexpr unsigned int kTextureCubeMapSeamless = 0x884F;

// Number of mip levels in the bloom downsample/upsample chain (see
// Renderer::execute_bloom_pass). Mip 0 is half the viewport's resolution;
// each subsequent mip halves again, so 5 levels reaches roughly 1/32 res —
// enough to catch a wide, soft glow without the chain getting so deep that
// the smallest mips are just a handful of texels.
constexpr int kBloomMipCount = 5;

constexpr int kCascadeShadowMapResolution = 1536;
constexpr int kSpotShadowMapResolution = 1024;

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
using GlUniform4f = void(APIENTRY*)(int location, float v0, float v1, float v2, float v3);
using GlUniform2f = void(APIENTRY*)(int location, float v0, float v1);
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
using GlGenerateMipmap = void(APIENTRY*)(unsigned int target);

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
    GlUniform4f uniform_4f = nullptr;
    GlUniform2f uniform_2f = nullptr;
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
    GlGenerateMipmap generate_mipmap = nullptr;

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
            && uniform_4f != nullptr
            && uniform_2f != nullptr
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
            && delete_framebuffers != nullptr
            && generate_mipmap != nullptr;
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
    glx.uniform_4f = load_gl_function<GlUniform4f>("glUniform4f");
    glx.uniform_2f = load_gl_function<GlUniform2f>("glUniform2f");
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
    glx.generate_mipmap = load_gl_function<GlGenerateMipmap>("glGenerateMipmap");
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

// The six view*projection matrices that turn a unit cube centered at
// `position` into the six faces of a GL_TEXTURE_CUBE_MAP, in the standard
// +X,-X,+Y,-Y,+Z,-Z face order (matching GL_TEXTURE_CUBE_MAP_POSITIVE_X + i).
// Same direction/up-vector convention as the existing point-light shadow
// cube in collect_object_uniforms, which is the convention already proven
// correct (matching image orientation, no mirroring) in this engine.
std::array<Mat4, 6> cube_face_view_projections(Vec3 position, float near_plane, float far_plane) {
    const Mat4 projection = Mat4::perspective(1.57079632679f, 1.0f, near_plane, far_plane);
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
    std::array<Mat4, 6> result;
    for (std::size_t i = 0; i < 6; ++i) {
        result[i] = projection * Mat4::look_at(position, position + directions[i], up_vectors[i]);
    }
    return result;
}

// The direction a light travels, derived from its actor rotation: the
// light's local -Y axis rotated into world space. Directional lights shine
// toward -direction; spot lights use this as their cone axis.
Vec3 light_forward_direction(Vec3 rotation_euler) {
    const Mat4 rotation = Mat4::rotation_xyz(rotation_euler);
    return normalize(Vec3{-rotation.m[4], -rotation.m[5], -rotation.m[6]});
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

// A small solid cone used for the Move gizmo's arrowheads (Renderer::draw_gizmo)
// — base circle at `base_center` with `radius`, apex at `base_center +
// axis_dir * height`. `axis_dir` must be a unit vector; it's arbitrary
// (not restricted to the principal axes) since the gizmo's arrows rotate
// with the actor in local transform space. Each side triangle is flat-shaded
// via the same `shade()` fake-directional-light helper the rest of the
// immediate-mode editor geometry (draw_cube, draw_sphere) uses, using the
// standard cone lateral-surface normal formula (radial * height + axis *
// radius, i.e. the slant-line direction rotated 90 degrees toward the
// axis) rather than a per-triangle cross product, so it doesn't depend on
// getting triangle winding exactly right. Callers should draw with face
// culling disabled for the same reason — with only ~14 segments and no
// culling the extra overdraw is free, and it sidesteps any winding-order
// mistake silently culling the whole cone.
void draw_cone(Vec3 base_center, Vec3 axis_dir, float radius, float height, const Material& material) {
    const Vec3 apex = base_center + axis_dir * height;
    const Vec3 helper = std::abs(axis_dir.y) > 0.99f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 tangent = normalize(cross(helper, axis_dir));
    const Vec3 bitangent = normalize(cross(axis_dir, tangent));

    constexpr int segments = 14;
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < segments; ++i) {
        const float t0 = static_cast<float>(i) / segments * kPi * 2.0f;
        const float t1 = static_cast<float>(i + 1) / segments * kPi * 2.0f;
        const Vec3 radial0 = tangent * std::cos(t0) + bitangent * std::sin(t0);
        const Vec3 radial1 = tangent * std::cos(t1) + bitangent * std::sin(t1);
        const Vec3 normal0 = normalize(radial0 * height + axis_dir * radius);
        const Vec3 normal1 = normalize(radial1 * height + axis_dir * radius);
        set_color(shade(material, normalize(normal0 + normal1)));
        vertex(apex);
        vertex(base_center + radial0 * radius);
        vertex(base_center + radial1 * radius);
    }
    glEnd();

    // Base cap, so the cone doesn't look hollow from a shallow viewing
    // angle where the shaft doesn't fully hide the opening.
    glBegin(GL_TRIANGLE_FAN);
    set_color(shade(material, axis_dir * -1.0f));
    vertex(base_center);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i % segments) / segments * kPi * 2.0f;
        const Vec3 radial = tangent * std::cos(t) + bitangent * std::sin(t);
        vertex(base_center + radial * radius);
    }
    glEnd();
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
    glEnable(kTextureCubeMapSeamless);
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
    if (bloom_threshold_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(bloom_threshold_program_);
        bloom_threshold_program_ = 0;
    }
    if (bloom_downsample_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(bloom_downsample_program_);
        bloom_downsample_program_ = 0;
    }
    if (bloom_upsample_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(bloom_upsample_program_);
        bloom_upsample_program_ = 0;
    }
    if (normal_depth_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(normal_depth_program_);
        normal_depth_program_ = 0;
    }
    if (ssao_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(ssao_program_);
        ssao_program_ = 0;
    }
    if (ssao_blur_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(ssao_blur_program_);
        ssao_blur_program_ = 0;
    }
    if (fxaa_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(fxaa_program_);
        fxaa_program_ = 0;
    }
    if (skybox_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(skybox_program_);
        skybox_program_ = 0;
    }
    destroy_all_render_targets();
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
    destroy_ibl_resources();
    if (ibl_brdf_lut_ != 0) {
        glDeleteTextures(1, &ibl_brdf_lut_);
        ibl_brdf_lut_ = 0;
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
    RenderFrameContext frame = begin_frame(scene, camera, engine);
    // ActorOutlines runs before EditorGrid (not the other way around, and not
    // after OpaqueGeometry) for two reasons: the outline shell technique
    // needs to draw before the real opaque geometry so the object's own
    // front faces paint over the inflated hull and only its silhouette peeks
    // out (see draw_gpu_mesh_outline); and running it before the grid means
    // the grid — which shares the same depth buffer and, near an object's
    // base, sits almost exactly coplanar with the outline shell — respects
    // the outline's already-written depth instead of winning ties against it
    // and visibly cutting through the outline right where objects meet the
    // ground plane.
    constexpr std::array<RenderPassKind, 11> pass_order{
        RenderPassKind::ShadowMaps,
        RenderPassKind::SceneColor,
        RenderPassKind::AmbientOcclusion,
        RenderPassKind::ActorOutlines,
        RenderPassKind::EditorGrid,
        RenderPassKind::OpaqueGeometry,
        RenderPassKind::TransparentGeometry,
        RenderPassKind::Bloom,
        RenderPassKind::Tonemap,
        RenderPassKind::Fxaa,
        RenderPassKind::EditorOverlay,
    };

    for (const RenderPassKind pass : pass_order) {
        switch (pass) {
        case RenderPassKind::ShadowMaps:
            execute_shadow_passes(frame);
            break;
        case RenderPassKind::SceneColor:
            begin_scene_color_pass(frame);
            break;
        case RenderPassKind::AmbientOcclusion:
            execute_ambient_occlusion_pass(frame);
            break;
        case RenderPassKind::EditorGrid:
            execute_editor_grid_pass(frame);
            break;
        case RenderPassKind::ActorOutlines:
            execute_outline_pass(frame);
            break;
        case RenderPassKind::OpaqueGeometry:
            execute_opaque_geometry_pass(frame);
            break;
        case RenderPassKind::TransparentGeometry:
            execute_transparent_geometry_pass(frame);
            break;
        case RenderPassKind::Bloom:
            execute_bloom_pass(frame);
            break;
        case RenderPassKind::Tonemap:
            end_scene_color_pass(frame);
            break;
        case RenderPassKind::Fxaa:
            execute_fxaa_pass(frame);
            break;
        case RenderPassKind::EditorOverlay:
            execute_editor_overlay_pass(frame);
            break;
        }
    }
}

RenderFrameContext Renderer::begin_frame(const Scene& scene, const Camera& camera, const Engine& engine) {
    sync_custom_mesh_cache(scene);
    resize(width_, height_);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 projection = Mat4::perspective(camera.fov_y_radians, aspect, 0.05f, 500.0f);
    const Mat4 view = Mat4::look_at(camera.position(), camera.target, {0.0f, 1.0f, 0.0f});
    ObjectUniforms uniforms = collect_object_uniforms(scene, camera, engine);
    uniforms.hdr_output_enabled = engine.render_settings().hdr_enabled && tonemap_program_ != 0;

    return {
        scene,
        camera,
        engine,
        projection,
        view,
        projection * view,
        uniforms,
        uniforms.hdr_output_enabled,
    };
}

void Renderer::execute_shadow_passes(const RenderFrameContext& frame) {
    if (frame.object_uniforms.shadows_enabled) {
        render_cascaded_shadow_maps(frame.scene, frame.object_uniforms);
    }
    if (frame.object_uniforms.spot_shadows_enabled) {
        render_spot_shadow_map(frame.scene, frame.object_uniforms);
    }
    if (frame.object_uniforms.point_shadows_enabled) {
        render_point_shadow_maps(frame.scene, frame.object_uniforms);
    }
}

void Renderer::begin_scene_color_pass(const RenderFrameContext& frame) {
    if (frame.use_hdr) {
        RenderTargetDesc scene_color_desc;
        scene_color_desc.name = "SceneColor";
        scene_color_desc.size = TargetSize::Viewport;
        scene_color_desc.color_format = TargetColorFormat::Rgba16f;
        scene_color_desc.depth_format = TargetDepthFormat::Depth24;
        const RenderTarget& scene_color = acquire_render_target(scene_color_desc);
        glx.bind_framebuffer(kFramebuffer, scene_color.framebuffer);
    } else if (glx.ready()) {
        glx.bind_framebuffer(kFramebuffer, 0);
        glDrawBuffer(kBack);
        glReadBuffer(kBack);
    }

    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    restore_camera_matrices(frame);
    if (frame.engine.render_settings().ibl_enabled) {
        draw_skybox(frame);
    }
}

// SSAO: a normal+depth prepass into its own framebuffer (see
// normal_depth_vertex_shader's comment, above in initialize(), for why this
// can't just be folded into the main opaque pass), a half-resolution
// hemisphere-kernel AO compute pass, and a 3x3 blur, in that order. The
// blurred result ("SceneAO") is sampled back in draw_gpu_mesh to darken the
// ambient/IBL term, the same place the glTF occlusion texture already
// applies. This whole pass runs *before* opaque/transparent geometry
// (unlike bloom, which runs after them reading the finished HDR image) —
// its entire purpose is to have "SceneAO" ready by the time the opaque
// pass's lighting wants to sample it.
void Renderer::execute_ambient_occlusion_pass(const RenderFrameContext& frame) {
    if (!frame.engine.render_settings().ao_enabled) {
        return;
    }
    if (normal_depth_program_ == 0 || ssao_program_ == 0 || ssao_blur_program_ == 0) {
        return;
    }

    render_normal_depth_prepass(frame.scene, frame.view_projection, frame.camera);
    const RenderTarget* normal_depth_target = find_render_target("SceneNormalDepth");
    if (normal_depth_target == nullptr || normal_depth_target->color_texture == 0) {
        return;
    }

    RenderTargetDesc ao_desc;
    ao_desc.size = TargetSize::Fixed;
    ao_desc.fixed_width = std::max(1, width_ / 2);
    ao_desc.fixed_height = std::max(1, height_ / 2);
    ao_desc.color_format = TargetColorFormat::Rgba8;
    ao_desc.depth_format = TargetDepthFormat::None;
    ao_desc.linear_filter = true;
    ao_desc.clamp_to_edge = true;
    ao_desc.name = "SceneAORaw";
    RenderTarget& ao_raw = acquire_render_target(ao_desc);
    ao_desc.name = "SceneAO";
    RenderTarget& ao_final = acquire_render_target(ao_desc);

    const RenderPipelineSettings& settings = frame.engine.render_settings();
    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    const float tan_half_fov_y = std::tan(frame.camera.fov_y_radians * 0.5f);
    const Vec3 camera_position = frame.camera.position();
    const Vec3 camera_forward = frame.camera.forward();
    const Vec3 camera_right = frame.camera.right();
    const Vec3 camera_up = frame.camera.up();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // 1) Raw hemisphere-kernel AO compute: "SceneNormalDepth" -> "SceneAORaw".
    glx.bind_framebuffer(kFramebuffer, ao_raw.framebuffer);
    glViewport(0, 0, ao_raw.width, ao_raw.height);
    glx.use_program(ssao_program_);
    glx.uniform_1i(glx.get_uniform_location(ssao_program_, "uNormalDepth"), 0);
    glx.uniform_3f(glx.get_uniform_location(ssao_program_, "uCameraPosition"), camera_position.x, camera_position.y, camera_position.z);
    glx.uniform_3f(glx.get_uniform_location(ssao_program_, "uCameraForward"), camera_forward.x, camera_forward.y, camera_forward.z);
    glx.uniform_3f(glx.get_uniform_location(ssao_program_, "uCameraRight"), camera_right.x, camera_right.y, camera_right.z);
    glx.uniform_3f(glx.get_uniform_location(ssao_program_, "uCameraUp"), camera_up.x, camera_up.y, camera_up.z);
    glx.uniform_1f(glx.get_uniform_location(ssao_program_, "uTanHalfFovY"), tan_half_fov_y);
    glx.uniform_1f(glx.get_uniform_location(ssao_program_, "uAspect"), aspect);
    glx.uniform_1f(glx.get_uniform_location(ssao_program_, "uRadius"), std::max(0.01f, settings.ao_radius));
    glx.uniform_1f(glx.get_uniform_location(ssao_program_, "uBias"), settings.ao_bias);
    glx.uniform_1f(glx.get_uniform_location(ssao_program_, "uIntensity"), settings.ao_intensity);
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, normal_depth_target->color_texture);
    draw_fullscreen_quad();

    // 2) Blur: "SceneAORaw" -> "SceneAO".
    glx.bind_framebuffer(kFramebuffer, ao_final.framebuffer);
    glViewport(0, 0, ao_final.width, ao_final.height);
    glx.use_program(ssao_blur_program_);
    glx.uniform_1i(glx.get_uniform_location(ssao_blur_program_, "uAoRaw"), 0);
    glx.uniform_2f(glx.get_uniform_location(ssao_blur_program_, "uTexelSize"), 1.0f / static_cast<float>(ao_raw.width), 1.0f / static_cast<float>(ao_raw.height));
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, ao_raw.color_texture);
    draw_fullscreen_quad();

    glBindTexture(kTexture2D, 0);
    glx.use_program(0);

    // Restore exactly the state begin_scene_color_pass (the previous pass)
    // left behind — SceneColor bound and the full viewport with HDR on, the
    // backbuffer directly with it off — since EditorGrid and every pass
    // after it assumes that state is still current rather than rebinding
    // it themselves.
    if (frame.use_hdr) {
        const RenderTarget* scene_color = find_render_target("SceneColor");
        if (scene_color != nullptr) {
            glx.bind_framebuffer(kFramebuffer, scene_color->framebuffer);
        }
    } else {
        glx.bind_framebuffer(kFramebuffer, 0);
        glDrawBuffer(kBack);
        glReadBuffer(kBack);
    }
    glViewport(0, 0, width_, height_);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::render_normal_depth_prepass(const Scene& scene, const Mat4& view_projection, const Camera& camera) {
    RenderTargetDesc desc;
    desc.name = "SceneNormalDepth";
    desc.size = TargetSize::Viewport;
    desc.color_format = TargetColorFormat::Rgba16f;
    desc.depth_format = TargetDepthFormat::Depth24;
    // Point-sampled, not filtered: bilinear-interpolating between two
    // unrelated surfaces' normals/depths at an edge produces meaningless
    // values, worse than just picking one or the other.
    desc.linear_filter = false;
    desc.clamp_to_edge = true;
    const RenderTarget& target = acquire_render_target(desc);

    glx.bind_framebuffer(kFramebuffer, target.framebuffer);
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    // Clear alpha (the packed view depth) to exactly 0 so the SSAO shader's
    // "no geometry here" check (`depth <= 0.0001`) is reliable in
    // background regions. glClearColor is otherwise fixed for the whole
    // renderer (set once in initialize()), so this saves and restores it
    // rather than leaving every later glClear(GL_COLOR_BUFFER_BIT) this
    // frame and every frame after silently clearing to black.
    float previous_clear_color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previous_clear_color);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(previous_clear_color[0], previous_clear_color[1], previous_clear_color[2], previous_clear_color[3]);

    glx.use_program(normal_depth_program_);
    const Vec3 camera_position = camera.position();
    const Vec3 camera_forward = camera.forward();
    glx.uniform_3f(glx.get_uniform_location(normal_depth_program_, "uCameraPosition"), camera_position.x, camera_position.y, camera_position.z);
    glx.uniform_3f(glx.get_uniform_location(normal_depth_program_, "uCameraForward"), camera_forward.x, camera_forward.y, camera_forward.z);

    for (const auto& actor : scene.actors()) {
        draw_normal_depth_actor(*actor, view_projection);
    }

    glx.use_program(0);
    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
}

void Renderer::execute_editor_grid_pass(const RenderFrameContext& frame) const {
    if (frame.engine.render_settings().draw_world_grid) {
        draw_grid(frame.camera);
    }
}

void Renderer::execute_outline_pass(const RenderFrameContext& frame) {
    if (!frame.engine.render_settings().draw_actor_outlines || outline_program_ == 0) {
        return;
    }

    for (const auto& actor : frame.scene.actors()) {
        draw_actor_outline(*actor, frame.view_projection);
    }
}

void Renderer::execute_opaque_geometry_pass(const RenderFrameContext& frame) {
    for (const auto& actor : frame.scene.actors()) {
        draw_actor(*actor, actor->id() == frame.scene.selected_actor_id(), frame.view_projection, frame.object_uniforms);
    }
}

// Draws AlphaMode::Blend mesh parts only, back-to-front sorted by distance
// from the camera so overlapping translucent surfaces composite correctly.
// Runs after the opaque pass (so it reads the depth buffer opaque geometry
// already wrote) with depth testing on but depth writes off, matching the
// standard forward-transparency approach: blended surfaces don't occlude
// each other in the depth buffer, but are still occluded by opaque geometry
// in front of them.
void Renderer::execute_transparent_geometry_pass(const RenderFrameContext& frame) {
    // Every mesh part on an actor shares the actor's single transform, so
    // sorting by actor position is equivalent to sorting by part position
    // here; there is no case where two parts on the same actor need to be
    // ordered relative to each other.
    struct BlendActor {
        const Actor* actor = nullptr;
        float distance_sq = 0.0f;
    };

    std::vector<BlendActor> draws;
    for (const auto& actor : frame.scene.actors()) {
        const auto* mesh = actor->find_component<StaticMeshComponent>();
        if (mesh == nullptr || !mesh->visible) {
            continue;
        }
        const bool has_blend_part = std::any_of(mesh->parts.begin(), mesh->parts.end(), [](const MeshPart& part) {
            return part.material.alpha_mode == AlphaMode::Blend;
        });
        if (!has_blend_part) {
            continue;
        }
        const Vec3 delta = actor->root().local_transform.translation - frame.camera.position();
        draws.push_back({actor.get(), delta.x * delta.x + delta.y * delta.y + delta.z * delta.z});
    }

    if (draws.empty()) {
        return;
    }

    std::sort(draws.begin(), draws.end(), [](const BlendActor& a, const BlendActor& b) {
        return a.distance_sq > b.distance_sq; // farthest first (back-to-front)
    });

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    for (const BlendActor& entry : draws) {
        draw_actor(*entry.actor, entry.actor->id() == frame.scene.selected_actor_id(), frame.view_projection, frame.object_uniforms, true);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// Bloom: thresholds SceneColor into BloomMip0, walks a downsample chain out
// to BloomMip4, then walks an additive tent-filter upsample chain back down
// to BloomMip0 — see the shader source comments above (in initialize()) for
// why each step looks the way it does. The result (BloomMip0, containing
// every mip's contribution combined) is read back in render_tonemapped_frame.
// No-ops with HDR off (there's no SceneColor target to threshold) or with
// bloom disabled in RenderPipelineSettings.
void Renderer::execute_bloom_pass(const RenderFrameContext& frame) {
    if (!frame.use_hdr || !frame.engine.render_settings().bloom_enabled) {
        return;
    }
    if (bloom_threshold_program_ == 0 || bloom_downsample_program_ == 0 || bloom_upsample_program_ == 0) {
        return;
    }
    const RenderTarget* scene_color = find_render_target("SceneColor");
    if (scene_color == nullptr || scene_color->color_texture == 0) {
        return;
    }

    std::array<RenderTarget*, kBloomMipCount> mips{};
    int mip_width = std::max(1, width_ / 2);
    int mip_height = std::max(1, height_ / 2);
    for (int i = 0; i < kBloomMipCount; ++i) {
        RenderTargetDesc desc;
        desc.name = "BloomMip" + std::to_string(i);
        desc.size = TargetSize::Fixed;
        desc.fixed_width = mip_width;
        desc.fixed_height = mip_height;
        desc.color_format = TargetColorFormat::Rgba16f;
        desc.depth_format = TargetDepthFormat::None;
        desc.linear_filter = true;
        desc.clamp_to_edge = true;
        mips[static_cast<std::size_t>(i)] = &acquire_render_target(desc);
        mip_width = std::max(1, mip_width / 2);
        mip_height = std::max(1, mip_height / 2);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // 1) Bright-pass threshold extract: SceneColor -> BloomMip0.
    RenderTarget& mip0 = *mips[0];
    glx.bind_framebuffer(kFramebuffer, mip0.framebuffer);
    glViewport(0, 0, mip0.width, mip0.height);
    glx.use_program(bloom_threshold_program_);
    glx.uniform_1i(glx.get_uniform_location(bloom_threshold_program_, "uSceneColor"), 0);
    glx.uniform_1f(glx.get_uniform_location(bloom_threshold_program_, "uThreshold"), std::max(0.0f, frame.engine.render_settings().bloom_threshold));
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, scene_color->color_texture);
    draw_fullscreen_quad();

    // 2) Downsample chain: mip[i] -> mip[i + 1].
    glx.use_program(bloom_downsample_program_);
    const int downsample_source_location = glx.get_uniform_location(bloom_downsample_program_, "uSource");
    const int downsample_texel_location = glx.get_uniform_location(bloom_downsample_program_, "uTexelSize");
    for (int i = 0; i + 1 < kBloomMipCount; ++i) {
        RenderTarget& src = *mips[static_cast<std::size_t>(i)];
        RenderTarget& dst = *mips[static_cast<std::size_t>(i + 1)];
        glx.bind_framebuffer(kFramebuffer, dst.framebuffer);
        glViewport(0, 0, dst.width, dst.height);
        glx.uniform_1i(downsample_source_location, 0);
        glx.uniform_2f(downsample_texel_location, 1.0f / static_cast<float>(src.width), 1.0f / static_cast<float>(src.height));
        glx.active_texture(kTexture0);
        glBindTexture(kTexture2D, src.color_texture);
        draw_fullscreen_quad();
    }

    // 3) Upsample + additive combine chain: mip[i + 1] -> mip[i], from the
    // smallest mip back down to mip0.
    glx.use_program(bloom_upsample_program_);
    const int upsample_source_location = glx.get_uniform_location(bloom_upsample_program_, "uSource");
    const int upsample_texel_location = glx.get_uniform_location(bloom_upsample_program_, "uTexelSize");
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = kBloomMipCount - 2; i >= 0; --i) {
        RenderTarget& src = *mips[static_cast<std::size_t>(i + 1)];
        RenderTarget& dst = *mips[static_cast<std::size_t>(i)];
        glx.bind_framebuffer(kFramebuffer, dst.framebuffer);
        glViewport(0, 0, dst.width, dst.height);
        glx.uniform_1i(upsample_source_location, 0);
        glx.uniform_2f(upsample_texel_location, 1.0f / static_cast<float>(src.width), 1.0f / static_cast<float>(src.height));
        glx.active_texture(kTexture0);
        glBindTexture(kTexture2D, src.color_texture);
        draw_fullscreen_quad();
    }
    glDisable(GL_BLEND);

    glBindTexture(kTexture2D, 0);
    glx.use_program(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::end_scene_color_pass(const RenderFrameContext& frame) {
    if (!frame.use_hdr) {
        return;
    }

    // With FXAA on, tonemap writes into an intermediate LDR target instead
    // of straight to the backbuffer — the next pass (execute_fxaa_pass)
    // reads it, applies antialiasing, and writes the actual backbuffer
    // itself, including the depth-clear/camera-matrix restore this
    // function would otherwise do here. With FXAA off, nothing changes
    // from before: tonemap writes the backbuffer directly.
    const bool apply_fxaa = frame.engine.render_settings().fxaa_enabled && fxaa_program_ != 0;
    if (apply_fxaa) {
        RenderTargetDesc ldr_desc;
        ldr_desc.name = "SceneColorLDR";
        ldr_desc.size = TargetSize::Viewport;
        ldr_desc.color_format = TargetColorFormat::Rgba8;
        ldr_desc.depth_format = TargetDepthFormat::None;
        const RenderTarget& ldr_target = acquire_render_target(ldr_desc);
        glx.bind_framebuffer(kFramebuffer, ldr_target.framebuffer);
        glViewport(0, 0, ldr_target.width, ldr_target.height);
        glClear(GL_COLOR_BUFFER_BIT);
        render_tonemapped_frame(frame.engine.render_settings(), frame.object_uniforms.debug_view);
        return;
    }

    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    render_tonemapped_frame(frame.engine.render_settings(), frame.object_uniforms.debug_view);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
    glClear(GL_DEPTH_BUFFER_BIT);
    restore_camera_matrices(frame);
}

// Reads "SceneColorLDR" (the tonemapped image end_scene_color_pass wrote
// there instead of the backbuffer, only when FXAA is enabled) and writes
// the antialiased result to the actual backbuffer, then does the same
// depth-clear/camera-matrix restore end_scene_color_pass's non-FXAA path
// does directly, since the editor overlay pass right after this one needs
// that state. No-ops (leaving the backbuffer untouched) with HDR off,
// FXAA disabled, or before the FXAA program has been created.
void Renderer::execute_fxaa_pass(const RenderFrameContext& frame) {
    if (!frame.use_hdr || !frame.engine.render_settings().fxaa_enabled || fxaa_program_ == 0) {
        return;
    }
    const RenderTarget* ldr_target = find_render_target("SceneColorLDR");
    if (ldr_target == nullptr || ldr_target->color_texture == 0) {
        return;
    }

    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glx.use_program(fxaa_program_);
    glx.uniform_1i(glx.get_uniform_location(fxaa_program_, "uSceneColor"), 0);
    glx.uniform_2f(glx.get_uniform_location(fxaa_program_, "uTexelSize"), 1.0f / static_cast<float>(std::max(1, width_)), 1.0f / static_cast<float>(std::max(1, height_)));
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, ldr_target->color_texture);
    draw_fullscreen_quad();
    glBindTexture(kTexture2D, 0);
    glx.use_program(0);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);
    restore_camera_matrices(frame);
}

void Renderer::execute_editor_overlay_pass(const RenderFrameContext& frame) {
    const Actor* selected = frame.scene.selected_actor();
    if (selected == nullptr || !frame.engine.render_settings().draw_editor_gizmos) {
        return;
    }

    glClear(GL_DEPTH_BUFFER_BIT);
    draw_gizmo(*selected, frame.engine.active_tool(), frame.engine.transform_space());
}

void Renderer::restore_camera_matrices(const RenderFrameContext& frame) const {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(frame.projection.m.data());
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(frame.view.m.data());
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
varying vec3 vWorldPosition;
varying vec3 vNormal;
varying vec3 vColor;
varying vec2 vUv;
varying vec3 vTangent;
varying float vTangentSign;
void main() {
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vColor = aColor;
    vUv = aUv;
    vTangent = normalize(mat3(uModel) * aTangent);
    vTangentSign = aTangentSign;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    constexpr const char* fragment_shader = R"(
#version 120
#extension GL_ARB_shader_texture_lod : enable
uniform vec3 uBaseColor;
uniform float uBaseColorAlpha;
uniform float uRoughness;
uniform float uMetallic;
uniform float uNormalStrength;
uniform vec3 uEmissiveFactor;
uniform float uOcclusionStrength;
uniform int uAlphaMode; // 0 = opaque, 1 = mask, 2 = blend
uniform float uAlphaCutoff;
uniform vec3 uCameraPosition;
uniform vec3 uCameraForward;
uniform vec3 uPointPosition;
uniform int uDebugView;

#define MAX_LIGHTS 16
struct Light {
    int type; // 0 = directional, 1 = point, 2 = spot
    vec3 position;
    vec3 direction;
    vec3 color;
    float intensity;
    float range;
    float innerCos;
    float outerCos;
    int castsShadow;
};
uniform int uLightCount;
uniform Light uLights[MAX_LIGHTS];
uniform sampler2D uBaseColorTexture;
uniform int uHasBaseColorTexture;
uniform sampler2D uMetallicRoughnessTexture;
uniform int uHasMetallicRoughnessTexture;
uniform sampler2D uNormalTexture;
uniform int uHasNormalTexture;
uniform sampler2D uEmissiveTexture;
uniform int uHasEmissiveTexture;
uniform sampler2D uOcclusionTexture;
uniform int uHasOcclusionTexture;
uniform sampler2D uSsaoTexture;
uniform int uSsaoEnabled;
uniform vec2 uViewportSize;
uniform sampler2D uCascadeShadowMap0;
uniform sampler2D uCascadeShadowMap1;
uniform sampler2D uCascadeShadowMap2;
uniform sampler2D uCascadeShadowMap3;
uniform mat4 uCascadeViewProjection0;
uniform mat4 uCascadeViewProjection1;
uniform mat4 uCascadeViewProjection2;
uniform mat4 uCascadeViewProjection3;
uniform vec4 uCascadeSplitDepths;
uniform int uShadowsEnabled;
uniform sampler2D uSpotShadowMap;
uniform mat4 uSpotLightViewProjection;
uniform int uSpotShadowsEnabled;
uniform int uHdrOutputEnabled;
uniform samplerCube uIrradianceMap;
uniform samplerCube uPrefilteredMap;
uniform sampler2D uBrdfLut;
uniform int uIblEnabled;
uniform float uPrefilteredMipCount;
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

const float PI = 3.14159265359;

// glTF/Sketchfab-style color textures (base color, emissive) are authored
// and stored sRGB-encoded; decoding them before lighting math keeps
// midtones correct instead of shading with gamma-compressed values as if
// they were already linear. Data textures (normal, metallic/roughness,
// occlusion) are never encoded this way and are sampled as-is.
vec3 srgb_to_linear(vec3 srgb) {
    return pow(clamp(srgb, 0.0, 1.0), vec3(2.2));
}

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

// Roughness-aware Fresnel used for the ambient/IBL term (Sebastien
// Lagarde's split-sum-friendly variant): clamping against (1-roughness)
// instead of 1.0 keeps rough surfaces from getting an unrealistically
// bright grazing-angle rim from the environment.
vec3 fresnel_schlick_roughness(float cosTheta, vec3 f0, float roughness) {
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

// Cascade shadow maps use one named sampler per cascade (GLSL 120 has no
// dynamically-indexable sampler arrays), mirroring the point-shadow
// per-face sampling functions below.
float sample_cascade_shadow_map(int cascade, vec2 uv) {
    if (cascade == 0) {
        return texture2D(uCascadeShadowMap0, uv).r;
    } else if (cascade == 1) {
        return texture2D(uCascadeShadowMap1, uv).r;
    } else if (cascade == 2) {
        return texture2D(uCascadeShadowMap2, uv).r;
    }
    return texture2D(uCascadeShadowMap3, uv).r;
}

vec4 cascade_shadow_position(int cascade, vec3 world_position) {
    if (cascade == 0) {
        return uCascadeViewProjection0 * vec4(world_position, 1.0);
    } else if (cascade == 1) {
        return uCascadeViewProjection1 * vec4(world_position, 1.0);
    } else if (cascade == 2) {
        return uCascadeViewProjection2 * vec4(world_position, 1.0);
    }
    return uCascadeViewProjection3 * vec4(world_position, 1.0);
}

// uCascadeSplitDepths.x/y/z/w are the camera-forward view-space distances
// where cascades 0/1/2/3 end, computed on the CPU side in
// Renderer::collect_object_uniforms using the same practical split scheme
// used to build the cascades themselves.
int select_cascade(float viewDepth) {
    if (viewDepth <= uCascadeSplitDepths.x) {
        return 0;
    } else if (viewDepth <= uCascadeSplitDepths.y) {
        return 1;
    } else if (viewDepth <= uCascadeSplitDepths.z) {
        return 2;
    }
    return 3;
}

float directional_shadow(vec3 n, vec3 light_dir) {
    if (uShadowsEnabled == 0) {
        return 1.0;
    }

    float viewDepth = dot(vWorldPosition - uCameraPosition, uCameraForward);
    if (viewDepth > uCascadeSplitDepths.w) {
        // Beyond the last cascade's far split: no shadow data captured out
        // here, so just treat the surface as lit rather than acne/clamp.
        return 1.0;
    }
    int cascade = select_cascade(viewDepth);

    vec4 shadowPosition = cascade_shadow_position(cascade, vWorldPosition);
    vec3 projected = shadowPosition.xyz / max(shadowPosition.w, 0.0001);
    projected = projected * 0.5 + 0.5;
    if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0 || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    // Farther cascades cover more world space per texel, so widen the bias
    // with cascade index to avoid acne without over-darkening nearby
    // shadows sampled from the tighter, higher-resolution-per-world-unit
    // cascade 0.
    float cascadeBiasScale = 1.0 + float(cascade) * 0.6;
    float bias = max(0.0015 * cascadeBiasScale * (1.0 - dot(n, light_dir)), 0.00045 * cascadeBiasScale);
    float shadow = 0.0;
    float texel = 1.0 / 1536.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = sample_cascade_shadow_map(cascade, projected.xy + vec2(float(x), float(y)) * texel);
            shadow += projected.z - bias > closest ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    return mix(0.38, 1.0, shadow);
}

float spot_shadow(vec3 n, vec3 light_dir, vec3 world_position) {
    if (uSpotShadowsEnabled == 0) {
        return 1.0;
    }

    vec4 shadowPosition = uSpotLightViewProjection * vec4(world_position, 1.0);
    vec3 projected = shadowPosition.xyz / max(shadowPosition.w, 0.0001);
    projected = projected * 0.5 + 0.5;
    if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0 || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    float bias = max(0.0025 * (1.0 - dot(n, light_dir)), 0.0008);
    float shadow = 0.0;
    float texel = 1.0 / 1024.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = texture2D(uSpotShadowMap, projected.xy + vec2(float(x), float(y)) * texel).r;
            shadow += projected.z - bias > closest ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    return mix(0.30, 1.0, shadow);
}
)" R"(

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

// Evaluates one light's contribution. Directional lights are infinitely far
// so only their direction matters; point/spot lights attenuate over range
// and spot lights additionally narrow by their cone angle. Only the first
// enabled directional light, first enabled point light, and first enabled
// spot light carry shadow maps today (castsShadow == 1); every other light
// is unshadowed.
vec3 evaluate_light(Light light, vec3 albedo, float metallic, float roughness, vec3 n, vec3 v) {
    if (light.intensity <= 0.0) {
        return vec3(0.0);
    }

    if (light.type == 0) {
        vec3 l = normalize(-light.direction);
        float shadow = light.castsShadow == 1 ? directional_shadow(n, l) : 1.0;
        return pbr_light(albedo, metallic, roughness, n, v, l, light.color * light.intensity) * shadow;
    }

    vec3 toLight = light.position - vWorldPosition;
    float dist = length(toLight);
    float range = max(light.range, 0.001);
    if (dist >= range) {
        return vec3(0.0);
    }
    vec3 l = toLight / max(dist, 0.0001);
    float attenuation = pow(clamp(1.0 - dist / range, 0.0, 1.0), 2.0);

    if (light.type == 2) {
        float cosAngle = dot(-l, normalize(light.direction));
        float spotAttenuation = clamp((cosAngle - light.outerCos) / max(light.innerCos - light.outerCos, 0.0001), 0.0, 1.0);
        attenuation *= spotAttenuation * spotAttenuation;
    }

    float shadow = 1.0;
    if (light.castsShadow == 1) {
        if (light.type == 1) {
            shadow = point_shadow(n, l);
        } else if (light.type == 2) {
            shadow = spot_shadow(n, l, vWorldPosition);
        }
    }
    return pbr_light(albedo, metallic, roughness, n, v, l, light.color * light.intensity * attenuation) * shadow;
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

    vec4 sampledBaseColorRgba = uHasBaseColorTexture == 1 ? texture2D(uBaseColorTexture, vUv) : vec4(1.0);
    vec3 sampledBaseColor = srgb_to_linear(sampledBaseColorRgba.rgb);
    vec3 albedo = clamp(uBaseColor * vColor * sampledBaseColor, 0.0, 1.0);

    float alpha = clamp(uBaseColorAlpha * sampledBaseColorRgba.a, 0.0, 1.0);
    if (uAlphaMode == 1) {
        // Mask: cut out below the threshold, otherwise render fully opaque.
        if (alpha < uAlphaCutoff) {
            discard;
        }
        alpha = 1.0;
    } else if (uAlphaMode == 0) {
        // Opaque: alpha is ignored entirely per the glTF spec.
        alpha = 1.0;
    }

    vec4 sampledMetallicRoughness = uHasMetallicRoughnessTexture == 1 ? texture2D(uMetallicRoughnessTexture, vUv) : vec4(1.0);
    float roughness = clamp(uRoughness * sampledMetallicRoughness.g, 0.04, 1.0);
    float metallic = clamp(uMetallic * sampledMetallicRoughness.b, 0.0, 1.0);
    float occlusion = mix(1.0, uHasOcclusionTexture == 1 ? texture2D(uOcclusionTexture, vUv).r : 1.0, uOcclusionStrength);
    if (uSsaoEnabled == 1) {
        // Screen-space AO darkens the same ambient term the glTF occlusion
        // texture already does, just from real-time scene geometry instead
        // of a baked-in texture — the two multiply together naturally.
        vec2 screenUv = gl_FragCoord.xy / uViewportSize;
        occlusion *= texture2D(uSsaoTexture, screenUv).r;
    }
    vec3 sampledEmissive = uHasEmissiveTexture == 1 ? srgb_to_linear(texture2D(uEmissiveTexture, vUv).rgb) : vec3(1.0);
    vec3 emissive = uEmissiveFactor * sampledEmissive;

    vec3 direct = vec3(0.0);
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (i >= uLightCount) {
            break;
        }
        direct += evaluate_light(uLights[i], albedo, metallic, roughness, n, v);
    }

    // Occlusion only darkens the ambient/IBL term, never direct light or
    // emissive self-illumination, per the glTF spec.
    vec3 ambient;
    if (uIblEnabled == 1) {
        vec3 f0 = mix(vec3(0.04), albedo, metallic);
        float ndotv = max(dot(n, v), 0.0);
        vec3 kS = fresnel_schlick_roughness(ndotv, f0, roughness);
        vec3 kD = (1.0 - kS) * (1.0 - metallic);
        vec3 irradiance = textureCube(uIrradianceMap, n).rgb;
        vec3 diffuseIBL = irradiance * albedo;

        vec3 r = reflect(-v, n);
        float maxLod = max(uPrefilteredMipCount - 1.0, 0.0);
#ifdef GL_ARB_shader_texture_lod
        vec3 prefilteredColor = textureCubeLod(uPrefilteredMap, r, roughness * maxLod).rgb;
#else
        vec3 prefilteredColor = textureCube(uPrefilteredMap, r).rgb;
#endif
        vec2 envBrdf = texture2D(uBrdfLut, vec2(ndotv, roughness)).rg;
        vec3 specularIBL = prefilteredColor * (f0 * envBrdf.x + envBrdf.y);

        ambient = (kD * diffuseIBL + specularIBL) * occlusion;
    } else {
        // Fallback flat ambient hack, used until an environment is loaded.
        ambient = albedo * mix(0.035, 0.075, 1.0 - roughness) * occlusion;
    }
    vec3 hdrColor = ambient + direct + emissive;
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
    } else if (uDebugView == 6) {
        gl_FragColor = vec4(vec3(occlusion), 1.0);
    } else if (uDebugView == 7) {
        gl_FragColor = vec4(emissive, 1.0);
    } else {
        gl_FragColor = vec4(uHdrOutputEnabled == 1 ? hdrColor : ldrColor, alpha);
    }
}
)";

    // uRadialExtrude switches the extrusion direction from the per-face
    // vertex normal to the direction from the mesh's local origin to the
    // vertex. For a hard-edged primitive with duplicated per-face vertices
    // (e.g. the built-in cube, which has 3 separate vertices per corner —
    // one per adjacent face, each with that face's normal), extruding along
    // the face normal pushes those 3 coincident corner vertices apart in 3
    // different directions, tearing a visible gap in the outline shell at
    // every corner. Since the built-in primitives are centered on their
    // local origin, the position-from-origin direction is identical for
    // all vertices that share a position (it doesn't depend on which face
    // they came from), so the corner stays sealed. This is only correct
    // for origin-centered convex primitives, so it's opt-in per draw call
    // (see draw_mesh_part_outline) rather than the default — imported
    // meshes with an off-center pivot keep the original normal-based
    // extrusion, which is the geometrically correct one for them.
    constexpr const char* outline_vertex_shader = R"(
#version 120
attribute vec3 aPosition;
attribute vec3 aNormal;
uniform mat4 uMvp;
uniform float uOutlineWidth;
uniform int uRadialExtrude;
void main() {
    vec3 direction = uRadialExtrude == 1 ? normalize(aPosition) : normalize(aNormal);
    gl_Position = uMvp * vec4(aPosition + direction * uOutlineWidth, 1.0);
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

    // uSceneColor is the raw HDR scene, uBloomTexture is BloomMip0 after the
    // full downsample/upsample chain (see Renderer::execute_bloom_pass) —
    // additively combined here before exposure/tonemapping so bloom reads
    // as "extra light" rather than a post-tonemap overlay. Vignette and the
    // parametric contrast/saturation/temperature grade run last, in LDR
    // space after gamma, and are skipped entirely in debug views so they
    // show raw data.
    constexpr const char* tonemap_fragment_shader = R"(
#version 120
uniform sampler2D uSceneColor;
uniform sampler2D uBloomTexture;
uniform int uBloomEnabled;
uniform float uBloomIntensity;
uniform float uExposure;
uniform int uDebugView;
uniform int uVignetteEnabled;
uniform float uVignetteIntensity;
uniform float uContrast;
uniform float uSaturation;
uniform float uTemperature;
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
    if (uBloomEnabled == 1) {
        hdrColor += texture2D(uBloomTexture, vUv).rgb * uBloomIntensity;
    }
    vec3 exposed = hdrColor * max(uExposure, 0.0);
    vec3 mapped = uDebugView == 0 ? aces_fitted(exposed) : clamp(hdrColor, 0.0, 1.0);
    vec3 outputColor = uDebugView == 0 ? pow(mapped, vec3(1.0 / 2.2)) : mapped;

    if (uDebugView == 0) {
        // Parametric grading only — a texture-based 3D LUT is the more
        // common AAA approach but is deferred (see AAA_PBR_ROADMAP.md).
        // Temperature nudges red up / blue down (or the reverse) directly;
        // saturation mixes toward Rec. 709 luma; contrast pivots around
        // mid-grey. Order matches a typical LDR grading stack.
        outputColor += vec3(uTemperature * 0.08, 0.0, -uTemperature * 0.08);
        float luma = dot(outputColor, vec3(0.2126, 0.7152, 0.0722));
        outputColor = mix(vec3(luma), outputColor, uSaturation);
        outputColor = (outputColor - 0.5) * uContrast + 0.5;
        outputColor = clamp(outputColor, 0.0, 1.0);

        if (uVignetteEnabled == 1) {
            vec2 centered = vUv - vec2(0.5);
            float dist = length(centered) * 1.4142135;
            float vignette = 1.0 - uVignetteIntensity * smoothstep(0.35, 1.1, dist);
            outputColor *= clamp(vignette, 0.0, 1.0);
        }
    }

    gl_FragColor = vec4(outputColor, 1.0);
}
)";

    // FXAA-style edge antialiasing: a single-pass, luma-edge-directed 3-tap
    // blend, not a port of the full NVIDIA FXAA 3.11 reference algorithm —
    // that one adds a per-edge search that walks along the edge's length in
    // both directions to find where it ends, plus a separate sub-pixel
    // aliasing removal term; both add real quality but also real sample
    // count and complexity. This keeps the same core idea (detect a
    // contrast edge from luma differences in the 3x3 neighborhood, then
    // blend along it rather than across it) in one pass, scoped down the
    // same way this project's bloom and SSAO passes are. Runs on the
    // tonemapped LDR image (uSceneColor here is "SceneColorLDR", not the
    // HDR scene color) since luma-based edge detection assumes an
    // LDR-ish contrast range.
    constexpr const char* fxaa_fragment_shader = R"(
#version 120
uniform sampler2D uSceneColor;
uniform vec2 uTexelSize;
varying vec2 vUv;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 colorCenter = texture2D(uSceneColor, vUv).rgb;
    vec3 colorN = texture2D(uSceneColor, vUv + vec2(0.0, -uTexelSize.y)).rgb;
    vec3 colorS = texture2D(uSceneColor, vUv + vec2(0.0,  uTexelSize.y)).rgb;
    vec3 colorE = texture2D(uSceneColor, vUv + vec2( uTexelSize.x, 0.0)).rgb;
    vec3 colorW = texture2D(uSceneColor, vUv + vec2(-uTexelSize.x, 0.0)).rgb;
    vec3 colorNW = texture2D(uSceneColor, vUv + vec2(-uTexelSize.x, -uTexelSize.y)).rgb;
    vec3 colorNE = texture2D(uSceneColor, vUv + vec2( uTexelSize.x, -uTexelSize.y)).rgb;
    vec3 colorSW = texture2D(uSceneColor, vUv + vec2(-uTexelSize.x,  uTexelSize.y)).rgb;
    vec3 colorSE = texture2D(uSceneColor, vUv + vec2( uTexelSize.x,  uTexelSize.y)).rgb;

    float lumaCenter = luma(colorCenter);
    float lumaN = luma(colorN);
    float lumaS = luma(colorS);
    float lumaE = luma(colorE);
    float lumaW = luma(colorW);
    float lumaNW = luma(colorNW);
    float lumaNE = luma(colorNE);
    float lumaSW = luma(colorSW);
    float lumaSE = luma(colorSE);

    float lumaMin = min(lumaCenter, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaCenter, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    // Low-contrast neighborhood: not an edge, leave the pixel untouched
    // rather than blurring flat shading or fine texture detail.
    float edgeThreshold = max(0.0625, lumaMax * 0.125);
    if (lumaRange < edgeThreshold) {
        gl_FragColor = vec4(colorCenter, 1.0);
        return;
    }

    // Sobel-style horizontal/vertical edge strength from the 3x3
    // neighborhood decides which axis the edge runs along.
    float edgeVert = abs(lumaNW + lumaSW - 2.0 * lumaW) * 2.0 + abs(lumaN + lumaS - 2.0 * lumaCenter) + abs(lumaNE + lumaSE - 2.0 * lumaE);
    float edgeHorz = abs(lumaNW + lumaNE - 2.0 * lumaN) * 2.0 + abs(lumaW + lumaE - 2.0 * lumaCenter) + abs(lumaSW + lumaSE - 2.0 * lumaS);
    bool isHorizontalEdge = edgeHorz >= edgeVert;

    float lumaPositive = isHorizontalEdge ? lumaS : lumaE;
    float lumaNegative = isHorizontalEdge ? lumaN : lumaW;
    float gradientPositive = abs(lumaPositive - lumaCenter);
    float gradientNegative = abs(lumaNegative - lumaCenter);
    float sampleDir = gradientPositive >= gradientNegative ? 1.0 : -1.0;

    // Blend two half-texel samples straddling the edge with the center
    // pixel — softens the aliased step without smearing perpendicular to
    // it (which would blur detail along the edge itself).
    vec2 sampleStep = (isHorizontalEdge ? vec2(0.0, uTexelSize.y) : vec2(uTexelSize.x, 0.0)) * sampleDir;
    vec3 colorAlong1 = texture2D(uSceneColor, vUv + sampleStep * 0.5).rgb;
    vec3 colorAlong2 = texture2D(uSceneColor, vUv - sampleStep * 0.5).rgb;
    vec3 blended = (colorAlong1 + colorAlong2 + colorCenter * 2.0) * 0.25;

    float blendFactor = clamp((lumaRange / max(lumaMax, 0.0001)) * 1.5, 0.0, 1.0);
    gl_FragColor = vec4(mix(colorCenter, blended, blendFactor), 1.0);
}
)";

    // Bloom bright-pass: extracts scene-color pixels above uThreshold into
    // a half-resolution target, with a soft-knee curve (from Unity's bloom
    // implementation notes) so contribution fades in over a small band
    // above the threshold instead of a hard on/off cutoff between
    // neighboring pixels of similar brightness.
    constexpr const char* bloom_threshold_fragment_shader = R"(
#version 120
uniform sampler2D uSceneColor;
uniform float uThreshold;
varying vec2 vUv;
void main() {
    vec3 color = texture2D(uSceneColor, vUv).rgb;
    float brightness = max(color.r, max(color.g, color.b));
    float knee = uThreshold * 0.5 + 0.0001;
    float soft = clamp(brightness - uThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.0001);
    float contribution = max(soft, brightness - uThreshold);
    contribution = contribution / max(brightness, 0.0001);
    gl_FragColor = vec4(color * contribution, 1.0);
}
)";

    // Downsample: a 4-tap box filter offset a half source-texel from the
    // destination center in each diagonal direction. Because the source is
    // bilinear-filtered, each tap already averages a 2x2 neighborhood, so
    // effectively this is a cheap ~13-tap-equivalent box filter — smoother
    // than a single point-sampled downsample without the cost of a real
    // wide Gaussian kernel.
    constexpr const char* bloom_downsample_fragment_shader = R"(
#version 120
uniform sampler2D uSource;
uniform vec2 uTexelSize;
varying vec2 vUv;
void main() {
    vec2 o = uTexelSize;
    vec3 sum = texture2D(uSource, vUv + vec2(-o.x, -o.y)).rgb
             + texture2D(uSource, vUv + vec2( o.x, -o.y)).rgb
             + texture2D(uSource, vUv + vec2(-o.x,  o.y)).rgb
             + texture2D(uSource, vUv + vec2( o.x,  o.y)).rgb;
    gl_FragColor = vec4(sum * 0.25, 1.0);
}
)";

    // Upsample: a standard 3x3 tent filter, drawn with additive blending
    // (GL_ONE, GL_ONE) onto the next-larger mip so its own downsampled
    // contents accumulate with the wider, blurrier mip below it. Run from
    // the smallest mip back up to mip 0, this is what turns a chain of
    // sharp downsampled copies into one smoothly-blurred composite.
    constexpr const char* bloom_upsample_fragment_shader = R"(
#version 120
uniform sampler2D uSource;
uniform vec2 uTexelSize;
varying vec2 vUv;
void main() {
    vec2 o = uTexelSize;
    vec3 sum = texture2D(uSource, vUv + vec2(-o.x, -o.y)).rgb
             + texture2D(uSource, vUv + vec2( 0.0, -o.y)).rgb * 2.0
             + texture2D(uSource, vUv + vec2( o.x, -o.y)).rgb
             + texture2D(uSource, vUv + vec2(-o.x,  0.0)).rgb * 2.0
             + texture2D(uSource, vUv + vec2( 0.0,  0.0)).rgb * 4.0
             + texture2D(uSource, vUv + vec2( o.x,  0.0)).rgb * 2.0
             + texture2D(uSource, vUv + vec2(-o.x,  o.y)).rgb
             + texture2D(uSource, vUv + vec2( 0.0,  o.y)).rgb * 2.0
             + texture2D(uSource, vUv + vec2( o.x,  o.y)).rgb;
    gl_FragColor = vec4(sum / 16.0, 1.0);
}
)";

    // SSAO normal+depth prepass: a second, cheap (position+normal only,
    // no textures) full-scene geometry draw into "SceneNormalDepth",
    // needed because SSAO has to sample every visible fragment's normal
    // and depth *before* the opaque pass shades it — there's no chicken-
    // and-egg way to compute occlusion for a fragment from data written by
    // that same fragment's own draw call. RGB stores the world-space
    // normal (RGBA16F holds signed values natively, no [-1,1]->[0,1]
    // packing needed); A stores view depth as the camera-forward distance
    // `dot(worldPos - cameraPos, cameraForward)` — the same metric already
    // used for cascade shadow selection — rather than canonical clip-space
    // depth, so the SSAO pass below can reconstruct world-space positions
    // from camera position/basis/FOV alone, with no matrix inverse needed
    // (Math.hpp has no Mat4::inverse(); this is the same workaround the
    // cascade frustum-corner code already uses).
    constexpr const char* normal_depth_vertex_shader = R"(
#version 120
attribute vec3 aPosition;
attribute vec3 aNormal;
uniform mat4 uModel;
uniform mat4 uMvp;
uniform vec3 uCameraPosition;
uniform vec3 uCameraForward;
varying vec3 vWorldNormal;
varying float vViewDepth;
void main() {
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldNormal = mat3(uModel) * aNormal;
    vViewDepth = dot(worldPosition.xyz - uCameraPosition, uCameraForward);
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    constexpr const char* normal_depth_fragment_shader = R"(
#version 120
varying vec3 vWorldNormal;
varying float vViewDepth;
void main() {
    gl_FragColor = vec4(normalize(vWorldNormal), vViewDepth);
}
)";

    // SSAO compute: a classic hemisphere-kernel ambient occlusion pass
    // (Crysis/LearnOpenGL-style), reformulated in world space relative to
    // the camera's own basis vectors instead of canonical view space, for
    // the same inverse-matrix-avoidance reason described above. The random
    // per-pixel hemisphere rotation is a cheap sin/hash function of screen
    // position instead of a tiled noise texture — one less GPU resource to
    // manage, at the cost of slightly more visible dithering (masked by
    // the blur pass below). Runs at half the viewport's resolution, like
    // the bloom mip chain, since AO is a soft, low-frequency effect that
    // doesn't need full-resolution precision.
    constexpr const char* ssao_fragment_shader = R"(
#version 120
uniform sampler2D uNormalDepth;
uniform vec3 uCameraPosition;
uniform vec3 uCameraForward;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform float uTanHalfFovY;
uniform float uAspect;
uniform float uRadius;
uniform float uBias;
uniform float uIntensity;
varying vec2 vUv;

vec3 reconstruct_world_position(vec2 uv, float depth) {
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = uv.y * 2.0 - 1.0;
    float halfHeight = depth * uTanHalfFovY;
    float halfWidth = halfHeight * uAspect;
    return uCameraPosition + uCameraForward * depth + uCameraRight * (ndcX * halfWidth) + uCameraUp * (ndcY * halfHeight);
}

float hash12(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

void main() {
    vec4 normalDepth = texture2D(uNormalDepth, vUv);
    float depth = normalDepth.a;
    if (depth <= 0.0001) {
        gl_FragColor = vec4(1.0);
        return;
    }
    vec3 normal = normalize(normalDepth.rgb);
    vec3 fragPos = reconstruct_world_position(vUv, depth);

    float angle = hash12(gl_FragCoord.xy) * 6.28318530718;
    vec3 randomVec = vec3(cos(angle), sin(angle), 0.0);
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);

    // A fixed hemisphere kernel (z >= 0 in tangent space), samples biased
    // toward the origin so more of them land close to the surface where
    // contact occlusion actually happens — same distribution strategy as
    // the LearnOpenGL SSAO reference.
    vec3 kernel[10];
    kernel[0] = vec3(-0.0025, -0.0050, 0.0046);
    kernel[1] = vec3(0.0140, -0.0523, 0.0113);
    kernel[2] = vec3(-0.0122, -0.0017, 0.0009);
    kernel[3] = vec3(-0.0089, 0.0387, 0.0073);
    kernel[4] = vec3(0.0225, 0.0791, 0.0510);
    kernel[5] = vec3(0.0571, -0.0543, 0.0514);
    kernel[6] = vec3(-0.2261, -0.2429, 0.0980);
    kernel[7] = vec3(-0.1401, 0.0358, 0.1402);
    kernel[8] = vec3(0.0151, -0.1381, 0.0094);
    kernel[9] = vec3(0.3504, -0.1406, 0.3051);

    float occlusion = 0.0;
    for (int i = 0; i < 10; i++) {
        vec3 sampleOffset = tangent * kernel[i].x + bitangent * kernel[i].y + normal * kernel[i].z;
        vec3 samplePos = fragPos + sampleOffset * uRadius;

        vec3 rel = samplePos - uCameraPosition;
        float sampleDepth = dot(rel, uCameraForward);
        if (sampleDepth <= 0.0001) {
            continue;
        }
        float sHalfHeight = sampleDepth * uTanHalfFovY;
        float sHalfWidth = sHalfHeight * uAspect;
        float sx = dot(rel, uCameraRight);
        float sy = dot(rel, uCameraUp);
        vec2 sampleUv = vec2(sx / sHalfWidth, sy / sHalfHeight) * 0.5 + 0.5;
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0) {
            continue;
        }

        // Occluded if the scene's actual depth at the sample's projected
        // screen position is closer to the camera than the sample point
        // itself (something is "in front of" where the sample expected
        // open space). The range check fades out occlusion from geometry
        // far behind the sample point, avoiding halos around silhouettes.
        float sceneDepth = texture2D(uNormalDepth, sampleUv).a;
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(depth - sceneDepth), 0.0001));
        occlusion += (sceneDepth < sampleDepth - uBias ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion = 1.0 - (occlusion / 10.0) * uIntensity;
    gl_FragColor = vec4(vec3(clamp(occlusion, 0.0, 1.0)), 1.0);
}
)";

    // Simple 3x3 box blur to smooth over the per-pixel hash-based dithering
    // in the raw SSAO result above.
    constexpr const char* ssao_blur_fragment_shader = R"(
#version 120
uniform sampler2D uAoRaw;
uniform vec2 uTexelSize;
varying vec2 vUv;
void main() {
    float result = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            result += texture2D(uAoRaw, vUv + vec2(float(x), float(y)) * uTexelSize).r;
        }
    }
    gl_FragColor = vec4(vec3(result / 9.0), 1.0);
}
)";

    // Skybox: draws the environment cubemap as the background, using a unit
    // cube and a view matrix with translation stripped so the cube always
    // surrounds the camera (see Renderer::draw_skybox). Writes straight into
    // whatever's currently bound (the HDR "SceneColor" target or, with HDR
    // off, the backbuffer directly), matching the HDR/non-HDR split every
    // other shader in this pipeline already does.
    constexpr const char* skybox_vertex_shader = R"(
#version 120
attribute vec3 aPosition;
varying vec3 vLocalPos;
uniform mat4 uViewProjection;
void main() {
    vLocalPos = aPosition;
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)";

    constexpr const char* skybox_fragment_shader = R"(
#version 120
varying vec3 vLocalPos;
uniform samplerCube uEnvMap;
uniform float uExposure;
uniform float uSkyIntensity;
uniform int uHdrOutputEnabled;
void main() {
    vec3 hdrColor = textureCube(uEnvMap, normalize(vLocalPos)).rgb * max(uSkyIntensity, 0.0);
    if (uHdrOutputEnabled == 1) {
        gl_FragColor = vec4(hdrColor, 1.0);
    } else {
        vec3 exposed = hdrColor * max(uExposure, 0.0);
        vec3 ldrColor = exposed / (exposed + vec3(1.0));
        gl_FragColor = vec4(pow(ldrColor, vec3(1.0 / 2.2)), 1.0);
    }
}
)";

    object_program_ = create_program(vertex_shader, fragment_shader);
    cache_light_uniform_locations();
    outline_program_ = create_program(outline_vertex_shader, outline_fragment_shader);
    depth_program_ = create_program(depth_vertex_shader, depth_fragment_shader);
    tonemap_program_ = create_program(tonemap_vertex_shader, tonemap_fragment_shader);
    bloom_threshold_program_ = create_program(tonemap_vertex_shader, bloom_threshold_fragment_shader);
    bloom_downsample_program_ = create_program(tonemap_vertex_shader, bloom_downsample_fragment_shader);
    bloom_upsample_program_ = create_program(tonemap_vertex_shader, bloom_upsample_fragment_shader);
    normal_depth_program_ = create_program(normal_depth_vertex_shader, normal_depth_fragment_shader);
    ssao_program_ = create_program(tonemap_vertex_shader, ssao_fragment_shader);
    ssao_blur_program_ = create_program(tonemap_vertex_shader, ssao_blur_fragment_shader);
    fxaa_program_ = create_program(tonemap_vertex_shader, fxaa_fragment_shader);
    skybox_program_ = create_program(skybox_vertex_shader, skybox_fragment_shader);

    // One fixed-size depth-only target per cascade, all the same resolution
    // for simplicity (a real budget-tuned engine might shrink resolution for
    // farther cascades, but a uniform size keeps the split-scheme math and
    // PCF texel size easy to reason about for now).
    for (int cascade = 0; cascade < kCascadeCount; ++cascade) {
        RenderTargetDesc cascade_desc;
        cascade_desc.name = "ShadowMapCascade" + std::to_string(cascade);
        cascade_desc.size = TargetSize::Fixed;
        cascade_desc.fixed_width = kCascadeShadowMapResolution;
        cascade_desc.fixed_height = kCascadeShadowMapResolution;
        cascade_desc.depth_format = TargetDepthFormat::Depth24;
        cascade_desc.linear_filter = false;
        acquire_render_target(cascade_desc);
    }

    RenderTargetDesc spot_shadow_map_desc;
    spot_shadow_map_desc.name = "SpotShadowMap";
    spot_shadow_map_desc.size = TargetSize::Fixed;
    spot_shadow_map_desc.fixed_width = kSpotShadowMapResolution;
    spot_shadow_map_desc.fixed_height = kSpotShadowMapResolution;
    spot_shadow_map_desc.depth_format = TargetDepthFormat::Depth24;
    spot_shadow_map_desc.linear_filter = false;
    acquire_render_target(spot_shadow_map_desc);

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

    // The split-sum BRDF LUT depends only on the analytic BRDF model, not on
    // any particular environment, so it's baked once here rather than in
    // Renderer::load_environment.
    ensure_brdf_lut_baked();
    return true;
}

void Renderer::cache_light_uniform_locations() {
    if (object_program_ == 0) {
        return;
    }

    light_count_uniform_location_ = glx.get_uniform_location(object_program_, "uLightCount");
    for (int i = 0; i < kMaxLights; ++i) {
        const std::string prefix = "uLights[" + std::to_string(i) + "].";
        LightUniformLocations& location = light_uniform_locations_[static_cast<std::size_t>(i)];
        location.type = glx.get_uniform_location(object_program_, (prefix + "type").c_str());
        location.position = glx.get_uniform_location(object_program_, (prefix + "position").c_str());
        location.direction = glx.get_uniform_location(object_program_, (prefix + "direction").c_str());
        location.color = glx.get_uniform_location(object_program_, (prefix + "color").c_str());
        location.intensity = glx.get_uniform_location(object_program_, (prefix + "intensity").c_str());
        location.range = glx.get_uniform_location(object_program_, (prefix + "range").c_str());
        location.inner_cos = glx.get_uniform_location(object_program_, (prefix + "innerCos").c_str());
        location.outer_cos = glx.get_uniform_location(object_program_, (prefix + "outerCos").c_str());
        location.casts_shadow = glx.get_uniform_location(object_program_, (prefix + "castsShadow").c_str());
    }
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
    uniforms.camera_forward = camera.forward();
    uniforms.shadows_enabled = engine.render_settings().shadows_enabled;
    uniforms.ibl_enabled = engine.render_settings().ibl_enabled && ibl_ready_;
    uniforms.ao_enabled = engine.render_settings().ao_enabled;

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
    case DebugView::Occlusion:
        uniforms.debug_view = 6;
        break;
    case DebugView::Emissive:
        uniforms.debug_view = 7;
        break;
    }

    bool found_directional = false;
    bool found_point = false;
    bool found_spot = false;
    Vec3 spot_shadow_position{};
    Vec3 spot_shadow_direction{0.0f, -1.0f, 0.0f};
    float spot_shadow_range = 10.0f;
    float spot_shadow_outer_half_angle = 0.5f;
    int light_index = 0;
    for (const auto& actor : scene.actors()) {
        const auto* light = actor->find_component<LightComponent>();
        if (light == nullptr || !light->enabled) {
            continue;
        }

        const Transform& transform = actor->root().local_transform;
        const bool is_shadow_caster_directional = light->light_kind == LightKind::Directional && !found_directional;
        const bool is_shadow_caster_point = light->light_kind == LightKind::Point && !found_point;
        const bool is_shadow_caster_spot = light->light_kind == LightKind::Spot && !found_spot;

        if (is_shadow_caster_directional) {
            uniforms.directional_direction = light_forward_direction(transform.rotation) * -1.0f;
            uniforms.directional_color = light->color;
            uniforms.directional_intensity = light->intensity;
            found_directional = true;
        } else if (is_shadow_caster_point) {
            uniforms.point_position = transform.translation;
            uniforms.point_color = light->color;
            uniforms.point_intensity = light->intensity;
            uniforms.point_range = light->range;
            found_point = true;
        } else if (is_shadow_caster_spot) {
            spot_shadow_position = transform.translation;
            spot_shadow_direction = light_forward_direction(transform.rotation);
            spot_shadow_range = std::max(light->range, 0.01f);
            spot_shadow_outer_half_angle = std::max(light->spot_angle_radians * 0.5f, 0.01f);
            found_spot = true;
        }

        if (light_index >= kMaxLights) {
            // Silently drop lights beyond the shader's fixed-size array; the
            // shadow-casting directional/point/spot light above is still
            // tracked even if it appears later than the array cap.
            continue;
        }

        GpuLight gpu_light;
        gpu_light.color = light->color;
        gpu_light.intensity = light->intensity;
        gpu_light.range = std::max(light->range, 0.01f);

        switch (light->light_kind) {
        case LightKind::Directional:
            gpu_light.type = 0;
            gpu_light.direction = light_forward_direction(transform.rotation);
            gpu_light.casts_shadow = is_shadow_caster_directional;
            break;
        case LightKind::Point:
            gpu_light.type = 1;
            gpu_light.position = transform.translation;
            gpu_light.casts_shadow = is_shadow_caster_point;
            break;
        case LightKind::Spot: {
            gpu_light.type = 2;
            gpu_light.position = transform.translation;
            gpu_light.direction = light_forward_direction(transform.rotation);
            const float outer_half_angle = std::max(light->spot_angle_radians * 0.5f, 0.01f);
            const float inner_half_angle = outer_half_angle * 0.82f;
            gpu_light.outer_cos = std::cos(outer_half_angle);
            gpu_light.inner_cos = std::cos(inner_half_angle);
            gpu_light.casts_shadow = is_shadow_caster_spot;
            break;
        }
        }

        uniforms.lights[static_cast<std::size_t>(light_index)] = gpu_light;
        ++light_index;
    }
    uniforms.light_count = light_index;

    if (!found_directional) {
        uniforms.directional_intensity = 0.0f;
    }

    // --- Cascaded shadow maps for the directional light ---------------------
    //
    // Practical split scheme (log/uniform blend, lambda = 0.5): each cascade
    // covers camera-forward distance (previous_split, split] out to a fixed
    // kCascadeShadowDistance, deliberately much shorter than the camera's
    // full 500-unit view distance, since directional shadow detail only
    // matters close to the camera. For each cascade, the camera's own
    // sub-frustum corners (computed directly from position/forward/right/up
    // and fov/aspect — no matrix inversion needed since this is a symmetric
    // perspective frustum) are fit with a bounding sphere; using a sphere
    // rather than a tight per-frame AABB keeps the shadow's world-space
    // texel size constant as the camera rotates, avoiding shimmering
    // ("swimming") shadow edges.
    const Vec3 light_direction = length(uniforms.directional_direction) > 0.0001f ? normalize(uniforms.directional_direction) : Vec3{0.35f, 0.9f, 0.28f};
    constexpr float kCascadeNear = 0.1f;
    constexpr float kCascadeShadowDistance = 60.0f;
    constexpr float kCascadeLambda = 0.5f;
    std::array<float, kCascadeCount> cascade_splits{};
    for (int i = 0; i < kCascadeCount; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(kCascadeCount);
        const float log_split = kCascadeNear * std::pow(kCascadeShadowDistance / kCascadeNear, p);
        const float uniform_split = kCascadeNear + (kCascadeShadowDistance - kCascadeNear) * p;
        cascade_splits[static_cast<std::size_t>(i)] = kCascadeLambda * log_split + (1.0f - kCascadeLambda) * uniform_split;
    }

    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    const Vec3 cam_pos = camera.position();
    const Vec3 cam_fwd = camera.forward();
    const Vec3 cam_right = camera.right();
    const Vec3 cam_up = camera.up();
    float previous_split = kCascadeNear;
    for (int i = 0; i < kCascadeCount; ++i) {
        const float split_far = cascade_splits[static_cast<std::size_t>(i)];

        std::array<Vec3, 8> corners{};
        int corner_index = 0;
        for (float depth : {previous_split, split_far}) {
            const float half_height = depth * std::tan(camera.fov_y_radians * 0.5f);
            const float half_width = half_height * aspect;
            const Vec3 center = cam_pos + cam_fwd * depth;
            const Vec3 up_extent = cam_up * half_height;
            const Vec3 right_extent = cam_right * half_width;
            corners[static_cast<std::size_t>(corner_index++)] = center + up_extent - right_extent;
            corners[static_cast<std::size_t>(corner_index++)] = center + up_extent + right_extent;
            corners[static_cast<std::size_t>(corner_index++)] = center - up_extent - right_extent;
            corners[static_cast<std::size_t>(corner_index++)] = center - up_extent + right_extent;
        }

        Vec3 slice_center{0.0f, 0.0f, 0.0f};
        for (const Vec3& corner : corners) {
            slice_center += corner;
        }
        slice_center *= (1.0f / 8.0f);

        float radius = 0.0f;
        for (const Vec3& corner : corners) {
            radius = std::max(radius, length(corner - slice_center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;
        radius = std::max(radius, 0.5f);

        const Vec3 cascade_light_eye = slice_center + light_direction * (radius + 5.0f);
        const Mat4 cascade_view = Mat4::look_at(cascade_light_eye, slice_center, {0.0f, 1.0f, 0.0f});
        const Mat4 cascade_projection = Mat4::orthographic(-radius, radius, -radius, radius, 0.1f, radius * 2.0f + 10.0f);
        uniforms.cascade_view_projections[static_cast<std::size_t>(i)] = cascade_projection * cascade_view;
        uniforms.cascade_split_depths[static_cast<std::size_t>(i)] = split_far;

        previous_split = split_far;
    }

    const RenderTarget* cascade0_target = find_render_target("ShadowMapCascade0");
    uniforms.shadows_enabled = uniforms.shadows_enabled && found_directional
        && cascade0_target != nullptr && cascade0_target->depth_texture != 0
        && depth_program_ != 0;

    // --- Spot light shadow map ----------------------------------------------
    if (found_spot) {
        const Vec3 spot_dir = length(spot_shadow_direction) > 0.0001f ? normalize(spot_shadow_direction) : Vec3{0.0f, -1.0f, 0.0f};
        // A little extra margin beyond the cone's own angle keeps the cone's
        // silhouette from being clipped exactly at the shadow frustum edge.
        const float fov = std::clamp(spot_shadow_outer_half_angle * 2.0f + 0.15f, 0.2f, 3.0f);
        const float far_plane = std::max(1.0f, spot_shadow_range);
        const Mat4 spot_projection = Mat4::perspective(fov, 1.0f, 0.05f, far_plane);
        const Vec3 spot_target = spot_shadow_position + spot_dir * far_plane;
        Vec3 spot_up{0.0f, 1.0f, 0.0f};
        if (std::abs(dot(spot_dir, spot_up)) > 0.999f) {
            spot_up = Vec3{1.0f, 0.0f, 0.0f};
        }
        const Mat4 spot_view = Mat4::look_at(spot_shadow_position, spot_target, spot_up);
        uniforms.spot_light_view_projection = spot_projection * spot_view;
    }
    const RenderTarget* spot_shadow_map_target = find_render_target("SpotShadowMap");
    uniforms.spot_shadows_enabled = engine.render_settings().shadows_enabled
        && found_spot
        && spot_shadow_map_target != nullptr && spot_shadow_map_target->depth_texture != 0
        && depth_program_ != 0;
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

unsigned int Renderer::resolve_emissive_texture(const Material& material) const {
    if (material.emissive_texture_path.empty()) {
        return white_texture_;
    }

    auto it = texture_cache_.find(material.emissive_texture_path);
    if (it == texture_cache_.end()) {
        it = texture_cache_.emplace(material.emissive_texture_path, create_texture_from_file(material.emissive_texture_path)).first;
    }
    if (it->second.failed || it->second.id == 0) {
        return white_texture_;
    }
    return it->second.id;
}

unsigned int Renderer::resolve_occlusion_texture(const Material& material) const {
    if (material.occlusion_texture_path.empty()) {
        return white_texture_;
    }

    // glTF commonly packs occlusion into the red channel of the same texture
    // used for metallic/roughness (an "ORM" texture: R=occlusion,
    // G=roughness, B=metallic). When that's the case this resolves to the
    // same cached GPU texture as resolve_metallic_roughness_texture, just
    // bound to a different sampler/unit, rather than uploading it twice.
    auto it = texture_cache_.find(material.occlusion_texture_path);
    if (it == texture_cache_.end()) {
        it = texture_cache_.emplace(material.occlusion_texture_path, create_texture_from_file(material.occlusion_texture_path)).first;
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

RenderTarget& Renderer::acquire_render_target(const RenderTargetDesc& desc) {
    auto it = render_targets_.find(desc.name);
    if (it == render_targets_.end()) {
        RenderTarget target;
        target.desc = desc;
        it = render_targets_.emplace(desc.name, std::move(target)).first;
    }

    RenderTarget& target = it->second;
    const int desired_width = desc.size == TargetSize::Viewport ? width_ : desc.fixed_width;
    const int desired_height = desc.size == TargetSize::Viewport ? height_ : desc.fixed_height;
    if (target.framebuffer == 0 || target.width != desired_width || target.height != desired_height) {
        destroy_render_target(target);
        target.desc = desc;
        target.width = std::max(1, desired_width);
        target.height = std::max(1, desired_height);
        create_render_target_resources(target);
    }
    return target;
}

void Renderer::create_render_target_resources(RenderTarget& target) const {
    if (!glx.ready()) {
        return;
    }

    const bool wants_color = target.desc.color_format != TargetColorFormat::None;
    const bool wants_depth = target.desc.depth_format != TargetDepthFormat::None;
    const unsigned int filter = target.desc.linear_filter ? kLinear : kNearest;
    const unsigned int wrap = target.desc.clamp_to_edge ? kClampToEdge : kRepeat;

    if (wants_color) {
        glGenTextures(1, &target.color_texture);
        glBindTexture(kTexture2D, target.color_texture);
        glTexParameteri(kTexture2D, kTextureMinFilter, filter);
        glTexParameteri(kTexture2D, kTextureMagFilter, filter);
        glTexParameteri(kTexture2D, kTextureWrapS, wrap);
        glTexParameteri(kTexture2D, kTextureWrapT, wrap);
        const unsigned int internal_format = target.desc.color_format == TargetColorFormat::Rgba16f ? kRgba16f : kRgba;
        const unsigned int data_type = target.desc.color_format == TargetColorFormat::Rgba16f ? kFloat : kUnsignedByte;
        glTexImage2D(kTexture2D, 0, static_cast<int>(internal_format), target.width, target.height, 0, kRgba, data_type, nullptr);
    }

    if (wants_depth) {
        glGenTextures(1, &target.depth_texture);
        glBindTexture(kTexture2D, target.depth_texture);
        glTexParameteri(kTexture2D, kTextureMinFilter, target.desc.linear_filter ? kLinear : kNearest);
        glTexParameteri(kTexture2D, kTextureMagFilter, target.desc.linear_filter ? kLinear : kNearest);
        glTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
        glTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
        glTexImage2D(kTexture2D, 0, static_cast<int>(kDepthComponent), target.width, target.height, 0, kDepthComponent, kFloat, nullptr);
    }

    glx.gen_framebuffers(1, &target.framebuffer);
    glx.bind_framebuffer(kFramebuffer, target.framebuffer);
    if (wants_depth) {
        glx.framebuffer_texture_2d(kFramebuffer, kDepthAttachment, kTexture2D, target.depth_texture, 0);
    }
    if (wants_color) {
        glx.framebuffer_texture_2d(kFramebuffer, kColorAttachment0, kTexture2D, target.color_texture, 0);
        glDrawBuffer(kColorAttachment0);
        glReadBuffer(kColorAttachment0);
    } else {
        glDrawBuffer(kNone);
        glReadBuffer(kNone);
    }

    if (glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
        glx.bind_framebuffer(kFramebuffer, 0);
        throw std::runtime_error("render target '" + target.desc.name + "' framebuffer is incomplete");
    }
    glx.bind_framebuffer(kFramebuffer, 0);
    glBindTexture(kTexture2D, 0);
}

void Renderer::destroy_render_target(RenderTarget& target) const {
    if (target.framebuffer != 0 && glx.delete_framebuffers != nullptr) {
        glx.delete_framebuffers(1, &target.framebuffer);
    }
    if (target.color_texture != 0) {
        glDeleteTextures(1, &target.color_texture);
    }
    if (target.depth_texture != 0) {
        glDeleteTextures(1, &target.depth_texture);
    }
    target.framebuffer = 0;
    target.color_texture = 0;
    target.depth_texture = 0;
    target.width = 0;
    target.height = 0;
}

void Renderer::destroy_all_render_targets() {
    for (auto& [name, target] : render_targets_) {
        (void)name;
        destroy_render_target(target);
    }
    render_targets_.clear();
}

RenderTarget* Renderer::find_render_target(const std::string& name) {
    auto it = render_targets_.find(name);
    return it == render_targets_.end() ? nullptr : &it->second;
}

const RenderTarget* Renderer::find_render_target(const std::string& name) const {
    auto it = render_targets_.find(name);
    return it == render_targets_.end() ? nullptr : &it->second;
}

std::vector<RenderPassDescriptor> Renderer::describe_passes() const {
    return {
        {RenderPassKind::ShadowMaps, "Shadow Maps", {}, {"ShadowMapCascade0", "ShadowMapCascade1", "ShadowMapCascade2", "ShadowMapCascade3", "SpotShadowMap", "PointShadowCube (legacy)"}},
        {RenderPassKind::SceneColor, "Scene Color Setup", {}, {"SceneColor"}},
        {RenderPassKind::AmbientOcclusion, "Ambient Occlusion", {}, {"SceneNormalDepth", "SceneAORaw", "SceneAO"}},
        {RenderPassKind::ActorOutlines, "Actor Outlines", {}, {"SceneColor"}},
        {RenderPassKind::EditorGrid, "Editor Grid", {}, {"SceneColor"}},
        {RenderPassKind::OpaqueGeometry, "Opaque Geometry", {"ShadowMapCascade0", "ShadowMapCascade1", "ShadowMapCascade2", "ShadowMapCascade3", "SpotShadowMap", "PointShadowCube (legacy)", "SceneAO"}, {"SceneColor"}},
        {RenderPassKind::TransparentGeometry, "Transparent Geometry", {"ShadowMapCascade0", "ShadowMapCascade1", "ShadowMapCascade2", "ShadowMapCascade3", "SpotShadowMap", "PointShadowCube (legacy)", "SceneAO"}, {"SceneColor"}},
        {RenderPassKind::Bloom, "Bloom", {"SceneColor"}, {"BloomMip0", "BloomMip1", "BloomMip2", "BloomMip3", "BloomMip4"}},
        {RenderPassKind::Tonemap, "Tonemap", {"SceneColor", "BloomMip0"}, {"Backbuffer (or SceneColorLDR if FXAA on)"}},
        {RenderPassKind::Fxaa, "FXAA", {"SceneColorLDR"}, {"Backbuffer"}},
        {RenderPassKind::EditorOverlay, "Editor Overlay", {}, {"Backbuffer"}},
    };
}

std::vector<RenderTargetInfo> Renderer::render_target_info() const {
    std::vector<RenderTargetInfo> info;
    info.reserve(render_targets_.size());
    for (const auto& [name, target] : render_targets_) {
        info.push_back({name, target.width, target.height, target.color_texture != 0, target.depth_texture != 0});
    }
    std::sort(info.begin(), info.end(), [](const RenderTargetInfo& a, const RenderTargetInfo& b) {
        return a.name < b.name;
    });
    return info;
}

// Draws a clip-space-covering triangle strip with matching UVs, used by
// every screen-space post-process pass (tonemap, bloom threshold/downsample/
// upsample). Caller is responsible for the active program, its uniforms,
// bound textures, and framebuffer/viewport state.
void Renderer::draw_fullscreen_quad() const {
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
}

void Renderer::render_tonemapped_frame(const RenderPipelineSettings& settings, int debug_view) const {
    const RenderTarget* scene_color = find_render_target("SceneColor");
    if (tonemap_program_ == 0 || scene_color == nullptr || scene_color->color_texture == 0) {
        return;
    }

    const RenderTarget* bloom = settings.bloom_enabled ? find_render_target("BloomMip0") : nullptr;
    const bool bloom_available = bloom != nullptr && bloom->color_texture != 0;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glx.use_program(tonemap_program_);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uSceneColor"), 0);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uBloomTexture"), 1);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uBloomEnabled"), bloom_available ? 1 : 0);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uBloomIntensity"), settings.bloom_intensity);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uExposure"), settings.exposure);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uDebugView"), debug_view);
    glx.uniform_1i(glx.get_uniform_location(tonemap_program_, "uVignetteEnabled"), settings.vignette_enabled ? 1 : 0);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uVignetteIntensity"), settings.vignette_intensity);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uContrast"), settings.color_grade_contrast);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uSaturation"), settings.color_grade_saturation);
    glx.uniform_1f(glx.get_uniform_location(tonemap_program_, "uTemperature"), settings.color_grade_temperature);
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, scene_color->color_texture);
    glx.active_texture(kTexture1);
    glBindTexture(kTexture2D, bloom_available ? bloom->color_texture : 0);

    draw_fullscreen_quad();

    glBindTexture(kTexture2D, 0);
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, 0);
    glx.use_program(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

// Draws `cube_mesh_` using only its position attribute, matching the layout
// every IBL bake shader (and the skybox shader) expects — mirrors how
// draw_depth_gpu_mesh reuses the same mesh/VAO with a position-only depth
// shader. Caller is responsible for the active program, its uniforms, and
// viewport/framebuffer state; this just issues the draw call.
void Renderer::draw_ibl_cube_geometry() const {
    const GpuMesh& mesh = cube_mesh_;
    if (mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

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
}

// Allocates a GL_TEXTURE_CUBE_MAP with `mip_levels` levels of storage
// (level 0 is width x height, halving per level) on all six faces. Cube
// wrap modes are all clamp-to-edge, which is standard practice for cubemaps
// to avoid seam artifacts at face boundaries.
unsigned int Renderer::create_cubemap(int width, int height, int mip_levels, bool mipmap_filtering) const {
    unsigned int texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(kTextureCubeMap, texture);
    for (int face = 0; face < 6; ++face) {
        for (int mip = 0; mip < mip_levels; ++mip) {
            const int mip_width = std::max(1, width >> mip);
            const int mip_height = std::max(1, height >> mip);
            glTexImage2D(kTextureCubeMapPositiveX + static_cast<unsigned int>(face), mip, static_cast<int>(kRgba16f), mip_width, mip_height, 0, kRgba, kFloat, nullptr);
        }
    }
    glTexParameteri(kTextureCubeMap, kTextureMinFilter, static_cast<int>(mipmap_filtering ? kLinearMipmapLinear : kLinear));
    glTexParameteri(kTextureCubeMap, kTextureMagFilter, static_cast<int>(kLinear));
    glTexParameteri(kTextureCubeMap, kTextureWrapS, static_cast<int>(kClampToEdge));
    glTexParameteri(kTextureCubeMap, kTextureWrapT, static_cast<int>(kClampToEdge));
    glTexParameteri(kTextureCubeMap, kTextureWrapR, static_cast<int>(kClampToEdge));
    // The prefiltered specular cubemap only allocates `mip_levels` levels
    // (e.g. 5, down to a 8x8 base) rather than the full chain down to 1x1;
    // without clamping GL_TEXTURE_MAX_LEVEL, GL_LINEAR_MIPMAP_LINEAR
    // filtering would consider the texture mipmap-incomplete (expecting
    // levels down to 1x1) and sample undefined/black.
    glTexParameteri(kTextureCubeMap, kTextureMaxLevel, mip_levels - 1);
    glBindTexture(kTextureCubeMap, 0);
    return texture;
}

void Renderer::destroy_ibl_resources() {
    if (ibl_env_cubemap_ != 0) {
        glDeleteTextures(1, &ibl_env_cubemap_);
        ibl_env_cubemap_ = 0;
    }
    if (ibl_irradiance_cubemap_ != 0) {
        glDeleteTextures(1, &ibl_irradiance_cubemap_);
        ibl_irradiance_cubemap_ = 0;
    }
    if (ibl_prefiltered_cubemap_ != 0) {
        glDeleteTextures(1, &ibl_prefiltered_cubemap_);
        ibl_prefiltered_cubemap_ = 0;
    }
    ibl_prefiltered_mip_count_ = 0;
    ibl_ready_ = false;
    ibl_environment_path_.clear();
}

// Bakes the split-sum BRDF LUT (Karis/Epic's environment-BRDF
// approximation): a 2D texture indexed by (NdotV, roughness) storing a
// scale/bias pair for F0 that lets the specular IBL term skip a per-pixel
// Fresnel integral. This depends only on the analytic BRDF model, not on
// any specific environment, so it's baked exactly once and reused across
// every environment the user loads afterward.
void Renderer::ensure_brdf_lut_baked() {
    if (ibl_brdf_lut_ != 0 || !glx.ready()) {
        return;
    }

    constexpr int kLutResolution = 512;

    glGenTextures(1, &ibl_brdf_lut_);
    glBindTexture(kTexture2D, ibl_brdf_lut_);
    glTexParameteri(kTexture2D, kTextureMinFilter, static_cast<int>(kLinear));
    glTexParameteri(kTexture2D, kTextureMagFilter, static_cast<int>(kLinear));
    glTexParameteri(kTexture2D, kTextureWrapS, static_cast<int>(kClampToEdge));
    glTexParameteri(kTexture2D, kTextureWrapT, static_cast<int>(kClampToEdge));
    glTexImage2D(kTexture2D, 0, static_cast<int>(kRgba16f), kLutResolution, kLutResolution, 0, kRgba, kFloat, nullptr);
    glBindTexture(kTexture2D, 0);

    unsigned int framebuffer = 0;
    glx.gen_framebuffers(1, &framebuffer);
    glx.bind_framebuffer(kFramebuffer, framebuffer);
    glx.framebuffer_texture_2d(kFramebuffer, kColorAttachment0, kTexture2D, ibl_brdf_lut_, 0);
    glDrawBuffer(kColorAttachment0);
    if (glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
        glx.bind_framebuffer(kFramebuffer, 0);
        glx.delete_framebuffers(1, &framebuffer);
        glDeleteTextures(1, &ibl_brdf_lut_);
        ibl_brdf_lut_ = 0;
        throw std::runtime_error("BRDF LUT framebuffer is incomplete");
    }

    constexpr const char* fullscreen_vertex_shader = R"(
#version 120
varying vec2 vUv;
void main() {
    vUv = gl_MultiTexCoord0.xy;
    gl_Position = gl_Vertex;
}
)";

    // Same Hammersley/importance-sampling machinery as the specular
    // prefilter pass below (radical_inverse_base2 avoids GLSL 120's lack of
    // bitwise operators by summing bit/2^(i+1) with plain float math), plus
    // the IBL-specific Smith geometry term (k = roughness^2/2, distinct
    // from the direct-light k = (roughness+1)^2/8 used elsewhere in this
    // shader file) per Karis's split-sum derivation.
    constexpr const char* brdf_lut_fragment_shader = R"(
#version 120
varying vec2 vUv;
const float PI = 3.14159265359;

float radical_inverse_base2(int bits) {
    float result = 0.0;
    float denom = 1.0;
    for (int i = 0; i < 16; ++i) {
        denom *= 2.0;
        float bit = mod(float(bits), 2.0);
        bits = int(float(bits) / 2.0);
        result += bit / denom;
    }
    return result;
}

vec2 hammersley(int i, int n) {
    return vec2(float(i) / float(n), radical_inverse_base2(i));
}

vec3 importance_sample_ggx(vec2 xi, vec3 n, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 h;
    h.x = cos(phi) * sinTheta;
    h.y = sin(phi) * sinTheta;
    h.z = cosTheta;
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * h.x + bitangent * h.y + n * h.z);
}

float geometry_schlick_ggx_ibl(float ndotv, float roughness) {
    float k = (roughness * roughness) / 2.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float geometry_smith_ibl(float ndotv, float ndotl, float roughness) {
    return geometry_schlick_ggx_ibl(ndotv, roughness) * geometry_schlick_ggx_ibl(ndotl, roughness);
}

vec2 integrate_brdf(float ndotv, float roughness) {
    vec3 v;
    v.x = sqrt(max(1.0 - ndotv * ndotv, 0.0));
    v.y = 0.0;
    v.z = ndotv;

    float a = 0.0;
    float b = 0.0;
    vec3 n = vec3(0.0, 0.0, 1.0);

    const int SAMPLE_COUNT = 64;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        vec2 xi = hammersley(i, SAMPLE_COUNT);
        vec3 h = importance_sample_ggx(xi, n, roughness);
        vec3 l = normalize(2.0 * dot(v, h) * h - v);

        float ndotl = max(l.z, 0.0);
        float ndoth = max(h.z, 0.0);
        float vdoth = max(dot(v, h), 0.0);

        if (ndotl > 0.0) {
            float g = geometry_smith_ibl(ndotv, ndotl, roughness);
            float gVis = (g * vdoth) / max(ndoth * ndotv, 0.0001);
            float fc = pow(1.0 - vdoth, 5.0);
            a += (1.0 - fc) * gVis;
            b += fc * gVis;
        }
    }
    a /= float(SAMPLE_COUNT);
    b /= float(SAMPLE_COUNT);
    return vec2(a, b);
}

void main() {
    vec2 result = integrate_brdf(clamp(vUv.x, 0.001, 1.0), clamp(vUv.y, 0.001, 1.0));
    gl_FragColor = vec4(result, 0.0, 1.0);
}
)";

    glViewport(0, 0, kLutResolution, kLutResolution);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    const unsigned int program = create_program(fullscreen_vertex_shader, brdf_lut_fragment_shader);
    glx.use_program(program);
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
    glx.use_program(0);
    glx.delete_program(program);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
    glx.delete_framebuffers(1, &framebuffer);
}

void Renderer::bake_ibl_environment(const HdrImage& hdr) {
    if (!glx.ready() || hdr.width <= 0 || hdr.height <= 0) {
        throw std::runtime_error("cannot bake IBL environment: renderer or HDR image not ready");
    }

    constexpr int kEnvResolution = 512;
    // Full mip chain for the reprojected environment cubemap, 512 down to
    // 8x8. This exists purely so the specular prefilter pass below can pull
    // pre-blurred samples instead of re-reading the full-detail 512x512
    // panorama for every one of its importance samples (see the "filtered
    // importance sampling" comment above the prefilter shader).
    constexpr int kEnvMipLevels = 7;
    constexpr int kIrradianceResolution = 32;
    constexpr int kPrefilterResolution = 128;
    constexpr int kPrefilterMipLevels = 5;

    // Everything below builds into fresh, locally-owned GL objects and only
    // touches ibl_*_ member state at the very end, after every bake pass has
    // succeeded — so if this throws partway through (e.g. an unsupported
    // driver), whatever environment was loaded before this call keeps
    // rendering instead of leaving the renderer with a half-baked one. The
    // one trade-off is that a failed bake leaks the partially-created GL
    // objects created up to the point of failure; that's an accepted,
    // low-severity cost for a rare error path (see AAA_PBR_ROADMAP.md).
    unsigned int equirect_texture = 0;
    glGenTextures(1, &equirect_texture);
    glBindTexture(kTexture2D, equirect_texture);
    glTexParameteri(kTexture2D, kTextureMinFilter, static_cast<int>(kLinear));
    glTexParameteri(kTexture2D, kTextureMagFilter, static_cast<int>(kLinear));
    glTexParameteri(kTexture2D, kTextureWrapS, static_cast<int>(kClampToEdge));
    glTexParameteri(kTexture2D, kTextureWrapT, static_cast<int>(kClampToEdge));
    glTexImage2D(kTexture2D, 0, static_cast<int>(kRgba16f), hdr.width, hdr.height, 0, kRgb, kFloat, hdr.pixels.data());
    glBindTexture(kTexture2D, 0);

    const unsigned int env_cubemap = create_cubemap(kEnvResolution, kEnvResolution, kEnvMipLevels, true);
    const unsigned int irradiance_cubemap = create_cubemap(kIrradianceResolution, kIrradianceResolution, 1, false);
    const unsigned int prefiltered_cubemap = create_cubemap(kPrefilterResolution, kPrefilterResolution, kPrefilterMipLevels, true);

    const std::array<Mat4, 6> capture_view_projections = cube_face_view_projections({0.0f, 0.0f, 0.0f}, 0.05f, 10.0f);

    unsigned int bake_framebuffer = 0;
    glx.gen_framebuffers(1, &bake_framebuffer);
    glx.bind_framebuffer(kFramebuffer, bake_framebuffer);
    glDrawBuffer(kColorAttachment0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    constexpr const char* ibl_cube_vertex_shader = R"(
#version 120
attribute vec3 aPosition;
varying vec3 vLocalPos;
uniform mat4 uMvp;
void main() {
    vLocalPos = aPosition;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    // Equirect -> cubemap: reprojects the source panorama onto each of the
    // six faces by converting each face's local-space direction to the
    // standard equirectangular UV (LearnOpenGL's sampleSphericalMap).
    {
        constexpr const char* equirect_to_cubemap_fragment_shader = R"(
#version 120
varying vec3 vLocalPos;
uniform sampler2D uEquirect;
const vec2 invAtan = vec2(0.1591549, 0.3183099);
vec2 sample_spherical_map(vec3 dir) {
    vec2 uv = vec2(atan(dir.z, dir.x), asin(clamp(dir.y, -1.0, 1.0)));
    uv *= invAtan;
    uv += 0.5;
    // HdrLoader stores scanlines in the file's own top-down order (Radiance
    // "-Y H +X W" resolution strings, i.e. row 0 of the decoded buffer is
    // the top of the source panorama), but GL treats row 0 of uploaded
    // pixel data as texture-space v=0. Left alone, that samples the source
    // image bottom-up: the whole environment (skybox, and every IBL term
    // baked from it below) comes out vertically flipped. Flip v here,
    // before it ever reaches a cubemap face, so every downstream bake
    // (env/irradiance/prefilter) is already correct.
    uv.y = 1.0 - uv.y;
    return uv;
}
void main() {
    vec3 dir = normalize(vLocalPos);
    gl_FragColor = vec4(texture2D(uEquirect, sample_spherical_map(dir)).rgb, 1.0);
}
)";
        const unsigned int program = create_program(ibl_cube_vertex_shader, equirect_to_cubemap_fragment_shader);
        glx.use_program(program);
        glx.uniform_1i(glx.get_uniform_location(program, "uEquirect"), 0);
        glx.active_texture(kTexture0);
        glBindTexture(kTexture2D, equirect_texture);
        glViewport(0, 0, kEnvResolution, kEnvResolution);
        for (int face = 0; face < 6; ++face) {
            glx.uniform_matrix_4fv(glx.get_uniform_location(program, "uMvp"), 1, kFalse, capture_view_projections[static_cast<std::size_t>(face)].m.data());
            glx.framebuffer_texture_2d(kFramebuffer, kColorAttachment0, kTextureCubeMapPositiveX + static_cast<unsigned int>(face), env_cubemap, 0);
            if (face == 0 && glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
                throw std::runtime_error("environment cubemap framebuffer is incomplete");
            }
            glClear(GL_COLOR_BUFFER_BIT);
            draw_ibl_cube_geometry();
        }
        glBindTexture(kTexture2D, 0);
        glx.use_program(0);
        glx.delete_program(program);
    }
    glDeleteTextures(1, &equirect_texture);

    // Build env_cubemap's mip chain (mip 0 was just filled by the
    // reprojection pass above) so the prefilter pass below can sample
    // pre-blurred, lower-resolution versions of the environment instead of
    // re-reading the full-detail base level for every importance sample.
    glBindTexture(kTextureCubeMap, env_cubemap);
    glx.generate_mipmap(kTextureCubeMap);
    glBindTexture(kTextureCubeMap, 0);

    // Diffuse irradiance convolution: cosine-weighted hemisphere integral
    // per output texel, sampled at a coarse angular step since irradiance
    // varies very smoothly (see AAA_PBR_ROADMAP.md for the accuracy/bake
    // time trade-off).
    {
        constexpr const char* irradiance_fragment_shader = R"(
#version 120
#extension GL_ARB_shader_texture_lod : enable
varying vec3 vLocalPos;
uniform samplerCube uEnvMap;
const float PI = 3.14159265359;
void main() {
    vec3 n = normalize(vLocalPos);
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, n));
    up = normalize(cross(n, right));

    vec3 irradiance = vec3(0.0);
    float sampleDelta = 0.05;
    float samples = 0.0;
    for (float phi = 0.0; phi < 6.283185; phi += sampleDelta) {
        for (float theta = 0.0; theta < 1.570796; theta += sampleDelta) {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * n;
            // Deliberately read a pre-blurred mip of env_cubemap (32x32,
            // built by glGenerateMipmap's box/linear downsampling), not the
            // full-detail 512x512 base level. This hemisphere sweep is a
            // fixed, sparse angular grid (sampleDelta = 0.05 rad, ~2.9
            // degrees between samples) — a small, directly-visible sun disc
            // in the source panorama is often narrower than one grid step,
            // so at full detail it's a lottery whether any given texel's
            // grid happens to land on it: neighboring output texels that
            // "miss" the sun stay dim while the rare one that "hits" it
            // spikes, producing a hard-edged checkerboard of bright/dark
            // texels across the baked irradiance map. By mip 4, that same
            // sun disc has already been box-averaged into its surrounding
            // ~16x16 source-pixel neighborhood, so every sample in the
            // sweep sees a smoothly-varying (already low-pass) version of
            // it instead of an all-or-nothing spike — which is also just a
            // correct way to compute this: irradiance is an integral over
            // the whole hemisphere, an inherently low-frequency quantity,
            // so convolving it from a matching low-frequency source is both
            // cheaper and more numerically stable than reading full detail
            // and hoping the sparse grid catches every bright feature.
#ifdef GL_ARB_shader_texture_lod
            irradiance += textureCubeLod(uEnvMap, sampleVec, 4.0).rgb * cos(theta) * sin(theta);
#else
            irradiance += textureCube(uEnvMap, sampleVec).rgb * cos(theta) * sin(theta);
#endif
            samples += 1.0;
        }
    }
    irradiance = PI * irradiance / max(samples, 1.0);
    gl_FragColor = vec4(irradiance, 1.0);
}
)";
        const unsigned int program = create_program(ibl_cube_vertex_shader, irradiance_fragment_shader);
        glx.use_program(program);
        glx.uniform_1i(glx.get_uniform_location(program, "uEnvMap"), 0);
        glx.active_texture(kTexture0);
        glBindTexture(kTextureCubeMap, env_cubemap);
        glViewport(0, 0, kIrradianceResolution, kIrradianceResolution);
        for (int face = 0; face < 6; ++face) {
            glx.uniform_matrix_4fv(glx.get_uniform_location(program, "uMvp"), 1, kFalse, capture_view_projections[static_cast<std::size_t>(face)].m.data());
            glx.framebuffer_texture_2d(kFramebuffer, kColorAttachment0, kTextureCubeMapPositiveX + static_cast<unsigned int>(face), irradiance_cubemap, 0);
            if (face == 0 && glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
                throw std::runtime_error("irradiance cubemap framebuffer is incomplete");
            }
            glClear(GL_COLOR_BUFFER_BIT);
            draw_ibl_cube_geometry();
        }
        glBindTexture(kTextureCubeMap, 0);
        glx.use_program(0);
        glx.delete_program(program);
    }

    // Prefiltered specular mip chain: GGX importance-sampled convolution of
    // the environment per roughness level, mip 0 = mirror-sharp (roughness
    // 0) down to mip (levels-1) = roughness 1. Sampled later via
    // textureCubeLod(uPrefilteredMap, R, roughness * (levels-1)).
    //
    // Filtered importance sampling (Karis, "Real Shading in Unreal Engine
    // 4"): a plain importance-sampled convolution that always reads the
    // full-detail 512x512 env_cubemap for every sample needs hundreds of
    // samples per texel to avoid visible per-texel noise once magnified
    // onto a large surface — with only 64 samples of a busy real-world
    // photo, that noise reads as a "faceted"/blocky reflection, worse at
    // higher roughness where the GGX lobe is wide (samples land almost at
    // random over a huge solid angle). Each sample's PDF says how much
    // solid angle it represents; comparing that to one texel's solid angle
    // gives a mip level to read env_cubemap at instead of mip 0 — wide-lobe
    // (low-PDF, high-roughness) samples land on a small, already-blurred
    // mip, so a handful of samples average out to a smooth result for
    // free, without changing SAMPLE_COUNT at all.
    {
        constexpr const char* prefilter_fragment_shader = R"(
#version 120
#extension GL_ARB_shader_texture_lod : enable
varying vec3 vLocalPos;
uniform samplerCube uEnvMap;
uniform float uRoughness;
uniform float uEnvResolution;
const float PI = 3.14159265359;

float radical_inverse_base2(int bits) {
    float result = 0.0;
    float denom = 1.0;
    for (int i = 0; i < 16; ++i) {
        denom *= 2.0;
        float bit = mod(float(bits), 2.0);
        bits = int(float(bits) / 2.0);
        result += bit / denom;
    }
    return result;
}

vec2 hammersley(int i, int n) {
    return vec2(float(i) / float(n), radical_inverse_base2(i));
}

vec3 importance_sample_ggx(vec2 xi, vec3 n, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 h;
    h.x = cos(phi) * sinTheta;
    h.y = sin(phi) * sinTheta;
    h.z = cosTheta;
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * h.x + bitangent * h.y + n * h.z);
}

float distribution_ggx(float ndoth, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth2 = ndoth * ndoth;
    float denom = (ndoth2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0001);
}

void main() {
    vec3 n = normalize(vLocalPos);
    vec3 r = n;
    vec3 v = r;

    // The mip-selection formula below (saSample = 1 / (SAMPLE_COUNT * pdf))
    // treats "how much solid angle one sample covers" as inversely
    // proportional to the sample count: too few samples makes each one look
    // like it covers far more solid angle than it really should, which
    // pushes the formula toward a much blurrier (lower-resolution) mip than
    // the actual roughness calls for — the plain importance-sampled version
    // of this shader got away with 64 samples because it never picked a
    // mip at all (always read full detail), but filtered importance
    // sampling needs a sample count large enough for that solid-angle
    // estimate to be meaningful. 1024 is the value this exact formula is
    // calibrated against in the reference implementation (Karis/Epic's UE4
    // course notes, and LearnOpenGL's IBL specular tutorial) — most of
    // those 1024 samples resolve to cheap low-resolution mip reads once
    // roughness is high, so this is still fast at bake time (once per
    // environment load), it just needed the sample count the formula
    // actually assumes.
    const int SAMPLE_COUNT = 1024;
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        vec2 xi = hammersley(i, SAMPLE_COUNT);
        vec3 h = importance_sample_ggx(xi, n, uRoughness);
        vec3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = max(dot(n, l), 0.0);
        if (ndotl > 0.0) {
#ifdef GL_ARB_shader_texture_lod
            float ndoth = max(dot(n, h), 0.0);
            float hdotv = max(dot(h, v), 0.0);
            float d = distribution_ggx(ndoth, uRoughness);
            float pdf = max(d * ndoth / max(4.0 * hdotv, 0.0001), 0.0001);
            float saTexel = 4.0 * PI / (6.0 * uEnvResolution * uEnvResolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf);
            float mipLevel = uRoughness <= 0.0 ? 0.0 : max(0.5 * log2(saSample / saTexel), 0.0);
            prefilteredColor += textureCubeLod(uEnvMap, l, mipLevel).rgb * ndotl;
#else
            prefilteredColor += textureCube(uEnvMap, l).rgb * ndotl;
#endif
            totalWeight += ndotl;
        }
    }
    prefilteredColor = totalWeight > 0.0 ? prefilteredColor / totalWeight : vec3(0.0);
    gl_FragColor = vec4(prefilteredColor, 1.0);
}
)";
        const unsigned int program = create_program(ibl_cube_vertex_shader, prefilter_fragment_shader);
        glx.use_program(program);
        glx.uniform_1i(glx.get_uniform_location(program, "uEnvMap"), 0);
        glx.uniform_1f(glx.get_uniform_location(program, "uEnvResolution"), static_cast<float>(kEnvResolution));
        glx.active_texture(kTexture0);
        glBindTexture(kTextureCubeMap, env_cubemap);
        for (int mip = 0; mip < kPrefilterMipLevels; ++mip) {
            const int mip_size = std::max(1, kPrefilterResolution >> mip);
            glViewport(0, 0, mip_size, mip_size);
            const float roughness = kPrefilterMipLevels > 1 ? static_cast<float>(mip) / static_cast<float>(kPrefilterMipLevels - 1) : 0.0f;
            glx.uniform_1f(glx.get_uniform_location(program, "uRoughness"), roughness);
            for (int face = 0; face < 6; ++face) {
                glx.uniform_matrix_4fv(glx.get_uniform_location(program, "uMvp"), 1, kFalse, capture_view_projections[static_cast<std::size_t>(face)].m.data());
                glx.framebuffer_texture_2d(kFramebuffer, kColorAttachment0, kTextureCubeMapPositiveX + static_cast<unsigned int>(face), prefiltered_cubemap, mip);
                if (mip == 0 && face == 0 && glx.check_framebuffer_status(kFramebuffer) != kFramebufferComplete) {
                    throw std::runtime_error("prefiltered specular cubemap framebuffer is incomplete");
                }
                glClear(GL_COLOR_BUFFER_BIT);
                draw_ibl_cube_geometry();
            }
        }
        glBindTexture(kTextureCubeMap, 0);
        glx.use_program(0);
        glx.delete_program(program);
    }

    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
    glx.delete_framebuffers(1, &bake_framebuffer);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, width_, height_);

    // All bake passes succeeded — safe to replace whatever was loaded before.
    destroy_ibl_resources();
    ibl_env_cubemap_ = env_cubemap;
    ibl_irradiance_cubemap_ = irradiance_cubemap;
    ibl_prefiltered_cubemap_ = prefiltered_cubemap;
    ibl_prefiltered_mip_count_ = kPrefilterMipLevels;
    ibl_ready_ = true;
}

void Renderer::load_environment(const std::string& hdr_path) {
    const HdrImage hdr = load_radiance_hdr(hdr_path);
    bake_ibl_environment(hdr);
    ibl_environment_path_ = hdr_path;
}

void Renderer::draw_skybox(const RenderFrameContext& frame) const {
    if (skybox_program_ == 0 || !ibl_ready_ || ibl_env_cubemap_ == 0) {
        return;
    }

    // Camera at the origin looking the same direction as the real camera:
    // the cube always exactly surrounds it, so it fills the whole viewport
    // regardless of the camera's actual world position (see
    // ENGINE_ARCHITECTURE.md). Drawn with depth test/write off, before any
    // other geometry, so it never needs to compete for depth with the scene.
    const Mat4 rotation_only_view = Mat4::look_at({0.0f, 0.0f, 0.0f}, frame.camera.forward(), {0.0f, 1.0f, 0.0f});
    const Mat4 view_projection = frame.projection * rotation_only_view;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glx.use_program(skybox_program_);
    glx.uniform_matrix_4fv(glx.get_uniform_location(skybox_program_, "uViewProjection"), 1, kFalse, view_projection.m.data());
    glx.uniform_1i(glx.get_uniform_location(skybox_program_, "uEnvMap"), 0);
    glx.uniform_1i(glx.get_uniform_location(skybox_program_, "uHdrOutputEnabled"), frame.use_hdr ? 1 : 0);
    glx.uniform_1f(glx.get_uniform_location(skybox_program_, "uExposure"), frame.engine.render_settings().exposure);
    glx.uniform_1f(glx.get_uniform_location(skybox_program_, "uSkyIntensity"), frame.engine.render_settings().sky_intensity);
    glx.active_texture(kTexture0);
    glBindTexture(kTextureCubeMap, ibl_env_cubemap_);
    draw_ibl_cube_geometry();
    glBindTexture(kTextureCubeMap, 0);
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
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uBaseColor"), material.base_color.x, material.base_color.y, material.base_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uBaseColorAlpha"), material.base_color_alpha);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uRoughness"), material.roughness);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uMetallic"), material.metallic);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uNormalStrength"), material.normal_strength);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uEmissiveFactor"), material.emissive_factor.x, material.emissive_factor.y, material.emissive_factor.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uOcclusionStrength"), material.occlusion_strength);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uAlphaMode"), static_cast<int>(material.alpha_mode));
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uAlphaCutoff"), material.alpha_cutoff);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uCameraPosition"), uniforms.camera_position.x, uniforms.camera_position.y, uniforms.camera_position.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uCameraForward"), uniforms.camera_forward.x, uniforms.camera_forward.y, uniforms.camera_forward.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uPointPosition"), uniforms.point_position.x, uniforms.point_position.y, uniforms.point_position.z);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uDebugView"), uniforms.debug_view);
    glx.uniform_1i(light_count_uniform_location_, uniforms.light_count);
    for (int i = 0; i < uniforms.light_count; ++i) {
        const GpuLight& light = uniforms.lights[static_cast<std::size_t>(i)];
        const LightUniformLocations& location = light_uniform_locations_[static_cast<std::size_t>(i)];
        glx.uniform_1i(location.type, light.type);
        glx.uniform_3f(location.position, light.position.x, light.position.y, light.position.z);
        glx.uniform_3f(location.direction, light.direction.x, light.direction.y, light.direction.z);
        glx.uniform_3f(location.color, light.color.x, light.color.y, light.color.z);
        glx.uniform_1f(location.intensity, light.intensity);
        glx.uniform_1f(location.range, light.range);
        glx.uniform_1f(location.inner_cos, light.inner_cos);
        glx.uniform_1f(location.outer_cos, light.outer_cos);
        glx.uniform_1i(location.casts_shadow, light.casts_shadow ? 1 : 0);
    }
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uBaseColorTexture"), 0);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasBaseColorTexture"), material.base_color_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uMetallicRoughnessTexture"), 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasMetallicRoughnessTexture"), material.metallic_roughness_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uNormalTexture"), 2);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasNormalTexture"), material.normal_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uEmissiveTexture"), 10);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasEmissiveTexture"), material.emissive_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uOcclusionTexture"), 11);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uHasOcclusionTexture"), material.occlusion_texture_path.empty() ? 0 : 1);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uCascadeShadowMap0"), 15);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uCascadeShadowMap1"), 16);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uCascadeShadowMap2"), 17);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uCascadeShadowMap3"), 18);
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uCascadeViewProjection0"), 1, kFalse, uniforms.cascade_view_projections[0].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uCascadeViewProjection1"), 1, kFalse, uniforms.cascade_view_projections[1].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uCascadeViewProjection2"), 1, kFalse, uniforms.cascade_view_projections[2].m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uCascadeViewProjection3"), 1, kFalse, uniforms.cascade_view_projections[3].m.data());
    glx.uniform_4f(glx.get_uniform_location(object_program_, "uCascadeSplitDepths"),
        uniforms.cascade_split_depths[0], uniforms.cascade_split_depths[1],
        uniforms.cascade_split_depths[2], uniforms.cascade_split_depths[3]);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uShadowsEnabled"), uniforms.shadows_enabled ? 1 : 0);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uSpotShadowMap"), 19);
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uSpotLightViewProjection"), 1, kFalse, uniforms.spot_light_view_projection.m.data());
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uSpotShadowsEnabled"), uniforms.spot_shadows_enabled ? 1 : 0);
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
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uIrradianceMap"), 12);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uPrefilteredMap"), 13);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uBrdfLut"), 14);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uIblEnabled"), uniforms.ibl_enabled ? 1 : 0);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uPrefilteredMipCount"), static_cast<float>(std::max(1, ibl_prefiltered_mip_count_)));
    // Unit 3 was the single directional shadow map before milestone 5's
    // cascades moved it to units 15-18; reused here for SSAO rather than
    // creeping the max unit count higher.
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uSsaoTexture"), 3);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uSsaoEnabled"), uniforms.ao_enabled ? 1 : 0);
    glx.uniform_2f(glx.get_uniform_location(object_program_, "uViewportSize"), static_cast<float>(std::max(1, width_)), static_cast<float>(std::max(1, height_)));
    glx.active_texture(kTexture0);
    glBindTexture(kTexture2D, resolve_base_color_texture(material));
    glx.active_texture(kTexture1);
    glBindTexture(kTexture2D, resolve_metallic_roughness_texture(material));
    glx.active_texture(kTexture2);
    glBindTexture(kTexture2D, resolve_normal_texture(material));
    glx.active_texture(kTexture3);
    const RenderTarget* ssao_target = find_render_target("SceneAO");
    glBindTexture(kTexture2D, ssao_target != nullptr ? ssao_target->color_texture : 0);
    const std::array<unsigned int, 6> point_texture_units{kTexture4, kTexture5, kTexture6, kTexture7, kTexture8, kTexture9};
    for (std::size_t index = 0; index < point_shadow_depth_textures_.size(); ++index) {
        glx.active_texture(point_texture_units[index]);
        glBindTexture(kTexture2D, point_shadow_depth_textures_[index]);
    }
    glx.active_texture(kTexture10);
    glBindTexture(kTexture2D, resolve_emissive_texture(material));
    glx.active_texture(kTexture11);
    glBindTexture(kTexture2D, resolve_occlusion_texture(material));
    glx.active_texture(kTexture12);
    glBindTexture(kTextureCubeMap, ibl_irradiance_cubemap_);
    glx.active_texture(kTexture13);
    glBindTexture(kTextureCubeMap, ibl_prefiltered_cubemap_);
    glx.active_texture(kTexture14);
    glBindTexture(kTexture2D, ibl_brdf_lut_);
    const std::array<unsigned int, kCascadeCount> cascade_texture_units{kTexture15, kTexture16, kTexture17, kTexture18};
    for (int cascade = 0; cascade < kCascadeCount; ++cascade) {
        glx.active_texture(cascade_texture_units[static_cast<std::size_t>(cascade)]);
        const RenderTarget* cascade_target = find_render_target("ShadowMapCascade" + std::to_string(cascade));
        glBindTexture(kTexture2D, cascade_target != nullptr ? cascade_target->depth_texture : 0);
    }
    glx.active_texture(kTexture19);
    const RenderTarget* spot_shadow_map_target = find_render_target("SpotShadowMap");
    glBindTexture(kTexture2D, spot_shadow_map_target != nullptr ? spot_shadow_map_target->depth_texture : 0);
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
    glx.active_texture(kTexture14);
    glBindTexture(kTexture2D, 0);
    glx.active_texture(kTexture13);
    glBindTexture(kTextureCubeMap, 0);
    glx.active_texture(kTexture12);
    glBindTexture(kTextureCubeMap, 0);
    glx.active_texture(kTexture11);
    glBindTexture(kTexture2D, 0);
    glx.active_texture(kTexture19);
    glBindTexture(kTexture2D, 0);
    for (std::size_t index = 0; index < cascade_texture_units.size(); ++index) {
        glx.active_texture(cascade_texture_units[index]);
        glBindTexture(kTexture2D, 0);
    }
    glx.active_texture(kTexture10);
    glBindTexture(kTexture2D, 0);
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

void Renderer::render_cascaded_shadow_maps(const Scene& scene, const ObjectUniforms& uniforms) {
    if (!uniforms.shadows_enabled) {
        return;
    }

    for (int cascade = 0; cascade < kCascadeCount; ++cascade) {
        const RenderTarget* cascade_target = find_render_target("ShadowMapCascade" + std::to_string(cascade));
        if (cascade_target == nullptr) {
            continue;
        }

        glx.bind_framebuffer(kFramebuffer, cascade_target->framebuffer);
        glViewport(0, 0, cascade_target->width, cascade_target->height);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glCullFace(GL_FRONT);

        for (const auto& actor : scene.actors()) {
            draw_depth_actor(*actor, uniforms.cascade_view_projections[static_cast<std::size_t>(cascade)]);
        }

        glCullFace(GL_BACK);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    glx.bind_framebuffer(kFramebuffer, 0);
    glDrawBuffer(kBack);
    glReadBuffer(kBack);
}

void Renderer::render_spot_shadow_map(const Scene& scene, const ObjectUniforms& uniforms) {
    if (!uniforms.spot_shadows_enabled) {
        return;
    }

    const RenderTarget* spot_shadow_map_target = find_render_target("SpotShadowMap");
    if (spot_shadow_map_target == nullptr) {
        return;
    }

    glx.bind_framebuffer(kFramebuffer, spot_shadow_map_target->framebuffer);
    glViewport(0, 0, spot_shadow_map_target->width, spot_shadow_map_target->height);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);

    for (const auto& actor : scene.actors()) {
        draw_depth_actor(*actor, uniforms.spot_light_view_projection);
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
    // Blended (translucent) surfaces don't cast shadows yet: a real
    // implementation would alpha-test against the base color texture in the
    // depth shader, which the current depth pass doesn't sample textures
    // for. Skipping keeps glass/foliage-style materials from casting solid
    // shadow boxes, at the cost of them casting no shadow at all for now.
    if (part.material.alpha_mode == AlphaMode::Blend) {
        return;
    }

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

// SSAO normal+depth prepass draw trio — structurally identical to the
// draw_depth_actor/draw_depth_mesh_part/draw_depth_gpu_mesh trio above
// (same per-actor/per-part traversal, same opaque-only scoping), just
// binding aNormal in addition to aPosition and targeting
// normal_depth_program_ instead of depth_program_. Unlike
// draw_depth_gpu_mesh, this doesn't call use_program itself — the caller
// (render_normal_depth_prepass) binds the program once and sets the
// per-frame camera uniforms before looping over actors, since those don't
// change per-mesh.
void Renderer::draw_normal_depth_actor(const Actor& actor, const Mat4& view_projection) {
    const auto* mesh = actor.find_component<StaticMeshComponent>();
    if (mesh == nullptr || !mesh->visible) {
        return;
    }
    for (const auto& part : mesh->parts) {
        draw_normal_depth_mesh_part(part, actor.root().local_transform, view_projection);
    }
}

void Renderer::draw_normal_depth_mesh_part(const MeshPart& part, const Transform& transform, const Mat4& view_projection) {
    // AO from opaque geometry only, same scoping as shadow casting above.
    if (part.material.alpha_mode == AlphaMode::Blend) {
        return;
    }

    switch (part.primitive) {
    case MeshPrimitive::Cube:
        draw_normal_depth_gpu_mesh(cube_mesh_, transform, view_projection);
        break;
    case MeshPrimitive::Plane:
        draw_normal_depth_gpu_mesh(plane_mesh_, transform, view_projection);
        break;
    case MeshPrimitive::Sphere:
        draw_normal_depth_gpu_mesh(sphere_mesh_, transform, view_projection);
        break;
    case MeshPrimitive::Custom: {
        auto it = custom_mesh_cache_.find(&part);
        if (it == custom_mesh_cache_.end()) {
            it = custom_mesh_cache_.emplace(&part, create_custom_mesh(part)).first;
        }
        draw_normal_depth_gpu_mesh(it->second, transform, view_projection);
        break;
    }
    }
}

void Renderer::draw_normal_depth_gpu_mesh(const GpuMesh& mesh, const Transform& transform, const Mat4& view_projection) const {
    if (normal_depth_program_ == 0 || mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

    const Mat4 model = transform.matrix();
    const Mat4 mvp = view_projection * model;
    glx.uniform_matrix_4fv(glx.get_uniform_location(normal_depth_program_, "uModel"), 1, kFalse, model.m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(normal_depth_program_, "uMvp"), 1, kFalse, mvp.m.data());

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
}

void Renderer::draw_gpu_mesh_outline(const GpuMesh& mesh, const Transform& transform, const Mat4& view_projection, float outline_width, bool radial_extrude) const {
    if (outline_program_ == 0 || mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

    const Mat4 model = transform.matrix();
    const Mat4 mvp = view_projection * model;

    glx.use_program(outline_program_);
    glx.uniform_matrix_4fv(glx.get_uniform_location(outline_program_, "uMvp"), 1, kFalse, mvp.m.data());
    glx.uniform_1f(glx.get_uniform_location(outline_program_, "uOutlineWidth"), outline_width);
    glx.uniform_1i(glx.get_uniform_location(outline_program_, "uRadialExtrude"), radial_extrude ? 1 : 0);
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
        // Radial (position-from-origin) extrusion — the built-in cube's
        // hard-edged corners need it to avoid cracking (see the comment on
        // outline_vertex_shader).
        draw_gpu_mesh_outline(cube_mesh_, transform, view_projection, outline_width, true);
        break;
    case MeshPrimitive::Sphere:
        // A unit sphere's vertex normal already equals its position
        // direction from the origin, so radial vs. normal-based extrusion
        // is identical here — true purely for consistency.
        draw_gpu_mesh_outline(sphere_mesh_, transform, view_projection, outline_width, true);
        break;
    case MeshPrimitive::Custom: {
        auto it = custom_mesh_cache_.find(&part);
        if (it == custom_mesh_cache_.end()) {
            it = custom_mesh_cache_.emplace(&part, create_custom_mesh(part)).first;
        }
        // Imported meshes aren't guaranteed to be centered on their local
        // origin, so radial extrusion would pull vertices in directions
        // unrelated to their actual surface normal — keep the
        // geometrically correct normal-based extrusion for these.
        draw_gpu_mesh_outline(it->second, transform, view_projection, outline_width, false);
        break;
    }
    case MeshPrimitive::Plane:
        break;
    }
}

void Renderer::draw_actor(const Actor& actor, bool selected, const Mat4& view_projection, const ObjectUniforms& uniforms, bool blend_only) {
    if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
        if (mesh->visible) {
            for (const auto& part : mesh->parts) {
                if ((part.material.alpha_mode == AlphaMode::Blend) != blend_only) {
                    continue;
                }
                draw_mesh_part(part, actor.root().local_transform, view_projection, uniforms);
            }
        }
    }
    if (!blend_only) {
        if (const auto* light = actor.find_component<LightComponent>()) {
            draw_light_icon(actor, *light, selected);
        }
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
    // A single cohesive coral/emerald/azure triad shared by every tool
    // (translate arrows, rotate rings, scale handles) so the X/Y/Z color
    // mapping stays consistent across the whole editor — still red-ish/
    // green-ish/blue-ish, preserving the universal X/Y/Z convention, but
    // matched in saturation and value as a set rather than the previous
    // slightly muddy, mismatched shades.
    constexpr Vec3 kAxisColorX{0.90f, 0.30f, 0.33f};
    constexpr Vec3 kAxisColorY{0.20f, 0.78f, 0.53f};
    constexpr Vec3 kAxisColorZ{0.25f, 0.55f, 0.92f};

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
        ring(y_axis, z_axis, kAxisColorX);
        ring(x_axis, z_axis, kAxisColorY);
        ring(x_axis, y_axis, kAxisColorZ);
        glLineWidth(1.0f);
        glEnable(GL_CULL_FACE);
    } else {
        const Vec3 axes[] = {x_axis, y_axis, z_axis};
        const Vec3 colors[] = {kAxisColorX, kAxisColorY, kAxisColorZ};

        if (tool == EditorTool::Move) {
            // Arrowheads: a shaft up to the cone's base, then a solid cone
            // — the conventional translate-gizmo look (Maya/Blender/UE),
            // and visually distinct from Scale's cube handles below so the
            // two tools read apart from each other at a glance.
            constexpr float cone_height = 0.24f;
            constexpr float cone_radius = 0.085f;
            glDisable(GL_CULL_FACE);
            for (int i = 0; i < 3; ++i) {
                const Vec3 tip = origin + axes[i] * scale;
                const Vec3 cone_base = tip - axes[i] * cone_height;
                draw_line(origin, cone_base, colors[i], 5.0f);
                Material handle;
                handle.base_color = colors[i];
                draw_cone(cone_base, axes[i], cone_radius, cone_height, handle);
            }
            glEnable(GL_CULL_FACE);
        } else {
            const Vec3 endpoints[] = {origin + x_axis * scale, origin + y_axis * scale, origin + z_axis * scale};
            for (int i = 0; i < 3; ++i) {
                draw_line(origin, endpoints[i], colors[i], 5.0f);
            }
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
    }

    glPushMatrix();
    glTranslatef(origin.x, origin.y, origin.z);
    glScalef(0.14f, 0.14f, 0.14f);
    Material center;
    center.base_color = {0.72f, 0.66f, 0.28f};
    draw_cube(center);
    glPopMatrix();
}

} // namespace Hollow

#include "Forge/Renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
constexpr unsigned int kVertexShader = 0x8B31;
constexpr unsigned int kFragmentShader = 0x8B30;
constexpr unsigned int kCompileStatus = 0x8B81;
constexpr unsigned int kLinkStatus = 0x8B82;
constexpr unsigned int kInfoLogLength = 0x8B84;

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
            && gen_buffers != nullptr
            && bind_buffer != nullptr
            && buffer_data != nullptr
            && delete_buffers != nullptr
            && enable_vertex_attrib_array != nullptr
            && disable_vertex_attrib_array != nullptr
            && vertex_attrib_pointer != nullptr;
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
    auto v = [](Vec3 p, Vec3 n) {
        return RenderVertex{p, n, {1.0f, 1.0f, 1.0f}};
    };
    return {
        v({-0.5f, -0.5f, 0.5f}, {0, 0, 1}), v({0.5f, -0.5f, 0.5f}, {0, 0, 1}), v({0.5f, 0.5f, 0.5f}, {0, 0, 1}), v({-0.5f, 0.5f, 0.5f}, {0, 0, 1}),
        v({0.5f, -0.5f, -0.5f}, {0, 0, -1}), v({-0.5f, -0.5f, -0.5f}, {0, 0, -1}), v({-0.5f, 0.5f, -0.5f}, {0, 0, -1}), v({0.5f, 0.5f, -0.5f}, {0, 0, -1}),
        v({-0.5f, -0.5f, -0.5f}, {-1, 0, 0}), v({-0.5f, -0.5f, 0.5f}, {-1, 0, 0}), v({-0.5f, 0.5f, 0.5f}, {-1, 0, 0}), v({-0.5f, 0.5f, -0.5f}, {-1, 0, 0}),
        v({0.5f, -0.5f, 0.5f}, {1, 0, 0}), v({0.5f, -0.5f, -0.5f}, {1, 0, 0}), v({0.5f, 0.5f, -0.5f}, {1, 0, 0}), v({0.5f, 0.5f, 0.5f}, {1, 0, 0}),
        v({-0.5f, 0.5f, 0.5f}, {0, 1, 0}), v({0.5f, 0.5f, 0.5f}, {0, 1, 0}), v({0.5f, 0.5f, -0.5f}, {0, 1, 0}), v({-0.5f, 0.5f, -0.5f}, {0, 1, 0}),
        v({-0.5f, -0.5f, -0.5f}, {0, -1, 0}), v({0.5f, -0.5f, -0.5f}, {0, -1, 0}), v({0.5f, -0.5f, 0.5f}, {0, -1, 0}), v({-0.5f, -0.5f, 0.5f}, {0, -1, 0}),
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
    glClearColor(0.08f, 0.10f, 0.09f, 1.0f);
    try {
        initialize_modern_pipeline();
    } catch (const std::exception&) {
        object_program_ = 0;
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
    if (object_program_ != 0 && glx.delete_program != nullptr) {
        glx.delete_program(object_program_);
        object_program_ = 0;
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
    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 projection = Mat4::perspective(camera.fov_y_radians, aspect, 0.05f, 500.0f);
    const Mat4 view = Mat4::look_at(camera.position(), camera.target, {0.0f, 1.0f, 0.0f});
    const Mat4 view_projection = projection * view;
    const ObjectUniforms object_uniforms = collect_object_uniforms(scene, camera, engine);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m.data());
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m.data());

    if (engine.render_settings().draw_world_grid) {
        draw_grid(camera);
    }

    for (const auto& actor : scene.actors()) {
        draw_actor(*actor, actor->id() == scene.selected_actor_id(), view_projection, object_uniforms);
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
    const Vec3 minor{0.13f, 0.29f, 0.27f};
    const Vec3 major{0.9f, 0.85f, 0.1f};
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
uniform mat4 uModel;
uniform mat4 uMvp;
varying vec3 vWorldPosition;
varying vec3 vNormal;
varying vec3 vColor;
void main() {
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vColor = aColor;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    constexpr const char* fragment_shader = R"(
#version 120
uniform vec3 uBaseColor;
uniform float uRoughness;
uniform float uMetallic;
uniform vec3 uCameraPosition;
uniform vec3 uDirectionalDirection;
uniform vec3 uDirectionalColor;
uniform float uDirectionalIntensity;
uniform vec3 uPointPosition;
uniform vec3 uPointColor;
uniform float uPointIntensity;
uniform float uPointRange;
uniform int uDebugView;
varying vec3 vWorldPosition;
varying vec3 vNormal;
varying vec3 vColor;

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

void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uCameraPosition - vWorldPosition);
    vec3 albedo = clamp(uBaseColor * vColor, 0.0, 1.0);
    float roughness = clamp(uRoughness, 0.04, 1.0);
    float metallic = clamp(uMetallic, 0.0, 1.0);

    vec3 direct = vec3(0.0);
    direct += pbr_light(albedo, metallic, roughness, n, v, normalize(uDirectionalDirection), uDirectionalColor * uDirectionalIntensity);

    vec3 pointVector = uPointPosition - vWorldPosition;
    float pointDistance = length(pointVector);
    if (uPointIntensity > 0.0 && pointDistance < uPointRange) {
        vec3 pointL = pointVector / max(pointDistance, 0.0001);
        float attenuation = pow(clamp(1.0 - pointDistance / max(uPointRange, 0.0001), 0.0, 1.0), 2.0);
        direct += pbr_light(albedo, metallic, roughness, n, v, pointL, uPointColor * uPointIntensity * attenuation);
    }

    vec3 ambient = albedo * mix(0.035, 0.075, 1.0 - roughness);
    vec3 finalColor = ambient + direct;
    finalColor = finalColor / (finalColor + vec3(1.0));
    finalColor = pow(finalColor, vec3(1.0 / 2.2));

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
        gl_FragColor = vec4(finalColor, 1.0);
    }
}
)";

    object_program_ = create_program(vertex_shader, fragment_shader);
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
        vertices.push_back({vertex.position, vertex.normal, vertex.color});
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

void Renderer::draw_gpu_mesh(const GpuMesh& mesh, const Material& material, const Transform& transform, const Mat4& view_projection, const ObjectUniforms& uniforms) const {
    if (object_program_ == 0 || mesh.vertex_buffer == 0 || mesh.index_buffer == 0 || mesh.index_count <= 0) {
        return;
    }

    const Mat4 model = transform.matrix();
    const Mat4 mvp = view_projection * model;
    glx.use_program(object_program_);
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uModel"), 1, kFalse, model.m.data());
    glx.uniform_matrix_4fv(glx.get_uniform_location(object_program_, "uMvp"), 1, kFalse, mvp.m.data());
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uBaseColor"), material.base_color.x, material.base_color.y, material.base_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uRoughness"), material.roughness);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uMetallic"), material.metallic);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uCameraPosition"), uniforms.camera_position.x, uniforms.camera_position.y, uniforms.camera_position.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uDirectionalDirection"), uniforms.directional_direction.x, uniforms.directional_direction.y, uniforms.directional_direction.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uDirectionalColor"), uniforms.directional_color.x, uniforms.directional_color.y, uniforms.directional_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uDirectionalIntensity"), uniforms.directional_intensity);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uPointPosition"), uniforms.point_position.x, uniforms.point_position.y, uniforms.point_position.z);
    glx.uniform_3f(glx.get_uniform_location(object_program_, "uPointColor"), uniforms.point_color.x, uniforms.point_color.y, uniforms.point_color.z);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uPointIntensity"), uniforms.point_intensity);
    glx.uniform_1f(glx.get_uniform_location(object_program_, "uPointRange"), uniforms.point_range);
    glx.uniform_1i(glx.get_uniform_location(object_program_, "uDebugView"), uniforms.debug_view);

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
    }
    glx.bind_buffer(kElementArrayBuffer, mesh.index_buffer);
    glDrawElements(kTriangles, mesh.index_count, kUnsignedInt, nullptr);

    if (mesh.vertex_array != 0 && glx.bind_vertex_array != nullptr) {
        glx.bind_vertex_array(0);
    } else {
        glx.disable_vertex_attrib_array(0);
        glx.disable_vertex_attrib_array(1);
        glx.disable_vertex_attrib_array(2);
    }
    glx.bind_buffer(kArrayBuffer, 0);
    glx.bind_buffer(kElementArrayBuffer, 0);
    glx.use_program(0);
}

GpuMesh Renderer::create_cube_mesh() const {
    return create_gpu_mesh(cube_vertices(), quad_face_indices());
}

GpuMesh Renderer::create_plane_mesh() const {
    std::vector<RenderVertex> vertices{
        {{-0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 1, 1}},
        {{0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 1, 1}},
        {{0.5f, 0.0f, 0.5f}, {0, 1, 0}, {1, 1, 1}},
        {{-0.5f, 0.0f, 0.5f}, {0, 1, 0}, {1, 1, 1}},
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
            vertices.push_back({normal * 0.5f, normal, {1, 1, 1}});
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
    if (selected && actor.find_component<StaticMeshComponent>()) {
        glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(2.0f);
        set_color({0.02f, 0.02f, 0.02f});
        glPushMatrix();
        const Mat4 model = actor.root().local_transform.matrix();
        glMultMatrixf(model.m.data());
        Material outline;
        outline.base_color = {0.02f, 0.02f, 0.02f};
        draw_cube(outline);
        glPopMatrix();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glLineWidth(1.0f);
        glEnable(GL_CULL_FACE);
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

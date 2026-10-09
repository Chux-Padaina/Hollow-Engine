// Vector/matrix/transform math used throughout Hollow Engine: Vec2/Vec3,
// column-major Mat4 (OpenGL layout), a TRS Transform, axis-aligned bounding
// boxes, rays, and view-frustum planes for culling. Free of engine
// dependencies so every module can use it.
#pragma once

#include <array>
#include <cmath>

namespace Hollow {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    static Vec3 zero();
    static Vec3 one();
};

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 value);
Vec3 operator*(Vec3 value, float scalar);
Vec3 operator*(float scalar, Vec3 value);
Vec3 operator/(Vec3 value, float scalar);
Vec3& operator+=(Vec3& a, Vec3 b);
Vec3& operator-=(Vec3& a, Vec3 b);
Vec3& operator*=(Vec3& value, float scalar);
bool operator==(Vec3 a, Vec3 b);
bool operator!=(Vec3 a, Vec3 b);

float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 value);
float length_squared(Vec3 value);
Vec3 normalize(Vec3 value);
Vec3 clamp(Vec3 value, float min_value, float max_value);
Vec3 component_min(Vec3 a, Vec3 b);
Vec3 component_max(Vec3 a, Vec3 b);
Vec3 component_mul(Vec3 a, Vec3 b);
Vec3 lerp(Vec3 a, Vec3 b, float t);
float max_component(Vec3 value);

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

Vec4 operator+(Vec4 a, Vec4 b);
Vec4 operator*(Vec4 value, float scalar);
float dot(Vec4 a, Vec4 b);

// Unit quaternion (x, y, z = vector part, w = scalar part). Used by the
// animation system; Transform itself stores Euler angles.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    static Quat identity() { return {}; }
    static Quat axis_angle(Vec3 axis, float radians);
    // Same convention as Mat4::rotation_xyz.
    static Quat from_euler_xyz(Vec3 radians);
};

Quat operator*(Quat a, Quat b);
Quat normalize_quat(Quat q);
float dot(Quat a, Quat b);
// Shortest-path spherical interpolation.
Quat slerp(Quat a, Quat b, float t);
// Normalized linear interpolation (fast; fine for small angles).
Quat nlerp(Quat a, Quat b, float t);
Vec3 rotate(Quat q, Vec3 v);

struct Mat4 {
    std::array<float, 16> m{};

    static Mat4 identity();
    static Mat4 translation(Vec3 value);
    static Mat4 scale(Vec3 value);
    static Mat4 rotation_xyz(Vec3 radians);
    static Mat4 rotation_axis_angle(Vec3 axis, float radians);
    static Mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z);
    static Mat4 orthographic(float left, float right, float bottom, float top, float near_z, float far_z);
    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up);
    static Mat4 rotation(Quat q);
    // GPU projections for the renderer's reversed-Z, [0,1]-depth convention
    // (glClipControl ZERO_TO_ONE): the near plane maps to depth 1 and the
    // far plane (or infinity) to 0, which spreads float depth precision
    // evenly. The CPU-side culling/picking code keeps using the
    // conventional perspective()/orthographic() above.
    static Mat4 perspective_reversed_z(float fov_y_radians, float aspect, float near_z);
    static Mat4 perspective_reversed_z(float fov_y_radians, float aspect, float near_z, float far_z);
    static Mat4 orthographic_reversed_z(float left, float right, float bottom, float top, float near_z, float far_z);
    // TRS composition with a quaternion rotation.
    static Mat4 trs(Vec3 translation, Quat rotation, Vec3 scale);

    Vec3 translation_part() const { return {m[12], m[13], m[14]}; }
};

Mat4 operator*(const Mat4& a, const Mat4& b);
// General 4x4 inverse. Returns identity for a singular matrix.
Mat4 inverse(const Mat4& value);
Mat4 transpose(const Mat4& value);
// Inverse-transpose of the upper 3x3, padded back to a 4x4: the matrix that
// correctly transforms normals under non-uniform scale.
Mat4 normal_matrix(const Mat4& model);
Vec3 transform_point(const Mat4& matrix, Vec3 point);
Vec3 transform_vector(const Mat4& matrix, Vec3 vector);
Vec4 operator*(const Mat4& matrix, Vec4 vector);
// Rotation part of a TRS matrix (scale removed).
Quat quat_from_matrix(const Mat4& matrix);
Vec3 euler_from_rotation_xyz(const Mat4& rotation);

// Engine-wide orientation conventions. An actor "looks" down its local -Z
// axis (cameras, spot/directional lights use their own convention, see
// Renderer). These convert between that forward vector and the XYZ Euler
// rotation Transform stores.
Vec3 forward_from_rotation(Vec3 rotation_radians);
Vec3 rotation_for_forward(Vec3 forward);

struct Transform {
    Vec3 translation = Vec3::zero();
    Vec3 rotation = Vec3::zero(); // XYZ Euler angles, radians
    Vec3 scale = Vec3::one();

    Mat4 matrix() const;
};

// Splits a TRS matrix (no shear) back into translation/rotation/scale.
Transform decompose(const Mat4& matrix);

struct Ray {
    Vec3 origin;
    Vec3 direction; // normalized
};

struct Aabb {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extents() const { return (max - min) * 0.5f; }
    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    void expand(Vec3 point);
    void expand(const Aabb& other);
    static Aabb empty();
};

// Axis-aligned box that encloses `box` after transforming it by `matrix`.
Aabb transform_aabb(const Aabb& box, const Mat4& matrix);
// Slab test. Returns the entry distance (or 0 when the origin is inside),
// or a negative value on a miss.
float ray_aabb(const Ray& ray, const Aabb& box);
float ray_sphere(const Ray& ray, Vec3 center, float radius);
// Moller-Trumbore. Returns the hit distance or a negative value on a miss.
// Two-sided.
float ray_triangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c);

struct Plane {
    Vec3 normal;
    float distance = 0.0f; // dot(normal, p) + distance >= 0 is "inside"
};

struct Frustum {
    std::array<Plane, 6> planes{};

    // Gribb/Hartmann plane extraction from a column-major view-projection
    // matrix. Works for both perspective and orthographic projections.
    static Frustum from_matrix(const Mat4& view_projection);
    bool intersects_sphere(Vec3 center, float radius) const;
};

} // namespace Hollow

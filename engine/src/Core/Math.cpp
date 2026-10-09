// Implementation of the math declared in Math.hpp. Matrices are stored column-major (matching OpenGL's expected
// layout), so translation lives in m[12..14] and matrix multiplication
// combines column/row indices accordingly — see operator* below.

#include "Hollow/Core/Math.hpp"

#include <algorithm>

namespace Hollow {

Vec3 Vec3::zero() { return {}; }

Vec3 Vec3::one() { return {1.0f, 1.0f, 1.0f}; }

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 operator*(Vec3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

Vec3 operator/(Vec3 value, float scalar) {
    return {value.x / scalar, value.y / scalar, value.z / scalar};
}

Vec3 operator-(Vec3 value) { return {-value.x, -value.y, -value.z}; }

Vec3 operator*(float scalar, Vec3 value) { return value * scalar; }

bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

bool operator!=(Vec3 a, Vec3 b) { return !(a == b); }

Vec3& operator+=(Vec3& a, Vec3 b) {
    a = a + b;
    return a;
}

Vec3& operator-=(Vec3& a, Vec3 b) {
    a = a - b;
    return a;
}

Vec3& operator*=(Vec3& value, float scalar) {
    value = value * scalar;
    return value;
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float length_squared(Vec3 value) {
    return dot(value, value);
}

Vec3 component_min(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }

Vec3 component_max(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

Vec3 component_mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

float max_component(Vec3 value) { return std::max({value.x, value.y, value.z}); }

float length(Vec3 value) {
    return std::sqrt(dot(value, value));
}

Vec3 normalize(Vec3 value) {
    const float len = length(value);
    if (len <= 0.00001f) {
        return {};
    }
    return value / len;
}

Vec3 clamp(Vec3 value, float min_value, float max_value) {
    return {
        std::clamp(value.x, min_value, max_value),
        std::clamp(value.y, min_value, max_value),
        std::clamp(value.z, min_value, max_value),
    };
}

Mat4 Mat4::identity() {
    Mat4 out;
    out.m = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    return out;
}

Mat4 Mat4::translation(Vec3 value) {
    Mat4 out = identity();
    out.m[12] = value.x;
    out.m[13] = value.y;
    out.m[14] = value.z;
    return out;
}

Mat4 Mat4::scale(Vec3 value) {
    Mat4 out = identity();
    out.m[0] = value.x;
    out.m[5] = value.y;
    out.m[10] = value.z;
    return out;
}

Mat4 Mat4::rotation_xyz(Vec3 radians) {
    const float cx = std::cos(radians.x);
    const float sx = std::sin(radians.x);
    const float cy = std::cos(radians.y);
    const float sy = std::sin(radians.y);
    const float cz = std::cos(radians.z);
    const float sz = std::sin(radians.z);

    Mat4 rx = identity();
    rx.m[5] = cx;
    rx.m[6] = sx;
    rx.m[9] = -sx;
    rx.m[10] = cx;

    Mat4 ry = identity();
    ry.m[0] = cy;
    ry.m[2] = -sy;
    ry.m[8] = sy;
    ry.m[10] = cy;

    Mat4 rz = identity();
    rz.m[0] = cz;
    rz.m[1] = sz;
    rz.m[4] = -sz;
    rz.m[5] = cz;

    return rz * ry * rx;
}

Mat4 Mat4::rotation_axis_angle(Vec3 axis, float radians) {
    axis = normalize(axis);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float t = 1.0f - c;

    Mat4 out = identity();
    out.m[0] = t * axis.x * axis.x + c;
    out.m[1] = t * axis.x * axis.y + s * axis.z;
    out.m[2] = t * axis.x * axis.z - s * axis.y;

    out.m[4] = t * axis.x * axis.y - s * axis.z;
    out.m[5] = t * axis.y * axis.y + c;
    out.m[6] = t * axis.y * axis.z + s * axis.x;

    out.m[8] = t * axis.x * axis.z + s * axis.y;
    out.m[9] = t * axis.y * axis.z - s * axis.x;
    out.m[10] = t * axis.z * axis.z + c;
    return out;
}

Mat4 Mat4::perspective(float fov_y_radians, float aspect, float near_z, float far_z) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 out{};
    out.m[0] = f / std::max(aspect, 0.0001f);
    out.m[5] = f;
    out.m[10] = (far_z + near_z) / (near_z - far_z);
    out.m[11] = -1.0f;
    out.m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    return out;
}

Mat4 Mat4::orthographic(float left, float right, float bottom, float top, float near_z, float far_z) {
    Mat4 out = identity();
    out.m[0] = 2.0f / std::max(right - left, 0.0001f);
    out.m[5] = 2.0f / std::max(top - bottom, 0.0001f);
    out.m[10] = -2.0f / std::max(far_z - near_z, 0.0001f);
    out.m[12] = -(right + left) / std::max(right - left, 0.0001f);
    out.m[13] = -(top + bottom) / std::max(top - bottom, 0.0001f);
    out.m[14] = -(far_z + near_z) / std::max(far_z - near_z, 0.0001f);
    return out;
}

Mat4 Mat4::look_at(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalize(target - eye);
    // When the view direction is (anti)parallel to `up` the cross product
    // degenerates to zero and the whole basis collapses (this used to break
    // cascaded shadows for a light pointing straight down). Fall back to a
    // different up axis in that case.
    Vec3 side = cross(f, up);
    if (length_squared(side) < 1e-8f) {
        side = cross(f, std::abs(f.z) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f});
    }
    const Vec3 s = normalize(side);
    const Vec3 u = cross(s, f);

    Mat4 out = identity();
    out.m[0] = s.x;
    out.m[4] = s.y;
    out.m[8] = s.z;
    out.m[1] = u.x;
    out.m[5] = u.y;
    out.m[9] = u.z;
    out.m[2] = -f.x;
    out.m[6] = -f.y;
    out.m[10] = -f.z;
    out.m[12] = -dot(s, eye);
    out.m[13] = -dot(u, eye);
    out.m[14] = dot(f, eye);
    return out;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                sum += a.m[i * 4 + row] * b.m[column * 4 + i];
            }
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

Vec3 euler_from_rotation_xyz(const Mat4& rotation) {
    const float r20 = rotation.m[2];
    Vec3 euler;
    euler.y = std::asin(std::clamp(-r20, -1.0f, 1.0f));

    const float cy = std::cos(euler.y);
    if (std::abs(cy) > 0.0001f) {
        euler.x = std::atan2(rotation.m[6], rotation.m[10]);
        euler.z = std::atan2(rotation.m[1], rotation.m[0]);
    } else {
        euler.x = 0.0f;
        euler.z = std::atan2(-rotation.m[4], rotation.m[5]);
    }
    return euler;
}

Mat4 Transform::matrix() const {
    return Mat4::translation(translation) * Mat4::rotation_xyz(rotation) * Mat4::scale(scale);
}

Mat4 inverse(const Mat4& value) {
    const auto& a = value.m;
    std::array<float, 16> inv{};
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::abs(det) < 1e-12f) {
        return Mat4::identity();
    }
    const float inv_det = 1.0f / det;
    Mat4 out;
    for (int i = 0; i < 16; ++i) {
        out.m[static_cast<std::size_t>(i)] = inv[static_cast<std::size_t>(i)] * inv_det;
    }
    return out;
}

Mat4 transpose(const Mat4& value) {
    Mat4 out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            out.m[static_cast<std::size_t>(row * 4 + column)] = value.m[static_cast<std::size_t>(column * 4 + row)];
        }
    }
    return out;
}

Mat4 normal_matrix(const Mat4& model) {
    Mat4 upper = model;
    upper.m[12] = upper.m[13] = upper.m[14] = 0.0f;
    upper.m[3] = upper.m[7] = upper.m[11] = 0.0f;
    upper.m[15] = 1.0f;
    return transpose(inverse(upper));
}

Vec3 transform_point(const Mat4& matrix, Vec3 p) {
    return {
        p.x * matrix.m[0] + p.y * matrix.m[4] + p.z * matrix.m[8] + matrix.m[12],
        p.x * matrix.m[1] + p.y * matrix.m[5] + p.z * matrix.m[9] + matrix.m[13],
        p.x * matrix.m[2] + p.y * matrix.m[6] + p.z * matrix.m[10] + matrix.m[14],
    };
}

Vec3 transform_vector(const Mat4& matrix, Vec3 v) {
    return {
        v.x * matrix.m[0] + v.y * matrix.m[4] + v.z * matrix.m[8],
        v.x * matrix.m[1] + v.y * matrix.m[5] + v.z * matrix.m[9],
        v.x * matrix.m[2] + v.y * matrix.m[6] + v.z * matrix.m[10],
    };
}

Vec3 forward_from_rotation(Vec3 rotation_radians) {
    return normalize(transform_vector(Mat4::rotation_xyz(rotation_radians), {0.0f, 0.0f, -1.0f}));
}

Vec3 rotation_for_forward(Vec3 forward) {
    // With R = Rz * Ry * Rx and z = 0, R * (0,0,-1) =
    // (-cos(x) sin(y), sin(x), -cos(x) cos(y)).
    const Vec3 f = normalize(forward);
    return {std::asin(std::clamp(f.y, -1.0f, 1.0f)), std::atan2(-f.x, -f.z), 0.0f};
}

Transform decompose(const Mat4& matrix) {
    Transform out;
    out.translation = matrix.translation_part();
    const Vec3 column_x{matrix.m[0], matrix.m[1], matrix.m[2]};
    const Vec3 column_y{matrix.m[4], matrix.m[5], matrix.m[6]};
    const Vec3 column_z{matrix.m[8], matrix.m[9], matrix.m[10]};
    out.scale = {length(column_x), length(column_y), length(column_z)};
    // A negative determinant means a mirror; fold it into X scale.
    if (dot(cross(column_x, column_y), column_z) < 0.0f) {
        out.scale.x = -out.scale.x;
    }
    Mat4 rotation = Mat4::identity();
    const Vec3 rx = out.scale.x != 0.0f ? column_x / out.scale.x : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 ry = out.scale.y != 0.0f ? column_y / out.scale.y : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 rz = out.scale.z != 0.0f ? column_z / out.scale.z : Vec3{0.0f, 0.0f, 1.0f};
    rotation.m[0] = rx.x;
    rotation.m[1] = rx.y;
    rotation.m[2] = rx.z;
    rotation.m[4] = ry.x;
    rotation.m[5] = ry.y;
    rotation.m[6] = ry.z;
    rotation.m[8] = rz.x;
    rotation.m[9] = rz.y;
    rotation.m[10] = rz.z;
    out.rotation = euler_from_rotation_xyz(rotation);
    return out;
}

void Aabb::expand(Vec3 point) {
    min = component_min(min, point);
    max = component_max(max, point);
}

void Aabb::expand(const Aabb& other) {
    if (!other.valid()) {
        return;
    }
    expand(other.min);
    expand(other.max);
}

Aabb Aabb::empty() {
    constexpr float big = 3.0e38f;
    return {{big, big, big}, {-big, -big, -big}};
}

Aabb transform_aabb(const Aabb& box, const Mat4& matrix) {
    // Arvo's method: transform the center, then project the extents onto
    // each world axis through the absolute rotation/scale matrix.
    const Vec3 center = transform_point(matrix, box.center());
    const Vec3 e = box.extents();
    const Vec3 world_extents{
        std::abs(matrix.m[0]) * e.x + std::abs(matrix.m[4]) * e.y + std::abs(matrix.m[8]) * e.z,
        std::abs(matrix.m[1]) * e.x + std::abs(matrix.m[5]) * e.y + std::abs(matrix.m[9]) * e.z,
        std::abs(matrix.m[2]) * e.x + std::abs(matrix.m[6]) * e.y + std::abs(matrix.m[10]) * e.z,
    };
    return {center - world_extents, center + world_extents};
}

float ray_aabb(const Ray& ray, const Aabb& box) {
    float t_min = 0.0f;
    float t_max = 3.0e38f;
    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-8f) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) {
                return -1.0f;
            }
            continue;
        }
        const float inv = 1.0f / direction[axis];
        float t0 = (lo[axis] - origin[axis]) * inv;
        float t1 = (hi[axis] - origin[axis]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_min > t_max) {
            return -1.0f;
        }
    }
    return t_min;
}

float ray_sphere(const Ray& ray, Vec3 center, float radius) {
    const Vec3 to_center = center - ray.origin;
    const float projection = dot(to_center, ray.direction);
    const float closest_sq = dot(to_center, to_center) - projection * projection;
    const float radius_sq = radius * radius;
    if (closest_sq > radius_sq) {
        return -1.0f;
    }
    const float half_chord = std::sqrt(radius_sq - closest_sq);
    const float near_hit = projection - half_chord;
    if (near_hit >= 0.0f) {
        return near_hit;
    }
    const float far_hit = projection + half_chord;
    return far_hit >= 0.0f ? 0.0f : -1.0f;
}

float ray_triangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 p = cross(ray.direction, edge2);
    const float det = dot(edge1, p);
    if (std::abs(det) < 1e-9f) {
        return -1.0f;
    }
    const float inv_det = 1.0f / det;
    const Vec3 t_vec = ray.origin - a;
    const float u = dot(t_vec, p) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }
    const Vec3 q = cross(t_vec, edge1);
    const float v = dot(ray.direction, q) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }
    const float t = dot(edge2, q) * inv_det;
    return t >= 0.0f ? t : -1.0f;
}

Frustum Frustum::from_matrix(const Mat4& vp) {
    const auto& m = vp.m;
    auto row = [&m](int r) {
        return std::array<float, 4>{m[static_cast<std::size_t>(r)], m[static_cast<std::size_t>(4 + r)], m[static_cast<std::size_t>(8 + r)], m[static_cast<std::size_t>(12 + r)]};
    };
    const auto r0 = row(0);
    const auto r1 = row(1);
    const auto r2 = row(2);
    const auto r3 = row(3);
    auto make = [](const std::array<float, 4>& a, const std::array<float, 4>& b, float sign) {
        Plane plane;
        plane.normal = {a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2]};
        plane.distance = a[3] + sign * b[3];
        const float len = length(plane.normal);
        if (len > 1e-8f) {
            plane.normal = plane.normal / len;
            plane.distance /= len;
        }
        return plane;
    };
    Frustum frustum;
    frustum.planes[0] = make(r3, r0, 1.0f);  // left
    frustum.planes[1] = make(r3, r0, -1.0f); // right
    frustum.planes[2] = make(r3, r1, 1.0f);  // bottom
    frustum.planes[3] = make(r3, r1, -1.0f); // top
    frustum.planes[4] = make(r3, r2, 1.0f);  // near
    frustum.planes[5] = make(r3, r2, -1.0f); // far
    return frustum;
}

bool Frustum::intersects_sphere(Vec3 center, float radius) const {
    for (const Plane& plane : planes) {
        if (dot(plane.normal, center) + plane.distance < -radius) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Vec4 / Quat

Vec4 operator+(Vec4 a, Vec4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

Vec4 operator*(Vec4 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar, value.w * scalar};
}

float dot(Vec4 a, Vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Vec4 operator*(const Mat4& matrix, Vec4 v) {
    const auto& m = matrix.m;
    return {
        m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
        m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
        m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
        m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w,
    };
}

Quat Quat::axis_angle(Vec3 axis, float radians) {
    const Vec3 n = normalize(axis);
    const float s = std::sin(radians * 0.5f);
    return {n.x * s, n.y * s, n.z * s, std::cos(radians * 0.5f)};
}

Quat Quat::from_euler_xyz(Vec3 radians) {
    return quat_from_matrix(Mat4::rotation_xyz(radians));
}

Quat operator*(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

float dot(Quat a, Quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quat normalize_quat(Quat q) {
    const float len = std::sqrt(dot(q, q));
    if (len < 1e-12f) {
        return Quat::identity();
    }
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Quat nlerp(Quat a, Quat b, float t) {
    if (dot(a, b) < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    return normalize_quat(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}

Quat slerp(Quat a, Quat b, float t) {
    float cos_theta = dot(a, b);
    if (cos_theta < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f) {
        return nlerp(a, b, t);
    }
    const float theta = std::acos(std::clamp(cos_theta, -1.0f, 1.0f));
    const float sin_theta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Mat4 Mat4::rotation(Quat q) {
    q = normalize_quat(q);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 out = identity();
    out.m[0] = 1.0f - 2.0f * (yy + zz);
    out.m[1] = 2.0f * (xy + wz);
    out.m[2] = 2.0f * (xz - wy);
    out.m[4] = 2.0f * (xy - wz);
    out.m[5] = 1.0f - 2.0f * (xx + zz);
    out.m[6] = 2.0f * (yz + wx);
    out.m[8] = 2.0f * (xz + wy);
    out.m[9] = 2.0f * (yz - wx);
    out.m[10] = 1.0f - 2.0f * (xx + yy);
    return out;
}

Mat4 Mat4::trs(Vec3 translation_value, Quat rotation_value, Vec3 scale_value) {
    Mat4 out = rotation(rotation_value);
    for (int i = 0; i < 3; ++i) {
        out.m[i] *= scale_value.x;
        out.m[4 + i] *= scale_value.y;
        out.m[8 + i] *= scale_value.z;
    }
    out.m[12] = translation_value.x;
    out.m[13] = translation_value.y;
    out.m[14] = translation_value.z;
    return out;
}

Quat quat_from_matrix(const Mat4& matrix) {
    // Normalize the basis columns first so scale doesn't leak in.
    Vec3 c0{matrix.m[0], matrix.m[1], matrix.m[2]};
    Vec3 c1{matrix.m[4], matrix.m[5], matrix.m[6]};
    Vec3 c2{matrix.m[8], matrix.m[9], matrix.m[10]};
    c0 = normalize(c0);
    c1 = normalize(c1);
    c2 = normalize(c2);
    const float m00 = c0.x, m10 = c0.y, m20 = c0.z;
    const float m01 = c1.x, m11 = c1.y, m21 = c1.z;
    const float m02 = c2.x, m12 = c2.y, m22 = c2.z;
    Quat q;
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return normalize_quat(q);
}

Mat4 Mat4::perspective_reversed_z(float fov_y_radians, float aspect, float near_z) {
    // Infinite far plane: depth = near / -z_view.
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 out{};
    out.m[0] = f / std::max(aspect, 0.0001f);
    out.m[5] = f;
    out.m[10] = 0.0f;
    out.m[11] = -1.0f;
    out.m[14] = near_z;
    return out;
}

Mat4 Mat4::perspective_reversed_z(float fov_y_radians, float aspect, float near_z, float far_z) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 out{};
    out.m[0] = f / std::max(aspect, 0.0001f);
    out.m[5] = f;
    out.m[10] = near_z / (far_z - near_z);
    out.m[11] = -1.0f;
    out.m[14] = far_z * near_z / (far_z - near_z);
    return out;
}

Mat4 Mat4::orthographic_reversed_z(float left, float right, float bottom, float top, float near_z, float far_z) {
    Mat4 out = identity();
    const float depth = std::max(far_z - near_z, 0.0001f);
    out.m[0] = 2.0f / std::max(right - left, 0.0001f);
    out.m[5] = 2.0f / std::max(top - bottom, 0.0001f);
    out.m[10] = 1.0f / depth;
    out.m[12] = -(right + left) / std::max(right - left, 0.0001f);
    out.m[13] = -(top + bottom) / std::max(top - bottom, 0.0001f);
    out.m[14] = far_z / depth;
    return out;
}

} // namespace Hollow

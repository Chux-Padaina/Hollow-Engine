#include "Forge/Math.hpp"

#include <algorithm>

namespace Forge {

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

Mat4 Mat4::look_at(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
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

Mat4 Transform::matrix() const {
    return Mat4::translation(translation) * Mat4::rotation_xyz(rotation) * Mat4::scale(scale);
}

} // namespace Forge

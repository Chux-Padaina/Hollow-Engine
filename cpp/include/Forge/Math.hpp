#pragma once

#include <array>
#include <cmath>

namespace Forge {

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
Vec3 operator*(Vec3 value, float scalar);
Vec3 operator/(Vec3 value, float scalar);
Vec3& operator+=(Vec3& a, Vec3 b);
Vec3& operator-=(Vec3& a, Vec3 b);
Vec3& operator*=(Vec3& value, float scalar);

float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 value);
Vec3 normalize(Vec3 value);
Vec3 clamp(Vec3 value, float min_value, float max_value);

struct Mat4 {
    std::array<float, 16> m{};

    static Mat4 identity();
    static Mat4 translation(Vec3 value);
    static Mat4 scale(Vec3 value);
    static Mat4 rotation_xyz(Vec3 radians);
    static Mat4 rotation_axis_angle(Vec3 axis, float radians);
    static Mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z);
    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up);
};

Mat4 operator*(const Mat4& a, const Mat4& b);
Vec3 euler_from_rotation_xyz(const Mat4& rotation);

struct Transform {
    Vec3 translation = Vec3::zero();
    Vec3 rotation = Vec3::zero();
    Vec3 scale = Vec3::one();

    Mat4 matrix() const;
};

} // namespace Forge

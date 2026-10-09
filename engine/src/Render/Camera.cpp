// Implementation of Camera.hpp.
#include "Hollow/Render/Camera.hpp"

#include <algorithm>

namespace Hollow {

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

Mat4 Camera::view_matrix() const {
    return Mat4::look_at(position(), target, {0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projection_matrix(float aspect) const {
    return Mat4::perspective(fov_y_radians, aspect, near_plane, far_plane);
}

void Camera::set_view(Vec3 eye, Vec3 view_forward, float fov_y) {
    const Vec3 f = normalize(view_forward);
    if (length(f) < 0.5f) {
        return;
    }
    // position = target + (sin(yaw) cos(pitch), sin(pitch), cos(yaw) cos(pitch)) * distance
    // and forward = -that direction.
    pitch = std::asin(std::clamp(-f.y, -1.0f, 1.0f));
    yaw = std::atan2(-f.x, -f.z);
    distance = 1.0f;
    target = eye + f;
    fov_y_radians = fov_y;
    clamp();
}

void Camera::clamp() {
    pitch = std::clamp(pitch, -1.5f, 1.5f);
    distance = std::clamp(distance, 0.5f, 200.0f);
}

Ray Camera::pick_ray(float pixel_x, float pixel_y, int viewport_width, int viewport_height) const {
    const float width = static_cast<float>(std::max(1, viewport_width));
    const float height = static_cast<float>(std::max(1, viewport_height));
    const float ndc_x = (2.0f * pixel_x / width) - 1.0f;
    const float ndc_y = 1.0f - (2.0f * pixel_y / height);
    const float half_height = std::tan(fov_y_radians * 0.5f);
    const float half_width = half_height * (width / height);
    return {position(), normalize(forward() + right() * (ndc_x * half_width) + up() * (ndc_y * half_height))};
}

float Camera::world_units_per_pixel(Vec3 point, int viewport_height) const {
    const float depth = std::max(dot(point - position(), forward()), near_plane);
    return 2.0f * depth * std::tan(fov_y_radians * 0.5f) / static_cast<float>(std::max(1, viewport_height));
}

float Camera::gizmo_scale(Vec3 point) const {
    // 1.0 at the default 7.5-unit orbit distance.
    const float depth = std::max(dot(point - position(), forward()), near_plane);
    return depth / 7.5f;
}

} // namespace Hollow

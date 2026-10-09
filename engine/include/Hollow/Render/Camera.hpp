// The view camera: an orbit camera (target + yaw/pitch/distance) used by the
// editor, which Play mode can also drive from a CameraComponent via
// set_view(). No GL dependencies.
#pragma once

#include "Hollow/Core/Math.hpp"

namespace Hollow {

struct Camera {
    Vec3 target{0.0f, 0.5f, 0.0f};
    float yaw = 0.72f;
    // Positive pitch = camera above the target looking down. (The old
    // default of -0.36 put the camera under the ground plane.)
    float pitch = 0.36f;
    float distance = 7.5f;
    float fov_y_radians = 1.05f;
    float near_plane = 0.05f;
    float far_plane = 500.0f;

    Vec3 position() const;
    Vec3 forward() const;
    Vec3 right() const;
    Vec3 up() const;

    Mat4 view_matrix() const;
    Mat4 projection_matrix(float aspect) const;

    // Points the camera from `position` along `forward` (keeps the orbit
    // parametrization valid, with the target one unit ahead).
    void set_view(Vec3 position, Vec3 forward, float fov_y);
    // Keeps pitch/distance in a usable range.
    void clamp();

    // World-space ray through a pixel of a width x height viewport.
    Ray pick_ray(float pixel_x, float pixel_y, int viewport_width, int viewport_height) const;
    // World units per screen pixel at `point`'s depth.
    float world_units_per_pixel(Vec3 point, int viewport_height) const;
    // Scale factor that keeps gizmos/icons a constant size on screen.
    float gizmo_scale(Vec3 point) const;
};

} // namespace Hollow

// Implementation of Animation.hpp.
#include "Hollow/Assets/Animation.hpp"

#include <algorithm>
#include <array>

namespace Hollow {

int Skeleton::find(const std::string& name) const {
    for (std::size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Skeleton::find_clip(const std::string& name) const {
    for (std::size_t i = 0; i < clips.size(); ++i) {
        if (clips[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<JointPose> rest_pose(const Skeleton& skeleton) {
    std::vector<JointPose> pose;
    pose.reserve(skeleton.joints.size());
    for (const Joint& joint : skeleton.joints) {
        pose.push_back(joint.rest);
    }
    return pose;
}

namespace {

// Index of the last key at or before t (keys sorted).
std::size_t find_key(const std::vector<float>& times, float t) {
    if (times.size() < 2 || t <= times.front()) {
        return 0;
    }
    if (t >= times.back()) {
        return times.size() - 1;
    }
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, (it - times.begin()) - 1));
}

template <int N>
std::array<float, N> read(const std::vector<float>& values, std::size_t element) {
    std::array<float, N> out{};
    const std::size_t base = element * N;
    for (int i = 0; i < N; ++i) {
        if (base + static_cast<std::size_t>(i) < values.size()) {
            out[static_cast<std::size_t>(i)] = values[base + static_cast<std::size_t>(i)];
        }
    }
    return out;
}

template <int N>
std::array<float, N> sample_channel(const AnimationChannel& channel, float t) {
    const std::vector<float>& times = channel.times;
    const bool cubic = channel.interpolation == Interpolation::CubicSpline;
    const std::size_t stride = cubic ? 3 : 1; // elements per key
    const std::size_t key = find_key(times, t);
    if (times.size() < 2 || key + 1 >= times.size() || t <= times.front()) {
        return read<N>(channel.values, key * stride + (cubic ? 1 : 0));
    }
    const float t0 = times[key];
    const float t1 = times[key + 1];
    const float dt = std::max(t1 - t0, 1e-6f);
    const float u = std::clamp((t - t0) / dt, 0.0f, 1.0f);
    if (channel.interpolation == Interpolation::Step) {
        return read<N>(channel.values, key);
    }
    if (cubic) {
        const auto p0 = read<N>(channel.values, key * 3 + 1);
        const auto m0 = read<N>(channel.values, key * 3 + 2); // out-tangent
        const auto m1 = read<N>(channel.values, (key + 1) * 3 + 0); // in-tangent
        const auto p1 = read<N>(channel.values, (key + 1) * 3 + 1);
        const float u2 = u * u;
        const float u3 = u2 * u;
        std::array<float, N> out{};
        for (int i = 0; i < N; ++i) {
            const auto c = static_cast<std::size_t>(i);
            out[c] = (2 * u3 - 3 * u2 + 1) * p0[c] + (u3 - 2 * u2 + u) * dt * m0[c] + (-2 * u3 + 3 * u2) * p1[c] + (u3 - u2) * dt * m1[c];
        }
        return out;
    }
    const auto a = read<N>(channel.values, key);
    const auto b = read<N>(channel.values, key + 1);
    std::array<float, N> out{};
    if constexpr (N == 4) {
        const Quat q = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
        return {q.x, q.y, q.z, q.w};
    } else {
        for (int i = 0; i < N; ++i) {
            const auto c = static_cast<std::size_t>(i);
            out[c] = a[c] + (b[c] - a[c]) * u;
        }
    }
    return out;
}

} // namespace

void sample_clip(const AnimationClip& clip, float time, std::vector<JointPose>& pose) {
    for (const AnimationChannel& channel : clip.channels) {
        if (channel.joint < 0 || static_cast<std::size_t>(channel.joint) >= pose.size() || channel.times.empty()) {
            continue;
        }
        JointPose& joint = pose[static_cast<std::size_t>(channel.joint)];
        switch (channel.path) {
        case AnimationPath::Translation: {
            const auto v = sample_channel<3>(channel, time);
            joint.translation = {v[0], v[1], v[2]};
            break;
        }
        case AnimationPath::Scale: {
            const auto v = sample_channel<3>(channel, time);
            joint.scale = {v[0], v[1], v[2]};
            break;
        }
        case AnimationPath::Rotation: {
            const auto v = sample_channel<4>(channel, time);
            joint.rotation = normalize_quat(Quat{v[0], v[1], v[2], v[3]});
            break;
        }
        }
    }
}

void blend_poses(const std::vector<JointPose>& a, const std::vector<JointPose>& b, float t, std::vector<JointPose>& out) {
    const std::size_t count = std::min(a.size(), b.size());
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i].translation = lerp(a[i].translation, b[i].translation, t);
        out[i].rotation = nlerp(a[i].rotation, b[i].rotation, t);
        out[i].scale = lerp(a[i].scale, b[i].scale, t);
    }
}

void pose_to_model(const Skeleton& skeleton, const std::vector<JointPose>& pose, std::vector<Mat4>& model) {
    model.resize(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        const Mat4 local = i < pose.size() ? pose[i].matrix() : skeleton.joints[i].rest.matrix();
        const int parent = skeleton.joints[i].parent;
        model[i] = parent >= 0 && static_cast<std::size_t>(parent) < i ? model[static_cast<std::size_t>(parent)] * local : skeleton.root_transform * local;
    }
}

} // namespace Hollow

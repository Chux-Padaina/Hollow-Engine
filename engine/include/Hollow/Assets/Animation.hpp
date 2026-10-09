// Skeletal animation data: a Skeleton (joint hierarchy with rest pose) and
// AnimationClips (keyframed translation/rotation/scale channels, as glTF
// stores them), plus pose evaluation and blending. No GPU code; the
// renderer turns the evaluated joint matrices into a skinning palette.
#pragma once

#include "Hollow/Core/Math.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Hollow {

struct JointPose {
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 matrix() const { return Mat4::trs(translation, rotation, scale); }
};

struct Joint {
    std::string name;
    int parent = -1; // index into Skeleton::joints, parents come first
    JointPose rest;
};

enum class AnimationPath {
    Translation,
    Rotation,
    Scale,
};

enum class Interpolation {
    Step,
    Linear,
    CubicSpline,
};

struct AnimationChannel {
    int joint = -1;
    AnimationPath path = AnimationPath::Translation;
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> times;
    // Vec3 (xyz) or Quat (xyzw) values, packed. Cubic splines store
    // in-tangent, value, out-tangent per key.
    std::vector<float> values;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

struct Skeleton {
    std::vector<Joint> joints;
    // Transform of the skeleton root's parent (model space), e.g. glTF
    // nodes above the joints.
    Mat4 root_transform = Mat4::identity();
    std::vector<AnimationClip> clips; // animations that target these joints
    int find(const std::string& name) const;
    int find_clip(const std::string& name) const;
};

using SkeletonHandle = std::shared_ptr<const Skeleton>;

// The rest pose.
std::vector<JointPose> rest_pose(const Skeleton& skeleton);
// Samples `clip` at `time` (seconds) on top of `pose` (joints without
// channels keep their value).
void sample_clip(const AnimationClip& clip, float time, std::vector<JointPose>& pose);
// Per-joint blend: out = mix(a, b, t).
void blend_poses(const std::vector<JointPose>& a, const std::vector<JointPose>& b, float t, std::vector<JointPose>& out);
// Model-space joint matrices from local poses.
void pose_to_model(const Skeleton& skeleton, const std::vector<JointPose>& pose, std::vector<Mat4>& model);

} // namespace Hollow

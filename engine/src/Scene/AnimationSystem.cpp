#include "Hollow/Scene/AnimationSystem.hpp"

#include "Hollow/Assets/Animation.hpp"
#include "Hollow/Assets/MeshAsset.hpp"

#include <cmath>

namespace Hollow {

namespace {

SkeletonHandle find_skeleton(const Actor& actor) {
    if (const auto* mesh = actor.find_component<StaticMeshComponent>()) {
        for (const MeshPart& part : mesh->parts) {
            if (part.mesh && part.mesh->skinned()) {
                return part.mesh->skeleton;
            }
        }
    }
    return nullptr;
}

float wrap_time(float time, float duration, bool loop) {
    if (duration <= 0.0f) {
        return 0.0f;
    }
    if (loop) {
        time = std::fmod(time, duration);
        return time < 0.0f ? time + duration : time;
    }
    return std::fmin(std::fmax(time, 0.0f), duration);
}

} // namespace

void update_animators(Scene& scene, float dt, bool playing) {
    for (const auto& actor_ptr : scene.actors()) {
        Actor& actor = *actor_ptr;
        auto* animator = actor.find_component<AnimatorComponent>();
        if (animator == nullptr) {
            continue;
        }
        animator->skeleton = find_skeleton(actor);
        const Skeleton* skeleton = animator->skeleton.get();
        if (skeleton == nullptr) {
            animator->joint_matrices.clear();
            continue;
        }
        const bool advance = animator->playing && (playing || animator->preview_in_editor);
        const int clip_index = animator->clip.empty() ? (skeleton->clips.empty() ? -1 : 0) : skeleton->find_clip(animator->clip);
        const std::string clip_name = clip_index >= 0 ? skeleton->clips[static_cast<std::size_t>(clip_index)].name : std::string();
        if (clip_name != animator->active_clip) {
            // Crossfade from whatever was playing.
            animator->previous_clip = animator->active_clip;
            animator->previous_time = animator->time;
            animator->fade_time = animator->previous_clip.empty() ? 0.0f : animator->crossfade;
            animator->active_clip = clip_name;
            animator->time = 0.0f;
        }
        const float step = advance ? dt * animator->speed : 0.0f;
        animator->time += step;
        animator->previous_time += step;

        std::vector<JointPose> pose = rest_pose(*skeleton);
        if (clip_index >= 0) {
            const AnimationClip& clip = skeleton->clips[static_cast<std::size_t>(clip_index)];
            animator->time = wrap_time(animator->time, clip.duration, animator->loop);
            sample_clip(clip, animator->time, pose);
        }
        if (animator->fade_time > 0.0f && animator->crossfade > 0.0f) {
            const int previous = skeleton->find_clip(animator->previous_clip);
            if (previous >= 0) {
                const AnimationClip& clip = skeleton->clips[static_cast<std::size_t>(previous)];
                std::vector<JointPose> from = rest_pose(*skeleton);
                sample_clip(clip, wrap_time(animator->previous_time, clip.duration, animator->loop), from);
                const float t = 1.0f - animator->fade_time / animator->crossfade;
                blend_poses(from, pose, t, pose);
            }
            animator->fade_time = std::fmax(animator->fade_time - std::fabs(step), 0.0f);
        }
        pose_to_model(*skeleton, pose, animator->joint_matrices);
        ++animator->pose_frame;
    }
}

} // namespace Hollow

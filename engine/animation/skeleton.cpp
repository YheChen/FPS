#include "engine/animation/skeleton.h"

#include <algorithm>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"

namespace eng {

namespace {

glm::mat4 compose(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
    glm::mat4 out = glm::translate(glm::mat4{1.0f}, translation);
    out *= glm::mat4_cast(rotation);
    return glm::scale(out, scale);
}

// Index of the keyframe at or before `time`, plus the fraction to the next.
// Returns {index, 0.0} when `time` sits on or past the last key.
std::pair<std::size_t, float> find_keyframe(const std::vector<float>& times, float time) {
    if (times.size() < 2 || time <= times.front()) {
        return {0u, 0.0f};
    }
    if (time >= times.back()) {
        return {times.size() - 1, 0.0f};
    }
    // upper_bound gives the first key strictly after `time`, so the segment
    // starts one before it.
    const auto next = std::upper_bound(times.begin(), times.end(), time);
    const auto index = static_cast<std::size_t>(std::distance(times.begin(), next)) - 1;
    const float span = times[index + 1] - times[index];
    const float fraction = span > 0.0f ? (time - times[index]) / span : 0.0f;
    return {index, fraction};
}

}  // namespace

std::optional<Skeleton> Skeleton::from_gltf(const GltfModel& model, const GltfSkin& skin) {
    if (skin.joints.size() != skin.inverse_bind_matrices.size()) {
        log::error("Skeleton: skin '{}' has {} joints but {} inverse bind matrices", skin.name,
                   skin.joints.size(), skin.inverse_bind_matrices.size());
        return std::nullopt;
    }
    if (skin.joints.empty()) {
        log::error("Skeleton: skin '{}' has no joints", skin.name);
        return std::nullopt;
    }

    // node index -> position in the skin's joint list.
    std::unordered_map<int, int> node_to_joint;
    node_to_joint.reserve(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const int node = skin.joints[i];
        if (node < 0 || static_cast<std::size_t>(node) >= model.nodes.size()) {
            log::error("Skeleton: skin '{}' joint {} references node {}, out of range", skin.name,
                       i, node);
            return std::nullopt;
        }
        node_to_joint.emplace(node, static_cast<int>(i));
    }

    Skeleton skeleton;
    skeleton.joints_.resize(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const GltfNode& node = model.nodes[static_cast<std::size_t>(skin.joints[i])];
        Joint& joint = skeleton.joints_[i];
        joint.name = node.name;
        joint.inverse_bind = skin.inverse_bind_matrices[i];
        joint.translation = node.translation;
        joint.rotation = node.rotation;
        joint.scale = node.scale;

        // A joint's parent is only a parent *within the skeleton*; a skin can
        // be attached under unrelated nodes, and those are roots here.
        joint.parent = -1;
        if (node.parent >= 0) {
            const auto found = node_to_joint.find(node.parent);
            if (found != node_to_joint.end()) {
                joint.parent = found->second;
            }
        }
    }

    // pose_to_joint_matrices walks the list once and reads parents' results,
    // so every parent must come first. glTF does not require that ordering.
    for (std::size_t i = 0; i < skeleton.joints_.size(); ++i) {
        const int parent = skeleton.joints_[i].parent;
        if (parent >= static_cast<int>(i)) {
            log::error(
                "Skeleton: skin '{}' joint {} ('{}') has parent {} which does not precede it; "
                "reordering is not supported because vertex joint indices refer to this order",
                skin.name, i, skeleton.joints_[i].name, parent);
            return std::nullopt;
        }
    }

    return skeleton;
}

int Skeleton::find(std::string_view name) const {
    for (std::size_t i = 0; i < joints_.size(); ++i) {
        if (joints_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Skeleton::rest_pose(Pose& out) const {
    out.resize(joints_.size());
    for (std::size_t i = 0; i < joints_.size(); ++i) {
        out.translations[i] = joints_[i].translation;
        out.rotations[i] = joints_[i].rotation;
        out.scales[i] = joints_[i].scale;
    }
}

void Skeleton::rest_pose(std::vector<glm::mat4>& out) const {
    Pose pose;
    rest_pose(pose);
    pose_to_joint_matrices(*this, pose, out);
}

std::vector<AnimationClip> build_clips(const GltfModel& model, const Skeleton& skeleton) {
    std::unordered_map<int, int> node_to_joint;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const int joint = skeleton.find(model.nodes[i].name);
        if (joint >= 0) {
            node_to_joint.emplace(static_cast<int>(i), joint);
        }
    }

    std::vector<AnimationClip> clips;
    clips.reserve(model.animations.size());
    for (const GltfAnimation& animation : model.animations) {
        AnimationClip clip;
        clip.name = animation.name;
        clip.duration_seconds = animation.duration_seconds;
        for (const GltfAnimationChannel& channel : animation.channels) {
            const auto found = node_to_joint.find(channel.node);
            if (found == node_to_joint.end()) {
                continue;  // animates something outside this skeleton
            }
            AnimationClip::Channel out;
            out.joint = found->second;
            out.path = channel.path;
            out.times = channel.times;
            out.values = channel.values;
            clip.channels.push_back(std::move(out));
        }
        clips.push_back(std::move(clip));
    }
    return clips;
}

const AnimationClip* find_clip(const std::vector<AnimationClip>& clips, std::string_view name) {
    for (const AnimationClip& clip : clips) {
        if (clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

void Pose::resize(std::size_t joints) {
    translations.assign(joints, glm::vec3{0.0f});
    rotations.assign(joints, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    scales.assign(joints, glm::vec3{1.0f});
}

void sample_clip(const Skeleton& skeleton, const AnimationClip& clip, float time_seconds, bool loop,
                 Pose& pose) {
    const std::size_t count = skeleton.joint_count();
    if (pose.translations.size() != count) {
        pose.resize(count);
    }
    // Start from the rest pose so joints the clip ignores keep their bind
    // transform instead of collapsing to the origin.
    for (std::size_t i = 0; i < count; ++i) {
        pose.translations[i] = skeleton.joints()[i].translation;
        pose.rotations[i] = skeleton.joints()[i].rotation;
        pose.scales[i] = skeleton.joints()[i].scale;
    }

    float time = time_seconds;
    if (loop && clip.duration_seconds > 0.0f) {
        time = std::fmod(time, clip.duration_seconds);
        if (time < 0.0f) {
            time += clip.duration_seconds;
        }
    } else {
        time = std::clamp(time, 0.0f, clip.duration_seconds);
    }

    for (const AnimationClip::Channel& channel : clip.channels) {
        if (channel.joint < 0 || static_cast<std::size_t>(channel.joint) >= count ||
            channel.times.empty()) {
            continue;
        }
        const auto [index, fraction] = find_keyframe(channel.times, time);
        const glm::vec4& a = channel.values[index];
        const glm::vec4& b = channel.values[std::min(index + 1, channel.values.size() - 1)];
        const auto joint = static_cast<std::size_t>(channel.joint);

        switch (channel.path) {
            case GltfAnimationPath::Translation:
                pose.translations[joint] = glm::mix(glm::vec3(a), glm::vec3(b), fraction);
                break;
            case GltfAnimationPath::Rotation: {
                // glTF quaternions are xyzw; glm::quat is wxyz.
                const glm::quat qa{a.w, a.x, a.y, a.z};
                const glm::quat qb{b.w, b.x, b.y, b.z};
                pose.rotations[joint] = glm::normalize(glm::slerp(qa, qb, fraction));
                break;
            }
            case GltfAnimationPath::Scale:
                pose.scales[joint] = glm::mix(glm::vec3(a), glm::vec3(b), fraction);
                break;
        }
    }
}

void blend_poses(const Pose& a, const Pose& b, float weight, Pose& out) {
    const std::size_t count = std::min(a.translations.size(), b.translations.size());
    if (out.translations.size() != count) {
        out.resize(count);
    }
    const float t = std::clamp(weight, 0.0f, 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        out.translations[i] = glm::mix(a.translations[i], b.translations[i], t);
        // slerp, not mix: a linear blend of quaternions shortens the arc and
        // makes limbs dip toward the origin mid-transition.
        out.rotations[i] = glm::normalize(glm::slerp(a.rotations[i], b.rotations[i], t));
        out.scales[i] = glm::mix(a.scales[i], b.scales[i], t);
    }
}

void pose_to_joint_matrices(const Skeleton& skeleton, const Pose& pose,
                            std::vector<glm::mat4>& out) {
    const std::size_t count = skeleton.joint_count();
    out.assign(count, glm::mat4{1.0f});
    if (pose.translations.size() < count) {
        return;
    }

    // Parents precede children (guaranteed by from_gltf), so one forward pass
    // is enough and every parent's world matrix is already final.
    std::vector<glm::mat4> world(count, glm::mat4{1.0f});
    for (std::size_t i = 0; i < count; ++i) {
        const glm::mat4 local = compose(pose.translations[i], pose.rotations[i], pose.scales[i]);
        const int parent = skeleton.joints()[i].parent;
        world[i] = parent >= 0 ? world[static_cast<std::size_t>(parent)] * local : local;
        out[i] = world[i] * skeleton.joints()[i].inverse_bind;
    }
}

}  // namespace eng

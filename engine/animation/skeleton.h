#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/assets/gltf_loader.h"

// Skeletal animation, headless. Everything here is pure data plus pure
// functions on it, so poses can be tested without a GPU or a window; the
// renderer only consumes the joint matrices this produces.
namespace eng {

struct Pose;  // defined below; Skeleton::rest_pose fills one

// A skeleton flattened out of a glTF skin.
//
// Joints are stored in the skin's own order, which is what vertex JOINTS_0
// indices refer to. `parent` is an index into this same list (-1 for a root),
// and every parent is guaranteed to appear BEFORE its children, so a pose can
// be computed in one forward pass with no recursion or repeated work.
class Skeleton {
public:
    struct Joint {
        std::string name;
        int parent = -1;
        glm::mat4 inverse_bind{1.0f};
        // Rest-pose local transform, used for any joint an animation does
        // not touch.
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };

    // Returns nullopt (logged) when the skin is malformed: a joint outside
    // the node list, a mismatched inverse-bind count, or a parent that does
    // not precede its child after sorting (which means a cycle).
    static std::optional<Skeleton> from_gltf(const GltfModel& model, const GltfSkin& skin);

    std::size_t joint_count() const { return joints_.size(); }
    const std::vector<Joint>& joints() const { return joints_; }
    // Index into this skeleton, or -1.
    int find(std::string_view name) const;

    // Writes the rest pose (no animation) into `out`.
    void rest_pose(std::vector<glm::mat4>& out) const;

    // Fills `out` with the bind-pose local transforms. Prefer this over
    // Pose::resize when hand-posing: resize() zeroes translations, which
    // collapses the skeleton onto its root.
    void rest_pose(Pose& out) const;

private:
    std::vector<Joint> joints_;
};

// An animation clip retargeted onto a Skeleton: channels addressed by
// skeleton joint index rather than by glTF node index, so sampling never has
// to search.
struct AnimationClip {
    struct Channel {
        int joint = -1;
        GltfAnimationPath path = GltfAnimationPath::Rotation;
        std::vector<float> times;
        std::vector<glm::vec4> values;
    };

    std::string name;
    float duration_seconds = 0.0f;
    std::vector<Channel> channels;
};

// Retargets every animation in `model` onto `skeleton`, dropping channels
// that address nodes outside the skin (a clip may animate unrelated nodes).
std::vector<AnimationClip> build_clips(const GltfModel& model, const Skeleton& skeleton);

const AnimationClip* find_clip(const std::vector<AnimationClip>& clips, std::string_view name);

// Local joint transforms at one instant. Kept separate from the final matrix
// array so two poses can be blended before the hierarchy is walked --
// blending world matrices instead would bend limbs the wrong way.
struct Pose {
    std::vector<glm::vec3> translations;
    std::vector<glm::quat> rotations;
    std::vector<glm::vec3> scales;

    // Sizes the arrays and zeroes them. Note this is NOT the rest pose:
    // translations become 0, which collapses every joint onto its parent.
    // Use Skeleton::rest_pose(Pose&) unless you are about to overwrite every
    // joint yourself.
    void resize(std::size_t joints);
};

// Samples `clip` at `time_seconds` into `pose`. Joints the clip does not
// animate keep their rest-pose values. `loop` wraps the time; otherwise it
// clamps to the last keyframe.
void sample_clip(const Skeleton& skeleton, const AnimationClip& clip, float time_seconds, bool loop,
                 Pose& pose);

// Linear blend of two poses, `weight` = 0 gives `a`, 1 gives `b`. Rotations
// use slerp (shortest arc), which is why this exists rather than a plain mix.
void blend_poses(const Pose& a, const Pose& b, float weight, Pose& out);

// Walks the hierarchy and produces the matrices a skinning shader wants:
// out[j] = world(j) * inverse_bind(j).
void pose_to_joint_matrices(const Skeleton& skeleton, const Pose& pose,
                            std::vector<glm::mat4>& out);

}  // namespace eng

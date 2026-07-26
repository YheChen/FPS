#include "engine/animation/skeleton.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"

namespace {

using Catch::Approx;

const eng::GltfModel* load_character() {
    static eng::AssetCache cache{*eng::find_assets_root()};
    return cache.model("models/character.glb");
}

// A two-joint chain built by hand: root at the origin, child 1 m above it.
// Small enough that every expected matrix can be reasoned about directly.
eng::GltfModel two_joint_model() {
    eng::GltfModel model;

    eng::GltfNode root;
    root.name = "root";
    root.translation = {0.0f, 0.0f, 0.0f};
    root.children = {1};

    eng::GltfNode child;
    child.name = "child";
    child.translation = {0.0f, 1.0f, 0.0f};
    child.parent = 0;

    model.nodes = {root, child};

    eng::GltfSkin skin;
    skin.name = "test";
    skin.joints = {0, 1};
    // Inverse of each joint's rest-pose world transform.
    skin.inverse_bind_matrices = {
        glm::mat4{1.0f},
        glm::translate(glm::mat4{1.0f}, {0.0f, -1.0f, 0.0f}),
    };
    model.skins = {skin};
    return model;
}

glm::vec3 transform_point(const glm::mat4& matrix, const glm::vec3& point) {
    return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

TEST_CASE("a skeleton is built from a skin with parents preceding children", "[skeleton]") {
    const eng::GltfModel model = two_joint_model();
    const auto skeleton = eng::Skeleton::from_gltf(model, model.skins[0]);
    REQUIRE(skeleton.has_value());

    CHECK(skeleton->joint_count() == 2);
    CHECK(skeleton->joints()[0].parent == -1);
    CHECK(skeleton->joints()[1].parent == 0);
    CHECK(skeleton->find("child") == 1);
    CHECK(skeleton->find("nonexistent") == -1);
}

TEST_CASE("the rest pose leaves vertices where they were authored", "[skeleton]") {
    // world * inverse_bind is the identity in the rest pose, by definition.
    // If it is not, every skinned mesh renders mangled before any animation
    // has even been applied -- the single most useful invariant here.
    const eng::GltfModel model = two_joint_model();
    const auto skeleton = eng::Skeleton::from_gltf(model, model.skins[0]);
    REQUIRE(skeleton.has_value());

    std::vector<glm::mat4> matrices;
    skeleton->rest_pose(matrices);
    REQUIRE(matrices.size() == 2);

    for (const glm::mat4& matrix : matrices) {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                CHECK(matrix[column][row] == Approx(glm::mat4{1.0f}[column][row]).margin(1e-5f));
            }
        }
    }
}

TEST_CASE("rotating a parent carries the child with it", "[skeleton]") {
    const eng::GltfModel model = two_joint_model();
    const auto skeleton = eng::Skeleton::from_gltf(model, model.skins[0]);
    REQUIRE(skeleton.has_value());

    // Start from the rest pose, not a zeroed one: Pose::resize() would put
    // the child's translation at 0 and collapse it onto the root.
    eng::Pose pose;
    skeleton->rest_pose(pose);
    // 90 degrees about +Z turns +Y into -X.
    pose.rotations[0] =
        glm::angleAxis(std::numbers::pi_v<float> * 0.5f, glm::vec3{0.0f, 0.0f, 1.0f});

    std::vector<glm::mat4> matrices;
    eng::pose_to_joint_matrices(*skeleton, pose, matrices);
    REQUIRE(matrices.size() == 2);

    // A vertex bound to the child at its rest position (0,1,0) should swing
    // to (-1,0,0). This is the test that catches a transposed matrix or a
    // parent/child multiplication in the wrong order.
    const glm::vec3 moved = transform_point(matrices[1], {0.0f, 1.0f, 0.0f});
    CHECK(moved.x == Approx(-1.0f).margin(1e-5f));
    CHECK(moved.y == Approx(0.0f).margin(1e-5f));
    CHECK(moved.z == Approx(0.0f).margin(1e-5f));

    // The root itself does not move a vertex at the origin.
    const glm::vec3 at_root = transform_point(matrices[0], {0.0f, 0.0f, 0.0f});
    CHECK(glm::length(at_root) == Approx(0.0f).margin(1e-5f));
}

TEST_CASE("clip sampling interpolates and respects looping", "[skeleton]") {
    const eng::GltfModel model = two_joint_model();
    const auto skeleton = eng::Skeleton::from_gltf(model, model.skins[0]);
    REQUIRE(skeleton.has_value());

    // Rotate the root from 0 to 90 degrees over one second.
    eng::AnimationClip clip;
    clip.name = "turn";
    clip.duration_seconds = 1.0f;
    eng::AnimationClip::Channel channel;
    channel.joint = 0;
    channel.path = eng::GltfAnimationPath::Rotation;
    channel.times = {0.0f, 1.0f};
    const glm::quat start{1.0f, 0.0f, 0.0f, 0.0f};
    const glm::quat end =
        glm::angleAxis(std::numbers::pi_v<float> * 0.5f, glm::vec3{0.0f, 0.0f, 1.0f});
    channel.values = {{start.x, start.y, start.z, start.w}, {end.x, end.y, end.z, end.w}};
    clip.channels = {channel};

    eng::Pose pose;
    eng::sample_clip(*skeleton, clip, 0.0f, false, pose);
    CHECK(glm::angle(pose.rotations[0]) == Approx(0.0f).margin(1e-4f));

    eng::sample_clip(*skeleton, clip, 0.5f, false, pose);
    CHECK(glm::angle(pose.rotations[0]) == Approx(std::numbers::pi_v<float> * 0.25f).margin(1e-3f));

    eng::sample_clip(*skeleton, clip, 1.0f, false, pose);
    CHECK(glm::angle(pose.rotations[0]) == Approx(std::numbers::pi_v<float> * 0.5f).margin(1e-4f));

    // Past the end without looping clamps to the last keyframe.
    eng::sample_clip(*skeleton, clip, 7.5f, false, pose);
    CHECK(glm::angle(pose.rotations[0]) == Approx(std::numbers::pi_v<float> * 0.5f).margin(1e-4f));

    // Looping wraps: 1.5 s is the same as 0.5 s.
    eng::Pose looped;
    eng::sample_clip(*skeleton, clip, 1.5f, true, looped);
    CHECK(glm::angle(looped.rotations[0]) ==
          Approx(std::numbers::pi_v<float> * 0.25f).margin(1e-3f));

    // And a negative time wraps forwards, not into a clamp at zero.
    eng::sample_clip(*skeleton, clip, -0.5f, true, looped);
    CHECK(glm::angle(looped.rotations[0]) ==
          Approx(std::numbers::pi_v<float> * 0.25f).margin(1e-3f));
}

TEST_CASE("joints a clip does not animate keep their rest transform", "[skeleton]") {
    const eng::GltfModel model = two_joint_model();
    const auto skeleton = eng::Skeleton::from_gltf(model, model.skins[0]);
    REQUIRE(skeleton.has_value());

    eng::AnimationClip empty;
    empty.name = "nothing";
    empty.duration_seconds = 1.0f;

    eng::Pose pose;
    eng::sample_clip(*skeleton, empty, 0.4f, true, pose);
    // The child must still be 1 m up, not collapsed onto the root.
    CHECK(pose.translations[1].y == Approx(1.0f));

    std::vector<glm::mat4> matrices;
    eng::pose_to_joint_matrices(*skeleton, pose, matrices);
    const glm::vec3 unmoved = transform_point(matrices[1], {0.0f, 1.0f, 0.0f});
    CHECK(unmoved.y == Approx(1.0f).margin(1e-5f));
}

TEST_CASE("pose blending takes the short way round", "[skeleton]") {
    eng::Pose a;
    eng::Pose b;
    a.resize(1);
    b.resize(1);
    a.rotations[0] = glm::angleAxis(-0.4f, glm::vec3{0.0f, 1.0f, 0.0f});
    b.rotations[0] = glm::angleAxis(0.4f, glm::vec3{0.0f, 1.0f, 0.0f});
    a.translations[0] = {0.0f, 0.0f, 0.0f};
    b.translations[0] = {2.0f, 0.0f, 0.0f};

    eng::Pose out;
    eng::blend_poses(a, b, 0.5f, out);
    // Halfway between -0.4 and +0.4 rad is 0, and the quaternion must stay
    // unit length -- a naive component mix would not.
    CHECK(glm::angle(out.rotations[0]) == Approx(0.0f).margin(1e-3f));
    CHECK(glm::length(out.rotations[0]) == Approx(1.0f).margin(1e-5f));
    CHECK(out.translations[0].x == Approx(1.0f));

    // The endpoints are exact, and weights outside [0,1] clamp.
    eng::blend_poses(a, b, 0.0f, out);
    CHECK(out.translations[0].x == Approx(0.0f));
    eng::blend_poses(a, b, 1.0f, out);
    CHECK(out.translations[0].x == Approx(2.0f));
    eng::blend_poses(a, b, 5.0f, out);
    CHECK(out.translations[0].x == Approx(2.0f));
    eng::blend_poses(a, b, -5.0f, out);
    CHECK(out.translations[0].x == Approx(0.0f));
}

TEST_CASE("a malformed skin is rejected rather than silently posed", "[skeleton]") {
    eng::GltfModel model = two_joint_model();

    eng::GltfSkin no_joints = model.skins[0];
    no_joints.joints.clear();
    no_joints.inverse_bind_matrices.clear();
    CHECK_FALSE(eng::Skeleton::from_gltf(model, no_joints).has_value());

    eng::GltfSkin mismatched = model.skins[0];
    mismatched.inverse_bind_matrices.pop_back();
    CHECK_FALSE(eng::Skeleton::from_gltf(model, mismatched).has_value());

    eng::GltfSkin out_of_range = model.skins[0];
    out_of_range.joints[1] = 99;
    CHECK_FALSE(eng::Skeleton::from_gltf(model, out_of_range).has_value());

    // Child listed before its parent: pose_to_joint_matrices walks forwards
    // once, so this ordering would read a stale parent matrix.
    eng::GltfSkin reversed = model.skins[0];
    reversed.joints = {1, 0};
    std::reverse(reversed.inverse_bind_matrices.begin(), reversed.inverse_bind_matrices.end());
    CHECK_FALSE(eng::Skeleton::from_gltf(model, reversed).has_value());
}

// --- the real asset --------------------------------------------------------

TEST_CASE("character.glb loads with a skin and three clips", "[skeleton]") {
    const eng::GltfModel* model = load_character();
    REQUIRE(model != nullptr);

    REQUIRE(model->skins.size() == 1);
    CHECK(model->skins[0].joints.size() == 12);
    CHECK(model->skins[0].inverse_bind_matrices.size() == 12);
    REQUIRE(model->animations.size() == 3);

    REQUIRE(model->meshes.size() == 1);
    REQUIRE(model->meshes[0].primitives.size() == 1);
    const eng::MeshData& mesh = model->meshes[0].primitives[0].mesh;
    CHECK(mesh.vertices.size() == 288);  // torso subdivided into 4 rings

    // Every vertex must be skinned, with weights summing to 1 and joints in
    // range -- an unweighted vertex would collapse to the origin.
    for (const eng::Vertex& vertex : mesh.vertices) {
        REQUIRE(vertex.skinned());
        const float sum = vertex.weights.x + vertex.weights.y + vertex.weights.z + vertex.weights.w;
        CHECK(sum == Approx(1.0f).margin(1e-4f));
        CHECK(vertex.joints.x < 12u);
        CHECK(vertex.joints.y < 12u);
        CHECK(vertex.joints.z < 12u);
        CHECK(vertex.joints.w < 12u);
    }
}

TEST_CASE("the character's torso blends between two joints", "[skeleton]") {
    // The rig is mostly rigid parts, so this is the only place the shader's
    // multi-bone blend does real work. If the generator ever stops emitting
    // it, the blend path silently goes untested.
    const eng::GltfModel* model = load_character();
    REQUIRE(model != nullptr);

    std::size_t blended = 0;
    for (const eng::Vertex& vertex : model->meshes[0].primitives[0].mesh.vertices) {
        const bool has_two = vertex.weights.x > 0.01f && vertex.weights.y > 0.01f;
        blended += has_two ? 1 : 0;
    }
    CHECK(blended == 48);
}

TEST_CASE("the character's rest pose is the identity", "[skeleton]") {
    const eng::GltfModel* model = load_character();
    REQUIRE(model != nullptr);
    const auto skeleton = eng::Skeleton::from_gltf(*model, model->skins[0]);
    REQUIRE(skeleton.has_value());

    std::vector<glm::mat4> matrices;
    skeleton->rest_pose(matrices);
    REQUIRE(matrices.size() == 12);
    for (const glm::mat4& matrix : matrices) {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                CHECK(matrix[column][row] == Approx(glm::mat4{1.0f}[column][row]).margin(1e-4f));
            }
        }
    }
}

TEST_CASE("the character's clips retarget onto the skeleton", "[skeleton]") {
    const eng::GltfModel* model = load_character();
    REQUIRE(model != nullptr);
    const auto skeleton = eng::Skeleton::from_gltf(*model, model->skins[0]);
    REQUIRE(skeleton.has_value());

    const std::vector<eng::AnimationClip> clips = eng::build_clips(*model, *skeleton);
    REQUIRE(clips.size() == 3);

    const eng::AnimationClip* run = eng::find_clip(clips, "run");
    REQUIRE(run != nullptr);
    CHECK(run->duration_seconds == Approx(0.8f));
    CHECK(run->channels.size() == 9);
    CHECK(eng::find_clip(clips, "idle") != nullptr);
    CHECK(eng::find_clip(clips, "jump") != nullptr);
    CHECK(eng::find_clip(clips, "cartwheel") == nullptr);

    for (const eng::AnimationClip::Channel& channel : run->channels) {
        CHECK(channel.joint >= 0);
        CHECK(channel.joint < 12);
        CHECK(channel.times.size() == channel.values.size());
    }
}

TEST_CASE("the run clip actually moves the legs and loops cleanly", "[skeleton]") {
    const eng::GltfModel* model = load_character();
    REQUIRE(model != nullptr);
    const auto skeleton = eng::Skeleton::from_gltf(*model, model->skins[0]);
    REQUIRE(skeleton.has_value());
    const std::vector<eng::AnimationClip> clips = eng::build_clips(*model, *skeleton);
    const eng::AnimationClip* run = eng::find_clip(clips, "run");
    REQUIRE(run != nullptr);

    const int thigh = skeleton->find("leg_l_upper");
    REQUIRE(thigh >= 0);
    const auto thigh_index = static_cast<std::size_t>(thigh);

    eng::Pose pose;
    eng::sample_clip(*skeleton, *run, 0.0f, true, pose);
    const glm::quat at_start = pose.rotations[thigh_index];
    eng::sample_clip(*skeleton, *run, 0.4f, true, pose);
    const glm::quat mid_cycle = pose.rotations[thigh_index];

    // Genuinely different halfway through the cycle.
    CHECK(std::abs(glm::dot(at_start, mid_cycle)) < 0.99f);

    // The clip loops: t = 0 and t = duration must match, or the run visibly
    // snaps every stride.
    eng::sample_clip(*skeleton, *run, run->duration_seconds, true, pose);
    CHECK(std::abs(glm::dot(at_start, pose.rotations[thigh_index])) == Approx(1.0f).margin(1e-4f));

    // And the whole skeleton stays finite across the cycle.
    for (int step = 0; step <= 40; ++step) {
        eng::sample_clip(*skeleton, *run, static_cast<float>(step) * 0.02f, true, pose);
        std::vector<glm::mat4> matrices;
        eng::pose_to_joint_matrices(*skeleton, pose, matrices);
        for (const glm::mat4& matrix : matrices) {
            CHECK(std::isfinite(matrix[3][0]));
            CHECK(std::isfinite(matrix[3][1]));
            CHECK(std::isfinite(matrix[3][2]));
        }
    }
}

TEST_CASE("posing the character moves a foot but not the hips", "[skeleton]") {
    const eng::GltfModel* model = load_character();
    REQUIRE(model != nullptr);
    const auto skeleton = eng::Skeleton::from_gltf(*model, model->skins[0]);
    REQUIRE(skeleton.has_value());
    const std::vector<eng::AnimationClip> clips = eng::build_clips(*model, *skeleton);
    const eng::AnimationClip* run = eng::find_clip(clips, "run");
    REQUIRE(run != nullptr);

    eng::Pose pose;
    eng::sample_clip(*skeleton, *run, 0.0f, true, pose);
    std::vector<glm::mat4> matrices;
    eng::pose_to_joint_matrices(*skeleton, pose, matrices);

    const int hips = skeleton->find("hips");
    const int shin = skeleton->find("leg_l_lower");
    REQUIRE(hips >= 0);
    REQUIRE(shin >= 0);

    // The run clip does not animate the hips, so a hip-bound vertex is where
    // it started.
    const glm::vec3 hip_point{0.0f, 0.95f, 0.0f};
    const glm::vec3 hip_moved =
        transform_point(matrices[static_cast<std::size_t>(hips)], hip_point);
    CHECK(glm::length(hip_moved - hip_point) == Approx(0.0f).margin(1e-4f));

    // The shin, two rotated joints down the chain, definitely moves.
    const glm::vec3 foot{0.11f, 0.22f, 0.0f};
    const glm::vec3 foot_moved = transform_point(matrices[static_cast<std::size_t>(shin)], foot);
    CHECK(glm::length(foot_moved - foot) > 0.05f);
}

}  // namespace

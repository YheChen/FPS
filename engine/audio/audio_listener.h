#pragma once

#include <glm/glm.hpp>

// Listener-frame maths for 3D audio, deliberately kept out of the miniaudio
// wrapper so the handedness can be unit-tested without an audio device. A
// left/right inversion here is invisible in a screenshot, inaudible to a
// passing playtest, and makes players turn the wrong way in a firefight --
// exactly the class of bug that needs a test rather than a listen.
//
// These functions reproduce what miniaudio's spatializer does internally
// (ma_spatializer_get_relative_position_and_direction): forward is the third
// basis vector, right is cross(forward, world_up), up closes the set, and the
// emitter goes through a look-at whose Z row is negated. miniaudio's listener
// defaults to ma_handedness_right with forward = -Z, which is this project's
// convention too (docs/rendering.md, +Y up, -Z forward), so nothing is
// flipped on the way in -- and this header is how we prove it.
namespace eng {

// An orthonormal listener frame. Right-handed: cross(right, up) == -forward.
struct ListenerBasis {
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
};

// `forward` need not be normalized and `world_up` need not be perpendicular
// to it. Looking straight along world_up leaves the right vector undefined;
// miniaudio falls back to +X there rather than producing NaNs, so we do too.
// A zero-length `forward` is the one place this deliberately differs: it
// substitutes -Z where miniaudio would carry the zero through, because a
// basis with no forward axis is a worse thing to hand back than a default one.
inline ListenerBasis listener_basis(const glm::vec3& forward, const glm::vec3& world_up) {
    ListenerBasis basis;
    const float forward_length2 = glm::dot(forward, forward);
    basis.forward = forward_length2 > 0.0f ? forward * glm::inversesqrt(forward_length2)
                                           : glm::vec3{0.0f, 0.0f, -1.0f};
    const glm::vec3 side = glm::cross(basis.forward, world_up);
    const float side_length2 = glm::dot(side, side);
    basis.right =
        side_length2 > 0.0f ? side * glm::inversesqrt(side_length2) : glm::vec3{1.0f, 0.0f, 0.0f};
    // Already unit length: right and forward are unit and perpendicular.
    basis.up = glm::cross(basis.right, basis.forward);
    return basis;
}

// The emitter expressed in the listener's own frame: +x is to the listener's
// right, +y above, and -z ahead (the same -Z-forward convention the camera
// uses). The sign of x is the whole ball game -- miniaudio pans by dotting
// this against per-channel directions, and the stereo front-left channel
// points at -x while front-right points at +x, so positive x is heard on the
// right.
inline glm::vec3 listener_space(const glm::vec3& listener_position, const glm::vec3& forward,
                                const glm::vec3& world_up, const glm::vec3& emitter_position) {
    const ListenerBasis basis = listener_basis(forward, world_up);
    const glm::vec3 offset = emitter_position - listener_position;
    return {glm::dot(basis.right, offset), glm::dot(basis.up, offset),
            -glm::dot(basis.forward, offset)};
}

}  // namespace eng

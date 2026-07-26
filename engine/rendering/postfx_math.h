#pragma once

#include <glm/glm.hpp>

// The pure maths behind the post-processing chain, kept headless so it can be
// unit-tested. PostFx runs the GPU passes.
//
// IMPORTANT: the shaders in postfx.cpp implement these same formulas in GLSL.
// The tests here lock the *curves* down (monotonic, correct endpoints, no
// values escaping the display range); they cannot prove the GLSL copy matches.
// If you change a curve, change both, and keep the constants side by side.
namespace eng {

// Rec. 709 relative luminance. Bloom thresholds on luminance rather than
// per-channel so a saturated colour is not treated as brighter than a white
// of the same intensity.
float luminance(const glm::vec3& color);

// Bloom bright-pass weight in [0, 1].
//
// A hard `luminance > threshold` cut makes bloom pop on and off as pixels
// cross it, which is very visible on a moving muzzle flash. `knee` widens the
// transition into a quadratic ramp: below `threshold - knee` nothing blooms,
// above `threshold + knee` everything does, and in between it eases.
//
// `knee <= 0` degenerates to the hard cut on purpose (useful for tests).
float bloom_weight(float pixel_luminance, float threshold, float knee);

// ACES filmic tonemap (Narkowicz's fit), mapping open-ended HDR values into
// [0, 1]. Chosen over Reinhard because it keeps saturation in the highlights
// instead of washing bright colours toward white.
glm::vec3 aces_tonemap(const glm::vec3& color);

// Half a framebuffer dimension for the bloom chain, never below 1: a 1-pixel
// wide window must not produce a zero-sized render target.
int half_resolution(int size);

}  // namespace eng

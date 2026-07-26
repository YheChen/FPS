#include "engine/rendering/postfx_math.h"

#include <algorithm>

namespace eng {

float luminance(const glm::vec3& color) {
    return glm::dot(color, glm::vec3{0.2126f, 0.7152f, 0.0722f});
}

float bloom_weight(float pixel_luminance, float threshold, float knee) {
    if (knee <= 0.0f) {
        return pixel_luminance > threshold ? 1.0f : 0.0f;
    }
    // Quadratic ease across the 2*knee wide band centred on `threshold`.
    const float t = std::clamp((pixel_luminance - (threshold - knee)) / (2.0f * knee), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

glm::vec3 aces_tonemap(const glm::vec3& color) {
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const glm::vec3 clamped = glm::max(color, glm::vec3{0.0f});
    const glm::vec3 mapped = (clamped * (a * clamped + b)) / (clamped * (c * clamped + d) + e);
    return glm::clamp(mapped, glm::vec3{0.0f}, glm::vec3{1.0f});
}

int half_resolution(int size) {
    return std::max(1, size / 2);
}

}  // namespace eng

#include "engine/rendering/postfx_math.h"

#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using Catch::Approx;

TEST_CASE("luminance uses Rec.709 weights and orders colours sensibly", "[postfx]") {
    CHECK(eng::luminance({0.0f, 0.0f, 0.0f}) == Approx(0.0f));
    CHECK(eng::luminance({1.0f, 1.0f, 1.0f}) == Approx(1.0f));

    // Green reads brightest, blue dimmest, at equal channel intensity.
    const float red = eng::luminance({1.0f, 0.0f, 0.0f});
    const float green = eng::luminance({0.0f, 1.0f, 0.0f});
    const float blue = eng::luminance({0.0f, 0.0f, 1.0f});
    CHECK(green > red);
    CHECK(red > blue);
    CHECK(red + green + blue == Approx(1.0f));

    // A saturated colour must not out-rank a white of the same intensity --
    // this is why bloom thresholds on luminance, not per channel.
    CHECK(eng::luminance({1.0f, 0.0f, 0.0f}) < eng::luminance({1.0f, 1.0f, 1.0f}));
}

TEST_CASE("bloom weight ramps smoothly across the knee", "[postfx]") {
    constexpr float threshold = 1.0f;
    constexpr float knee = 0.4f;

    // Fully outside the band in both directions.
    CHECK(eng::bloom_weight(0.0f, threshold, knee) == Approx(0.0f));
    CHECK(eng::bloom_weight(threshold - knee, threshold, knee) == Approx(0.0f));
    CHECK(eng::bloom_weight(threshold + knee, threshold, knee) == Approx(1.0f));
    CHECK(eng::bloom_weight(50.0f, threshold, knee) == Approx(1.0f));

    // Centre of the band is halfway up the smoothstep.
    CHECK(eng::bloom_weight(threshold, threshold, knee) == Approx(0.5f));

    // Monotonic non-decreasing, and always a valid weight.
    float previous = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const float l = static_cast<float>(i) * 0.02f;
        const float w = eng::bloom_weight(l, threshold, knee);
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
        CHECK(w >= previous - 1e-6f);
        previous = w;
    }
}

TEST_CASE("a zero knee degenerates to a hard cut", "[postfx]") {
    CHECK(eng::bloom_weight(0.99f, 1.0f, 0.0f) == Approx(0.0f));
    CHECK(eng::bloom_weight(1.01f, 1.0f, 0.0f) == Approx(1.0f));
    // Negative knee must not invert the comparison or divide by a negative.
    CHECK(eng::bloom_weight(0.5f, 1.0f, -1.0f) == Approx(0.0f));
    CHECK(eng::bloom_weight(2.0f, 1.0f, -1.0f) == Approx(1.0f));
}

TEST_CASE("ACES tonemap keeps everything inside the display range", "[postfx]") {
    CHECK(eng::aces_tonemap({0.0f, 0.0f, 0.0f}).r == Approx(0.0f));

    // The whole point: open-ended HDR input, bounded output. A Reinhard-style
    // curve would also pass this, but the values below pin down this curve.
    for (const float v : {0.1f, 0.5f, 1.0f, 4.0f, 20.0f, 1000.0f}) {
        const glm::vec3 mapped = eng::aces_tonemap(glm::vec3{v});
        CHECK(mapped.r >= 0.0f);
        CHECK(mapped.r <= 1.0f);
        CHECK(std::isfinite(mapped.r));
    }

    // Monotonic in intensity, so brighter input never renders darker.
    float previous = -1.0f;
    for (int i = 0; i <= 400; ++i) {
        const float v = static_cast<float>(i) * 0.05f;
        const float mapped = eng::aces_tonemap(glm::vec3{v}).g;
        CHECK(mapped >= previous - 1e-6f);
        previous = mapped;
    }

    // Highlights stay separable across the range that actually matters. Note
    // this fit *does* reach 1.0 -- it crosses it near 7.8 and is clamped
    // above that, so do not assert it merely approaches white.
    CHECK(eng::aces_tonemap(glm::vec3{4.0f}).r < 1.0f);
    CHECK(eng::aces_tonemap(glm::vec3{6.0f}).r < 1.0f);
    CHECK(eng::aces_tonemap(glm::vec3{6.0f}).r > eng::aces_tonemap(glm::vec3{4.0f}).r);
    CHECK(eng::aces_tonemap(glm::vec3{4.0f}).r > eng::aces_tonemap(glm::vec3{2.0f}).r);
    CHECK(eng::aces_tonemap(glm::vec3{20.0f}).r == Approx(1.0f));

    // The curve darkens the low end and lifts contrast, so 1.0 in must not
    // come back as 1.0 out; that would leave no headroom for bloom.
    CHECK(eng::aces_tonemap(glm::vec3{1.0f}).r < 1.0f);
    CHECK(eng::aces_tonemap(glm::vec3{1.0f}).r > 0.5f);
}

TEST_CASE("negative HDR values are clamped, not reflected", "[postfx]") {
    // Subtractive blending or a stray NaN-adjacent value must not come back
    // as a positive colour through the rational curve.
    const glm::vec3 mapped = eng::aces_tonemap({-1.0f, -0.5f, 0.0f});
    CHECK(mapped.r == Approx(0.0f));
    CHECK(mapped.g == Approx(0.0f));
    CHECK(mapped.b == Approx(0.0f));
}

TEST_CASE("tonemap treats channels independently", "[postfx]") {
    const glm::vec3 mapped = eng::aces_tonemap({4.0f, 1.0f, 0.25f});
    CHECK(mapped.r > mapped.g);
    CHECK(mapped.g > mapped.b);
    CHECK(mapped.r == Approx(eng::aces_tonemap(glm::vec3{4.0f}).r));
}

TEST_CASE("half resolution never collapses to zero", "[postfx]") {
    CHECK(eng::half_resolution(1920) == 960);
    CHECK(eng::half_resolution(721) == 360);
    // A 1-pixel window must still produce a valid render target size.
    CHECK(eng::half_resolution(1) == 1);
    CHECK(eng::half_resolution(0) == 1);
    CHECK(eng::half_resolution(-4) == 1);
}

}  // namespace

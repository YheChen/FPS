#include "game/shared/player_color.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

// The palette's whole job is that eight figures look like eight people. A
// reviewer nudging a colour because they prefer it cannot see that they have
// just merged two players, or dropped one onto the arena's own green -- so
// these tests measure it instead of trusting the eye.
//
// Thresholds sit well under what the shipped palette achieves (56 / 25 / 26
// against a bar of 35 / 15 / 20). Like the web smoke test, they are here to
// separate "still works" from "badly broken", not to freeze exact numbers.
namespace {

// The arena's own surfaces, roughly: mottled grey concrete floor, tan wall
// panels, blue-grey pillars, green centre platform, and the two ends of the
// sky gradient. Approximated from tools/gen_textures.py and sky.h -- exact
// values are not the point, the neighbourhood is.
constexpr std::array<glm::vec3, 6> kArenaColors{{
    {0.46f, 0.47f, 0.49f},  // floor
    {0.60f, 0.56f, 0.50f},  // wall
    {0.33f, 0.38f, 0.48f},  // pillar
    {0.36f, 0.52f, 0.40f},  // platform
    {0.38f, 0.44f, 0.54f},  // sky, horizon
    {0.09f, 0.18f, 0.40f},  // sky, zenith
}};

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// CIE L*a*b* under D65. Perceptual, unlike RGB distance: two colours 0.2 apart
// in blue are far less different than two 0.2 apart in green, and a test that
// used RGB distance would happily accept a palette nobody can read.
glm::vec3 to_lab(const glm::vec3& rgb) {
    const float r = srgb_to_linear(rgb.r);
    const float g = srgb_to_linear(rgb.g);
    const float b = srgb_to_linear(rgb.b);
    const float x = (0.4124564f * r + 0.3575761f * g + 0.1804375f * b) / 0.95047f;
    const float y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    const float z = (0.0193339f * r + 0.1191920f * g + 0.9503041f * b) / 1.08883f;
    const auto f = [](float t) {
        return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f;
    };
    return {116.0f * f(y) - 16.0f, 500.0f * (f(x) - f(y)), 200.0f * (f(y) - f(z))};
}

float delta_e(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = to_lab(a) - to_lab(b);
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

// Vienot, Brettel & Mollon (1999) dichromat simulation, via the Hunt-Pointer-
// Estevez cone space. Deuteranopia reconstructs the missing M response from L
// and S, which is what makes red and green collapse onto each other.
glm::vec3 simulate_deuteranopia(const glm::vec3& rgb) {
    const float r = srgb_to_linear(rgb.r);
    const float g = srgb_to_linear(rgb.g);
    const float b = srgb_to_linear(rgb.b);
    const float l = 17.8824f * r + 43.5161f * g + 4.11935f * b;
    const float s = 0.0299566f * r + 0.184309f * g + 1.46709f * b;
    const float m = 0.494207f * l + 1.24827f * s;
    const auto encode = [](float v) {
        const float c = std::clamp(v, 0.0f, 1.0f);
        return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    };
    return {encode(0.0809444479f * l - 0.130504409f * m + 0.116721066f * s),
            encode(-0.0102485335f * l + 0.0540193266f * m - 0.113614708f * s),
            encode(-0.000365296938f * l - 0.00412161469f * m + 0.693511405f * s)};
}

}  // namespace

TEST_CASE("a player's colour follows their id, not their arrival", "[player_color]") {
    for (std::uint8_t id = 0; id < game::kMaxPlayers; ++id) {
        CHECK(game::player_color(id) == game::kPlayerColors[id]);
    }
    // The stability that matters: the same id is the same colour on every
    // client and across a rejoin, because nothing else feeds the lookup.
    CHECK(game::player_color(3) == game::player_color(3));

    // kNoPlayer is the world, and 255 % 8 == 7 -- wrapping would dress the
    // world as player 7.
    CHECK(game::player_color(game::kNoPlayer) == game::kNoPlayerColor);
    CHECK(game::player_color(game::kMaxPlayers) == game::kNoPlayerColor);
}

TEST_CASE("every player colour is a usable albedo", "[player_color]") {
    for (const glm::vec3& color : game::kPlayerColors) {
        CHECK(color.r >= 0.0f);
        CHECK(color.g >= 0.0f);
        CHECK(color.b >= 0.0f);
        CHECK(color.r <= 1.0f);
        CHECK(color.g <= 1.0f);
        CHECK(color.b <= 1.0f);
    }
}

TEST_CASE("no two players share a colour anyone could confuse", "[player_color]") {
    float worst = 1e9f;
    for (std::size_t a = 0; a < game::kPlayerColors.size(); ++a) {
        for (std::size_t b = a + 1; b < game::kPlayerColors.size(); ++b) {
            worst = std::min(worst, delta_e(game::kPlayerColors[a], game::kPlayerColors[b]));
        }
    }
    CHECK(worst > 35.0f);
}

// Deuteranopia is ~8% of men, and a palette that separates only along the
// red-green axis is invisible to all of them. Simulating the palette and
// re-measuring is the only way to keep that property through future edits.
TEST_CASE("player colours survive deuteranopia", "[player_color]") {
    float worst = 1e9f;
    for (std::size_t a = 0; a < game::kPlayerColors.size(); ++a) {
        for (std::size_t b = a + 1; b < game::kPlayerColors.size(); ++b) {
            worst = std::min(worst, delta_e(simulate_deuteranopia(game::kPlayerColors[a]),
                                            simulate_deuteranopia(game::kPlayerColors[b])));
        }
    }
    CHECK(worst > 15.0f);
}

// A player who matches the wall behind them is worse than a grey one: they are
// camouflaged rather than merely anonymous.
TEST_CASE("no player colour disappears into the arena", "[player_color]") {
    float worst = 1e9f;
    for (const glm::vec3& player : game::kPlayerColors) {
        for (const glm::vec3& arena : kArenaColors) {
            worst = std::min(worst, delta_e(player, arena));
        }
    }
    CHECK(worst > 20.0f);
}

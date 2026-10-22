#include "game/shared/nav.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include "engine/assets/gltf_loader.h"
#include "engine/assets/paths.h"
#include "engine/rendering/mesh_data.h"
#include "tests/game/boxes.h"

// The navigation grid and the search over it, on maps built here out of a
// few boxes (tests/game/boxes.h) -- the same (unit cube, transform) pairs the
// server builds its physics world from, so what these tests call a wall is
// what the controller would collide with.
namespace {

using Catch::Approx;
using test::box;
using test::floor;
using Meshes = test::BoxMeshes;

float horizontal(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2{a.x - b.x, a.z - b.z});
}

float route_length(const glm::vec3& from, const std::vector<glm::vec3>& points) {
    float length = 0.0f;
    glm::vec3 previous = from;
    for (const glm::vec3& point : points) {
        length += horizontal(previous, point);
        previous = point;
    }
    return length;
}

// Every leg of the route, from `from` through each point, is walkable in a
// straight line -- which is what makes it a route and not a list of places.
void require_legs_walkable(const game::NavGrid& grid, const glm::vec3& from,
                           const std::vector<glm::vec3>& points) {
    auto previous = game::nearest_walkable(grid, from);
    REQUIRE(previous);
    for (const glm::vec3& point : points) {
        const auto cell = grid.cell_of(point);
        REQUIRE(cell);
        CHECK(grid.walkable(*cell));
        CHECK(game::straight_walk_clear(grid, *previous, *cell));
        previous = cell;
    }
}

TEST_CASE("an empty mesh list gives a grid with nothing on it", "[nav]") {
    const game::NavGrid grid = game::build_nav_grid(Meshes{});
    CHECK(grid.empty());
    CHECK(grid.walkable_count() == 0);
    CHECK_FALSE(grid.cell_of({0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(game::nearest_walkable(grid, {0.0f, 0.0f, 0.0f}));
    CHECK(game::find_path(grid, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}).empty());
    CHECK_FALSE(game::random_walkable_point(grid, 1, {0.0f, 0.0f, 0.0f}));
}

TEST_CASE("a flat floor is walkable wherever a cell centre is on it", "[nav]") {
    const game::NavGrid grid = game::build_nav_grid(floor(10.0f));
    REQUIRE_FALSE(grid.empty());

    // Inside the floor: walkable, at floor height. Past its edge: not.
    for (const float x : {-9.75f, -4.25f, 0.25f, 6.75f, 9.75f}) {
        const auto cell = grid.cell_of({x, 0.0f, x});
        REQUIRE(cell);
        CHECK(grid.walkable(*cell));
        CHECK(grid.floor_height(*cell) == Approx(0.0f).margin(1e-4f));
    }
    const auto beyond = grid.cell_of({10.25f, 0.0f, 0.0f});
    REQUIRE(beyond);  // the margin cell exists
    CHECK_FALSE(grid.walkable(*beyond));
    CHECK_FALSE(grid.cell_of({50.0f, 0.0f, 0.0f}));

    // One floor, one region.
    std::uint32_t region = 0;
    for (const std::size_t index : grid.walkable_cells()) {
        const std::uint32_t r = grid.region(grid.cell_at_index(index));
        if (region == 0) {
            region = r;
        }
        CHECK(r == region);
    }

    // Nothing in the way: a route is one leg, ending at the goal's cell.
    const glm::vec3 from{-8.0f, 0.0f, -8.0f};
    const glm::vec3 to{8.0f, 0.0f, 8.0f};
    const auto path = game::find_path(grid, from, to);
    REQUIRE(path.size() == 1);
    CHECK(horizontal(path.back(), to) < grid.cell_size());
    CHECK(path.back().y == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("a wall is not walkable, nor is the agent's radius around it", "[nav]") {
    Meshes meshes = floor(10.0f);
    meshes.push_back(box({0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 10.0f}));  // faces at x = +-0.5
    const game::NavGrid grid = game::build_nav_grid(meshes);

    // Within 0.4 m of its face: not walkable.
    for (const float x : {0.75f, -0.75f}) {
        const auto cell = grid.cell_of({x, 0.0f, 0.0f});
        REQUIRE(cell);
        CHECK_FALSE(grid.walkable(*cell));
    }
    // Under it, the only floor is its top, four metres up.
    for (const float x : {0.25f, -0.25f}) {
        const auto cell = grid.cell_of({x, 0.0f, 0.0f});
        REQUIRE(cell);
        if (grid.walkable(*cell)) {
            CHECK(grid.floor_height(*cell) == Approx(4.0f).margin(1e-4f));
        }
    }
    // A radius and a bit away: walkable, and on the floor rather than the
    // wall's top.
    for (const float x : {1.25f, -1.25f}) {
        const auto cell = grid.cell_of({x, 0.0f, 0.0f});
        REQUIRE(cell);
        CHECK(grid.walkable(*cell));
        CHECK(grid.floor_height(*cell) == Approx(0.0f).margin(1e-4f));
    }
    // The wall's top is a floor of its own, metres above its neighbours and
    // in a region of its own: nothing routes onto it.
    const auto top = grid.cell_of({0.25f, 4.0f, 3.25f});
    REQUIRE(top);
    const auto beside = grid.cell_of({1.25f, 0.0f, 3.25f});
    REQUIRE(beside);
    if (grid.walkable(*top)) {
        CHECK(grid.floor_height(*top) == Approx(4.0f).margin(1e-4f));
        CHECK(grid.region(*top) != grid.region(*beside));
    }
}

TEST_CASE("a route goes round a wall, and every leg of it is walkable", "[nav]") {
    Meshes meshes = floor(15.0f);
    meshes.push_back(box({0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 16.0f}));  // z in [-8, 8]
    const game::NavGrid grid = game::build_nav_grid(meshes);

    const glm::vec3 from{-5.0f, 0.0f, 0.0f};
    const glm::vec3 to{5.0f, 0.0f, 0.0f};
    const auto path = game::find_path(grid, from, to);
    REQUIRE_FALSE(path.empty());
    CHECK(horizontal(path.back(), to) < grid.cell_size());
    require_legs_walkable(grid, from, path);

    // No point of it is inside the wall's band...
    for (const glm::vec3& point : path) {
        const bool in_band = std::abs(point.x) < 0.9f && std::abs(point.z) < 8.4f;
        CHECK_FALSE(in_band);
    }
    // ...and it is a real detour: round an end 8 m away, not through.
    CHECK(route_length(from, path) > 18.0f);
    // Pulled straight: a corner at the wall's end, not forty half-metre
    // steps.
    CHECK(path.size() <= 4);
}

TEST_CASE("the line between two cells knows about the wall between them", "[nav]") {
    Meshes meshes = floor(15.0f);
    meshes.push_back(box({0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 16.0f}));
    const game::NavGrid grid = game::build_nav_grid(meshes);

    const auto cell = [&](float x, float z) {
        const auto c = grid.cell_of({x, 0.0f, z});
        REQUIRE(c);
        REQUIRE(grid.walkable(*c));
        return *c;
    };
    CHECK_FALSE(game::straight_walk_clear(grid, cell(-3.0f, 0.0f), cell(3.0f, 0.0f)));
    CHECK_FALSE(game::straight_walk_clear(grid, cell(-3.0f, -5.0f), cell(3.0f, 5.0f)));
    CHECK(game::straight_walk_clear(grid, cell(-3.0f, 0.0f), cell(-3.0f, 9.0f)));
    CHECK(game::straight_walk_clear(grid, cell(-3.0f, -10.0f), cell(3.0f, -10.0f)));
    CHECK(game::straight_walk_clear(grid, cell(-3.0f, -10.0f), cell(3.0f, -12.0f)));
    // A cell is clear to itself, and a diagonal across open floor is clear.
    CHECK(game::straight_walk_clear(grid, cell(5.0f, 5.0f), cell(5.0f, 5.0f)));
    CHECK(game::straight_walk_clear(grid, cell(2.0f, 2.0f), cell(9.0f, 9.0f)));
}

TEST_CASE("a sealed room has no route in, and its own region", "[nav]") {
    Meshes meshes = floor(20.0f);
    // Four walls, 0.5 m thick, enclosing x, z in [-3, 3].
    meshes.push_back(box({0.0f, 2.0f, -3.25f}, {7.0f, 4.0f, 0.5f}));
    meshes.push_back(box({0.0f, 2.0f, 3.25f}, {7.0f, 4.0f, 0.5f}));
    meshes.push_back(box({-3.25f, 2.0f, 0.0f}, {0.5f, 4.0f, 7.0f}));
    meshes.push_back(box({3.25f, 2.0f, 0.0f}, {0.5f, 4.0f, 7.0f}));
    const game::NavGrid grid = game::build_nav_grid(meshes);

    const glm::vec3 outside{10.0f, 0.0f, 10.0f};
    const glm::vec3 inside{0.0f, 0.0f, 0.0f};
    CHECK(game::find_path(grid, outside, inside).empty());
    CHECK(game::find_path(grid, inside, outside).empty());
    CHECK_FALSE(game::find_path(grid, inside, {1.5f, 0.0f, -1.5f}).empty());
    CHECK_FALSE(game::find_path(grid, outside, {-10.0f, 0.0f, 5.0f}).empty());

    const auto in = game::nearest_walkable(grid, inside);
    const auto out = game::nearest_walkable(grid, outside);
    REQUIRE(in);
    REQUIRE(out);
    CHECK(grid.region(*in) != grid.region(*out));

    // Roaming from either side stays on that side.
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        const auto from_outside = game::random_walkable_point(grid, seed, outside);
        REQUIRE(from_outside);
        CHECK(grid.region(*game::nearest_walkable(grid, *from_outside)) == grid.region(*out));
        const auto from_inside = game::random_walkable_point(grid, seed, inside);
        REQUIRE(from_inside);
        CHECK(grid.region(*game::nearest_walkable(grid, *from_inside)) == grid.region(*in));
        CHECK(std::abs(from_inside->x) < 3.0f);
        CHECK(std::abs(from_inside->z) < 3.0f);
    }
}

TEST_CASE("a ramp joins the floor to a platform", "[nav]") {
    // arena01's centre platform and east ramp, to the metre.
    Meshes meshes = floor(20.0f);
    meshes.push_back(box({0.0f, 0.75f, 0.0f}, {6.0f, 1.5f, 6.0f}));
    meshes.push_back(box({4.8f, 0.65f, 0.0f}, {4.2f, 0.3f, 3.0f}, -16.0f));
    const game::NavGrid grid = game::build_nav_grid(meshes);

    const glm::vec3 on_floor{12.0f, 0.0f, 0.0f};
    const glm::vec3 on_platform{0.0f, 1.5f, 0.0f};
    const auto up = game::find_path(grid, on_floor, on_platform);
    REQUIRE_FALSE(up.empty());
    require_legs_walkable(grid, on_floor, up);
    CHECK(up.back().y == Approx(1.5f).margin(0.05f));
    // It climbs: the cells along the way pass through ramp heights.
    bool climbed = false;
    auto previous = *game::nearest_walkable(grid, on_floor);
    for (const glm::vec3& point : up) {
        const auto cell = *grid.cell_of(point);
        // Walk the leg cell by cell and look at the heights.
        const int steps = std::max(std::abs(cell.x - previous.x), std::abs(cell.y - previous.y));
        for (int i = 1; i <= steps; ++i) {
            const glm::ivec2 at{previous.x + (cell.x - previous.x) * i / steps,
                                previous.y + (cell.y - previous.y) * i / steps};
            if (grid.walkable(at) && grid.floor_height(at) > 0.3f && grid.floor_height(at) < 1.3f) {
                climbed = true;
            }
        }
        previous = cell;
    }
    CHECK(climbed);
    // And back down the same way.
    const auto down = game::find_path(grid, on_platform, on_floor);
    REQUIRE_FALSE(down.empty());
    require_legs_walkable(grid, on_platform, down);
    CHECK(down.back().y == Approx(0.0f).margin(0.05f));

    // The platform's other sides are lips, not routes: a point just beyond
    // the west edge is reached by the ramp, not by stepping off.
    const glm::vec3 west_of_platform{-6.0f, 0.0f, 0.0f};
    const auto west = game::find_path(grid, on_platform, west_of_platform);
    REQUIRE_FALSE(west.empty());
    CHECK(route_length(on_platform, west) > 12.0f);
    // One region: the platform is part of the floor's world.
    CHECK(grid.region(*game::nearest_walkable(grid, on_platform)) ==
          grid.region(*game::nearest_walkable(grid, on_floor)));
}

TEST_CASE("a step within the controller's reach is walked over", "[nav]") {
    Meshes meshes = floor(10.0f);
    meshes.push_back(box({0.0f, 0.15f, 0.0f}, {4.0f, 0.3f, 4.0f}));  // 0.3 m high
    const game::NavGrid grid = game::build_nav_grid(meshes);
    const auto on_step = grid.cell_of({0.25f, 0.3f, 0.25f});
    REQUIRE(on_step);
    CHECK(grid.walkable(*on_step));
    CHECK(grid.floor_height(*on_step) == Approx(0.3f).margin(1e-4f));
    const auto path = game::find_path(grid, {-6.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f});
    REQUIRE(path.size() == 1);  // straight over it
    // A 1.2 m crate, the arenas' cover height, is not stepped onto.
    meshes.push_back(box({0.0f, 0.6f, -6.0f}, {2.0f, 1.2f, 2.0f}));
    const game::NavGrid with_crate = game::build_nav_grid(meshes);
    const auto crate_top = with_crate.cell_of({0.25f, 1.2f, -6.0f});
    REQUIRE(crate_top);
    const auto beside_crate = with_crate.cell_of({3.0f, 0.0f, -6.0f});
    REQUIRE(beside_crate);
    CHECK(with_crate.walkable(*beside_crate));
    if (with_crate.walkable(*crate_top)) {
        CHECK(with_crate.region(*crate_top) != with_crate.region(*beside_crate));
    }
}

TEST_CASE("nearest_walkable steps out of the wall band and keeps its height", "[nav]") {
    Meshes meshes = floor(20.0f);
    meshes.push_back(box({0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 10.0f}));
    meshes.push_back(box({10.0f, 0.75f, 0.0f}, {6.0f, 1.5f, 6.0f}));  // platform, x in [7, 13]
    const game::NavGrid grid = game::build_nav_grid(meshes);

    // Pressed against the wall: the nearest cell is off it, on the floor.
    const auto beside_wall = game::nearest_walkable(grid, {0.6f, 0.0f, 0.0f});
    REQUIRE(beside_wall);
    const glm::vec3 center = grid.center_of(*beside_wall);
    CHECK(center.x >= 0.9f);
    CHECK(center.y == Approx(0.0f).margin(1e-4f));
    CHECK(horizontal(center, {0.6f, 0.0f, 0.0f}) < 1.0f);

    // At the platform's edge: the platform, not the floor 1.5 m below that is
    // as close sideways.
    const auto at_edge = game::nearest_walkable(grid, {7.1f, 1.5f, 0.0f});
    REQUIRE(at_edge);
    CHECK(grid.floor_height(*at_edge) == Approx(1.5f).margin(1e-4f));

    // Off the grid altogether still finds the edge of the world.
    const auto far = game::nearest_walkable(grid, {100.0f, 0.0f, 0.0f});
    REQUIRE(far);
    CHECK(grid.center_of(*far).x > 19.0f);
    // Too far from anything walkable at its own height: nothing. The wall's
    // top is within reach sideways, and four metres up.
    CHECK_FALSE(game::nearest_walkable(grid, {0.0f, 0.0f, 0.0f}, 0.4f));
    CHECK_FALSE(game::nearest_walkable(grid, {0.0f, 0.0f, 0.0f}, 0.6f));
}

TEST_CASE("routes are a function of their inputs", "[nav]") {
    Meshes meshes = floor(15.0f);
    meshes.push_back(box({0.0f, 2.0f, 0.0f}, {1.0f, 4.0f, 16.0f}));
    meshes.push_back(box({6.0f, 1.5f, 6.0f}, {2.0f, 3.0f, 2.0f}));
    const game::NavGrid grid = game::build_nav_grid(meshes);
    const game::NavGrid again = game::build_nav_grid(meshes);
    const glm::vec3 from{-9.0f, 0.0f, 3.0f};
    const glm::vec3 to{9.0f, 0.0f, 9.0f};
    const auto a = game::find_path(grid, from, to);
    const auto b = game::find_path(again, from, to);
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i] == b[i]);
    }
    // Different seeds roam to different places, the same seed to the same.
    const auto p1 = game::random_walkable_point(grid, 1, from);
    const auto p2 = game::random_walkable_point(grid, 2, from);
    const auto p1_again = game::random_walkable_point(grid, 1, from);
    REQUIRE(p1);
    REQUIRE(p2);
    CHECK(*p1 == *p1_again);
    CHECK(*p1 != *p2);
}

TEST_CASE("triangle_intersects_box", "[nav]") {
    const glm::vec3 center{0.0f, 0.0f, 0.0f};
    const glm::vec3 half{1.0f, 1.0f, 1.0f};
    // Through the middle.
    CHECK(game::triangle_intersects_box({-2.0f, 0.0f, -2.0f}, {2.0f, 0.0f, -2.0f},
                                        {0.0f, 0.0f, 2.0f}, center, half));
    // A huge triangle whose plane passes through the box: the box is well
    // inside the triangle's extent, so no vertex is in the box.
    CHECK(game::triangle_intersects_box({-50.0f, 0.3f, -50.0f}, {50.0f, 0.3f, -50.0f},
                                        {0.0f, 0.3f, 50.0f}, center, half));
    // The same plane, above the box.
    CHECK_FALSE(game::triangle_intersects_box({-50.0f, 1.5f, -50.0f}, {50.0f, 1.5f, -50.0f},
                                              {0.0f, 1.5f, 50.0f}, center, half));
    // Beside the box, overlapping in two axes but not the third.
    CHECK_FALSE(game::triangle_intersects_box({3.0f, -5.0f, -5.0f}, {3.0f, 5.0f, -5.0f},
                                              {3.0f, 0.0f, 5.0f}, center, half));
    // Clipping a corner: the triangle's plane cuts the corner off.
    CHECK(game::triangle_intersects_box({1.5f, 0.0f, 0.0f}, {0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, 1.5f},
                                        center, half));
    // Just missing that corner.
    CHECK_FALSE(game::triangle_intersects_box({3.2f, 0.0f, 0.0f}, {0.0f, 3.2f, 0.0f},
                                              {0.0f, 0.0f, 3.2f}, center, half));
}

// The shipped arenas, loaded the way game/server/main.cpp loads them. A new
// map whose ramps do not connect, or whose spawns sit somewhere the grid
// cannot reach, fails here before a bot ever stands on it.
std::optional<std::pair<Meshes, std::vector<glm::vec3>>> load_arena(const std::string& name) {
    const auto root = eng::find_assets_root();
    if (!root) {
        return std::nullopt;
    }
    const auto model = eng::load_gltf(*root / "maps" / name, /*decode_images=*/false);
    if (!model) {
        return std::nullopt;
    }
    Meshes meshes;
    std::vector<glm::vec3> spawns;
    for (const eng::GltfNode& node : model->nodes) {
        if (node.name.starts_with("spawn_")) {
            spawns.emplace_back(node.transform[3]);
        }
        if (node.mesh < 0) {
            continue;
        }
        for (const eng::GltfPrimitive& primitive :
             model->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
            meshes.emplace_back(primitive.mesh, node.transform);
        }
    }
    return std::pair{std::move(meshes), std::move(spawns)};
}

TEST_CASE("every spawn of every shipped arena can reach every other", "[nav][assets]") {
    for (const std::string name : {"arena01.glb", "arena02.glb"}) {
        INFO(name);
        const auto arena = load_arena(name);
        REQUIRE(arena);
        const auto& [meshes, spawns] = *arena;
        REQUIRE(spawns.size() == 8);
        const game::NavGrid grid = game::build_nav_grid(meshes);
        REQUIRE_FALSE(grid.empty());
        // Most of the arena is floor: a grid that eroded it all away would
        // still "connect" eight spawns that all snapped to nothing.
        CHECK(grid.walkable_count() > 2000);
        for (std::size_t i = 0; i < spawns.size(); ++i) {
            for (std::size_t j = 0; j < spawns.size(); ++j) {
                if (i == j) {
                    continue;
                }
                INFO("spawn " << i << " -> spawn " << j);
                const auto path = game::find_path(grid, spawns[i], spawns[j]);
                REQUIRE_FALSE(path.empty());
                require_legs_walkable(grid, spawns[i], path);
                CHECK(horizontal(path.back(), spawns[j]) < 2.0f);
                // A spawn on a gallery is reached up a ramp, so the route
                // ends at gallery height, not on the floor beneath it.
                CHECK(path.back().y == Approx(spawns[j].y - 0.1f).margin(0.25f));
            }
        }
    }
}

}  // namespace

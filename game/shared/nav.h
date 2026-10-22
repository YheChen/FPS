#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/mesh_data.h"

// Navigation (M58): a walkability grid built from the map's collision
// triangles, and A* over it.
//
// Bots used to steer by one ray: walk at the target, back off when something
// is within 2.2 m, wander on a random heading otherwise. That is enough to
// cross an open floor and not enough to go round a pillar, up a ramp, or
// through the lane behind a gallery -- and an enemy behind any of those was an
// enemy the bot slid along a wall toward forever.
//
// The grid is derived from the same (mesh, transform) pairs the physics world
// is built from, so it describes the arena that is actually there rather than
// a hand-placed waypoint set that drifts the first time a box moves. Each
// cell records the height of the highest upward-facing surface under its
// centre (one floor per cell: no bridges over walkways, which these maps do
// not have) and whether a standing agent fits there, tested as a box the
// agent's size against every triangle. Cells are joined when the floor
// difference between them is within what the character controller can step
// up, so a ramp reads as a run of connected cells and a platform lip as the
// edge of the walkable world.
//
// Everything here is a pure function of the geometry and its arguments. No
// physics world, no server, no file I/O: a test hands it a few boxes.
namespace game {

// What counts as standing room. Defaults match eng::CharacterConfig and the
// controller's stair step, which is the point: a cell the grid calls walkable
// is one the controller can actually stand on.
struct NavParams {
    float cell_size = 0.5f;
    float agent_radius = 0.4f;
    float agent_height = 1.8f;
    // Largest rise between two adjacent cells the agent can walk up, and the
    // largest fall it is routed down. Symmetric on purpose: the cells beside
    // any ledge taller than this sit inside the band the agent radius erodes
    // (the ledge's face is within the radius of their centres), so a drop is
    // never between two walkable cells anyway. Bots take ramps.
    float step_height = 0.4f;
    float max_slope_degrees = 50.0f;
};

class NavGrid {
public:
    // An empty grid: nothing is walkable, every query says so. What a server
    // with no geometry gets, and what every bot falls back to its old steering
    // on.
    NavGrid() = default;

    int width() const { return width_; }
    int depth() const { return depth_; }
    float cell_size() const { return cell_size_; }
    std::size_t walkable_count() const { return walkable_cells_.size(); }
    bool empty() const { return walkable_cells_.empty(); }

    bool in_bounds(glm::ivec2 cell) const {
        return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < depth_;
    }
    bool walkable(glm::ivec2 cell) const {
        return in_bounds(cell) && walkable_[index_of(cell)] != 0;
    }
    // Only meaningful for a walkable cell.
    float floor_height(glm::ivec2 cell) const { return floor_[index_of(cell)]; }
    // Which connected patch of floor a cell belongs to, or 0 when it is not
    // walkable. The tops of walls and pillars are regions of their own, which
    // is how a roaming bot avoids being sent somewhere it could only reach by
    // flying.
    std::uint32_t region(glm::ivec2 cell) const {
        return walkable(cell) ? region_[index_of(cell)] : 0u;
    }

    // The cell under a world position, by X and Z alone; nullopt outside the
    // grid. Says nothing about walkability.
    std::optional<glm::ivec2> cell_of(const glm::vec3& position) const;
    // The world point at a cell's centre, on its floor.
    glm::vec3 center_of(glm::ivec2 cell) const;

    // Whether an agent standing on `from` can walk straight onto `to`: both
    // walkable, 8-adjacent, the floors within a step of each other. A
    // diagonal additionally needs both cells it passes the corner of, so a
    // route never clips the edge of a wall.
    bool can_step(glm::ivec2 from, glm::ivec2 to) const;

    // Every walkable cell, in index order, and the walkable cells of one
    // region. Exposed for random choice and for tests that want to assert on
    // the whole grid.
    std::span<const std::size_t> walkable_cells() const { return walkable_cells_; }
    std::span<const std::size_t> region_cells(std::uint32_t region) const;
    glm::ivec2 cell_at_index(std::size_t index) const {
        return {static_cast<int>(index % static_cast<std::size_t>(width_)),
                static_cast<int>(index / static_cast<std::size_t>(width_))};
    }
    // Callers pass in-bounds cells; the arithmetic is only meaningful then.
    std::size_t index_of(glm::ivec2 cell) const {
        return static_cast<std::size_t>(cell.y * width_ + cell.x);
    }

private:
    friend NavGrid build_nav_grid(std::span<const std::pair<eng::MeshData, glm::mat4>> meshes,
                                  const NavParams& params);

    glm::vec2 origin_{0.0f};  // world X/Z of the corner of cell (0, 0)
    float cell_size_ = 0.5f;
    float step_height_ = 0.4f;
    int width_ = 0;
    int depth_ = 0;
    std::vector<float> floor_;
    std::vector<std::uint8_t> walkable_;
    std::vector<std::uint32_t> region_;
    std::vector<std::size_t> walkable_cells_;
    // region_cells_[r] is the cells of region r + 1 (region 0 is "none").
    std::vector<std::vector<std::size_t>> region_cells_;
};

// Rasterizes the collision geometry into a grid. The grid covers the
// geometry's X/Z extent with one cell of margin, so a position off the edge
// of the map is simply off the grid.
//
// Geometry too large for the cell size (over a few million cells) produces an
// empty grid and a warning rather than a very long startup: the bots then
// steer the way they did before this existed.
NavGrid build_nav_grid(std::span<const std::pair<eng::MeshData, glm::mat4>> meshes,
                       const NavParams& params = {});

// Separating-axis test between a triangle and an axis-aligned box. Public
// because it is the one piece of geometry in here worth testing on its own:
// a wrong answer makes a wall walkable or a floor an obstacle.
bool triangle_intersects_box(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                             const glm::vec3& box_center, const glm::vec3& box_half_extents);

// The walkable cell nearest to a world position, searching up to
// `max_distance` metres sideways and preferring a floor near the position's
// own height. A player hugging a wall stands inside the band the agent radius
// erodes, so "the cell under their feet" is routinely not walkable; this is
// how both ends of a route land on the grid.
std::optional<glm::ivec2> nearest_walkable(const NavGrid& grid, const glm::vec3& position,
                                           float max_distance = 2.0f);

// Whether the straight line between two cell centres can be walked: every
// cell it crosses is steppable from the one before. The routes find_path
// returns are pulled straight with this, so a bot walks a few long legs
// rather than a staircase of half-metre cells.
bool straight_walk_clear(const NavGrid& grid, glm::ivec2 from, glm::ivec2 to);

// A* from `from` to `to`. Returns the points to walk through in order, not
// including where the bot already is and ending at `to`'s cell; empty when
// either end is off the grid or there is no route. Deterministic: the same
// grid and endpoints give the same route, which replays and tests rely on.
std::vector<glm::vec3> find_path(const NavGrid& grid, const glm::vec3& from, const glm::vec3& to);

// A walkable point chosen by `seed` from the region `near` stands in, for a
// bot with nowhere in particular to be. nullopt when `near` is nowhere near
// the grid, or the grid is empty.
std::optional<glm::vec3> random_walkable_point(const NavGrid& grid, std::uint32_t seed,
                                               const glm::vec3& near);

}  // namespace game

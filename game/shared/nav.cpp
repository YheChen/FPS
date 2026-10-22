#include "game/shared/nav.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

#include "engine/core/log.h"
#include "game/shared/rng.h"

namespace game {

namespace {

constexpr float kSqrt2 = 1.41421356f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();
// Above this the grid is refused. 2048 x 2048 cells at half a metre is a
// kilometre square, which no arena here is anywhere near; it is a guard
// against a mesh with one stray vertex far from the rest turning startup
// into a minute of rasterizing nothing.
constexpr std::int64_t kMaxCells = 1 << 22;

struct Triangle {
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 c;
    glm::vec3 min;
    glm::vec3 max;
    glm::vec3 normal;  // unit, or zero for a degenerate triangle
};

std::vector<Triangle> world_triangles(std::span<const std::pair<eng::MeshData, glm::mat4>> meshes) {
    std::vector<Triangle> triangles;
    for (const auto& [mesh, transform] : meshes) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            Triangle t;
            t.a = glm::vec3{transform * glm::vec4{mesh.vertices[mesh.indices[i]].position, 1.0f}};
            t.b =
                glm::vec3{transform * glm::vec4{mesh.vertices[mesh.indices[i + 1]].position, 1.0f}};
            t.c =
                glm::vec3{transform * glm::vec4{mesh.vertices[mesh.indices[i + 2]].position, 1.0f}};
            t.min = glm::min(t.a, glm::min(t.b, t.c));
            t.max = glm::max(t.a, glm::max(t.b, t.c));
            const glm::vec3 cross = glm::cross(t.b - t.a, t.c - t.a);
            const float length = glm::length(cross);
            t.normal = length > 1e-8f ? cross / length : glm::vec3{0.0f};
            triangles.push_back(t);
        }
    }
    return triangles;
}

// Whether (x, z) lies inside the triangle's projection onto the ground
// plane, edges included. Inclusive so a cell centre exactly on the seam
// between two floor triangles belongs to both -- they agree on the height
// there, and a seam of missing floor across the middle of every box would
// otherwise cut the arena in two.
bool contains_xz(const Triangle& t, float x, float z) {
    const auto edge = [](const glm::vec3& p, const glm::vec3& q, float px, float pz) {
        return (q.x - p.x) * (pz - p.z) - (q.z - p.z) * (px - p.x);
    };
    const float e0 = edge(t.a, t.b, x, z);
    const float e1 = edge(t.b, t.c, x, z);
    const float e2 = edge(t.c, t.a, x, z);
    constexpr float kTolerance = 1e-5f;
    const bool all_non_negative = e0 >= -kTolerance && e1 >= -kTolerance && e2 >= -kTolerance;
    const bool all_non_positive = e0 <= kTolerance && e1 <= kTolerance && e2 <= kTolerance;
    return all_non_negative || all_non_positive;
}

// Height of the triangle's plane at (x, z). Only called for triangles whose
// normal has a real vertical component.
float height_at(const Triangle& t, float x, float z) {
    return t.a.y - (t.normal.x * (x - t.a.x) + t.normal.z * (z - t.a.z)) / t.normal.y;
}

float horizontal_distance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2{a.x - b.x, a.z - b.z});
}

}  // namespace

bool triangle_intersects_box(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                             const glm::vec3& box_center, const glm::vec3& box_half_extents) {
    // Akenine-Moller: thirteen separating axes. The triangle is moved into
    // the box's frame first so the box is centred on the origin.
    const glm::vec3 v0 = a - box_center;
    const glm::vec3 v1 = b - box_center;
    const glm::vec3 v2 = c - box_center;
    const glm::vec3& h = box_half_extents;

    const auto separated_on = [&](const glm::vec3& axis) {
        const float p0 = glm::dot(v0, axis);
        const float p1 = glm::dot(v1, axis);
        const float p2 = glm::dot(v2, axis);
        const float radius =
            h.x * std::abs(axis.x) + h.y * std::abs(axis.y) + h.z * std::abs(axis.z);
        return std::max({p0, p1, p2}) < -radius || std::min({p0, p1, p2}) > radius;
    };

    // The box's own axes: a plain bounds overlap.
    if (std::max({v0.x, v1.x, v2.x}) < -h.x || std::min({v0.x, v1.x, v2.x}) > h.x ||
        std::max({v0.y, v1.y, v2.y}) < -h.y || std::min({v0.y, v1.y, v2.y}) > h.y ||
        std::max({v0.z, v1.z, v2.z}) < -h.z || std::min({v0.z, v1.z, v2.z}) > h.z) {
        return false;
    }

    // Nine cross products of triangle edges with box axes.
    const glm::vec3 edges[3] = {v1 - v0, v2 - v1, v0 - v2};
    const glm::vec3 box_axes[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    for (const glm::vec3& edge : edges) {
        for (const glm::vec3& box_axis : box_axes) {
            const glm::vec3 axis = glm::cross(edge, box_axis);
            if (glm::dot(axis, axis) < 1e-12f) {
                continue;  // edge parallel to this box axis: no information
            }
            if (separated_on(axis)) {
                return false;
            }
        }
    }

    // The triangle's own plane.
    return !separated_on(glm::cross(edges[0], edges[1]));
}

std::optional<glm::ivec2> NavGrid::cell_of(const glm::vec3& position) const {
    if (width_ == 0 || depth_ == 0) {
        return std::nullopt;
    }
    const glm::ivec2 cell{static_cast<int>(std::floor((position.x - origin_.x) / cell_size_)),
                          static_cast<int>(std::floor((position.z - origin_.y) / cell_size_))};
    if (!in_bounds(cell)) {
        return std::nullopt;
    }
    return cell;
}

glm::vec3 NavGrid::center_of(glm::ivec2 cell) const {
    return {origin_.x + (static_cast<float>(cell.x) + 0.5f) * cell_size_, floor_[index_of(cell)],
            origin_.y + (static_cast<float>(cell.y) + 0.5f) * cell_size_};
}

bool NavGrid::can_step(glm::ivec2 from, glm::ivec2 to) const {
    if (!walkable(from) || !walkable(to)) {
        return false;
    }
    const glm::ivec2 delta = to - from;
    if (delta == glm::ivec2{0, 0} || std::abs(delta.x) > 1 || std::abs(delta.y) > 1) {
        return false;
    }
    const auto rise_ok = [&](glm::ivec2 a, glm::ivec2 b) {
        return std::abs(floor_[index_of(b)] - floor_[index_of(a)]) <= step_height_;
    };
    if (!rise_ok(from, to)) {
        return false;
    }
    if (delta.x != 0 && delta.y != 0) {
        // The two cells the diagonal passes the corner of. Both must be
        // steppable too, or the route cuts through the edge of a wall.
        const glm::ivec2 side_x{from.x + delta.x, from.y};
        const glm::ivec2 side_z{from.x, from.y + delta.y};
        if (!walkable(side_x) || !walkable(side_z) || !rise_ok(from, side_x) ||
            !rise_ok(from, side_z)) {
            return false;
        }
    }
    return true;
}

NavGrid build_nav_grid(std::span<const std::pair<eng::MeshData, glm::mat4>> meshes,
                       const NavParams& params) {
    NavGrid grid;
    grid.cell_size_ = params.cell_size;
    grid.step_height_ = params.step_height;

    const std::vector<Triangle> triangles = world_triangles(meshes);
    if (triangles.empty() || params.cell_size <= 0.0f) {
        return grid;
    }

    glm::vec3 min{kInfinity};
    glm::vec3 max{-kInfinity};
    for (const Triangle& t : triangles) {
        min = glm::min(min, t.min);
        max = glm::max(max, t.max);
    }
    // One cell of margin on every side, and the origin snapped to the cell
    // size so cell edges fall on the same world coordinates whatever the
    // geometry's exact extent is.
    const float cell = params.cell_size;
    grid.origin_ = {std::floor(min.x / cell) * cell - cell, std::floor(min.z / cell) * cell - cell};
    const std::int64_t width =
        static_cast<std::int64_t>(std::ceil((max.x - grid.origin_.x) / cell)) + 1;
    const std::int64_t depth =
        static_cast<std::int64_t>(std::ceil((max.z - grid.origin_.y) / cell)) + 1;
    if (width <= 0 || depth <= 0 || width * depth > kMaxCells) {
        eng::log::warn("Nav grid: geometry spans {}x{} cells of {} m; refusing to build it", width,
                       depth, cell);
        return NavGrid{};
    }
    grid.width_ = static_cast<int>(width);
    grid.depth_ = static_cast<int>(depth);
    const std::size_t count = static_cast<std::size_t>(width * depth);
    grid.floor_.assign(count, -kInfinity);
    grid.walkable_.assign(count, 0);

    // Pass one: the floor under each cell centre, the HIGHEST upward-facing
    // surface there. Walls and pillars have tops, so their cells get a floor
    // too -- an unreachable one, metres above the neighbours, which the step
    // rule leaves disconnected.
    const float min_normal_y = std::cos(glm::radians(params.max_slope_degrees));
    const auto cell_range = [&](float lo, float hi, float origin, int limit, int& first,
                                int& last) {
        first = std::clamp(static_cast<int>(std::floor((lo - origin) / cell)), 0, limit - 1);
        last = std::clamp(static_cast<int>(std::floor((hi - origin) / cell)), 0, limit - 1);
    };
    for (const Triangle& t : triangles) {
        if (t.normal.y < min_normal_y) {
            continue;
        }
        int x0 = 0;
        int x1 = 0;
        int z0 = 0;
        int z1 = 0;
        cell_range(t.min.x, t.max.x, grid.origin_.x, grid.width_, x0, x1);
        cell_range(t.min.z, t.max.z, grid.origin_.y, grid.depth_, z0, z1);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const glm::vec3 center = grid.center_of({x, z});
                if (!contains_xz(t, center.x, center.z)) {
                    continue;
                }
                float& floor = grid.floor_[grid.index_of({x, z})];
                floor = std::max(floor, height_at(t, center.x, center.z));
            }
        }
    }

    // Pass two: standing room. A cell with a floor is walkable unless some
    // triangle intersects the box an agent would occupy standing on it --
    // from step height up (anything lower is stepped over) to head height,
    // and the agent's radius to either side. The radius is what keeps routes
    // off walls: the cells within it of a wall face are not walkable, so a
    // path never asks the capsule to overlap one.
    const glm::vec3 half{params.agent_radius, 0.5f * (params.agent_height - params.step_height),
                         params.agent_radius};
    for (const Triangle& t : triangles) {
        int x0 = 0;
        int x1 = 0;
        int z0 = 0;
        int z1 = 0;
        cell_range(t.min.x - params.agent_radius, t.max.x + params.agent_radius, grid.origin_.x,
                   grid.width_, x0, x1);
        cell_range(t.min.z - params.agent_radius, t.max.z + params.agent_radius, grid.origin_.y,
                   grid.depth_, z0, z1);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const std::size_t index = grid.index_of({x, z});
                const float floor = grid.floor_[index];
                if (floor == -kInfinity || grid.walkable_[index] == 2) {
                    continue;  // no floor, or already known to be blocked
                }
                // Cheap vertical reject first: the floor's own triangles sit
                // below every box, and they are the ones covering every cell.
                const float bottom = floor + params.step_height;
                const float top = floor + params.agent_height;
                if (t.max.y <= bottom || t.min.y >= top) {
                    continue;
                }
                const glm::vec3 center = grid.center_of({x, z});
                const glm::vec3 box_center{center.x, 0.5f * (bottom + top), center.z};
                if (triangle_intersects_box(t.a, t.b, t.c, box_center, half)) {
                    grid.walkable_[index] = 2;  // blocked, and no need to test it again
                }
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const bool walkable = grid.floor_[i] != -kInfinity && grid.walkable_[i] != 2;
        grid.walkable_[i] = walkable ? 1 : 0;
        if (walkable) {
            grid.walkable_cells_.push_back(i);
        }
    }

    // Pass three: regions, by flood fill over the step relation. A gallery
    // and the floor below it share one because there is a ramp between them;
    // a wall top, metres above anything beside it, is a region of its own.
    grid.region_.assign(count, 0);
    std::vector<std::size_t> frontier;
    constexpr glm::ivec2 kNeighbours[8] = {{1, 0}, {-1, 0}, {0, 1},  {0, -1},
                                           {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (const std::size_t seed : grid.walkable_cells_) {
        if (grid.region_[seed] != 0) {
            continue;
        }
        const auto region = static_cast<std::uint32_t>(grid.region_cells_.size() + 1);
        grid.region_cells_.emplace_back();
        grid.region_[seed] = region;
        frontier.assign(1, seed);
        while (!frontier.empty()) {
            const std::size_t index = frontier.back();
            frontier.pop_back();
            grid.region_cells_.back().push_back(index);
            const glm::ivec2 current = grid.cell_at_index(index);
            for (const glm::ivec2& offset : kNeighbours) {
                const glm::ivec2 next = current + offset;
                if (!grid.walkable(next) || grid.region_[grid.index_of(next)] != 0) {
                    continue;
                }
                if (grid.can_step(current, next)) {
                    grid.region_[grid.index_of(next)] = region;
                    frontier.push_back(grid.index_of(next));
                }
            }
        }
        std::sort(grid.region_cells_.back().begin(), grid.region_cells_.back().end());
    }
    return grid;
}

std::span<const std::size_t> NavGrid::region_cells(std::uint32_t region) const {
    if (region == 0 || region > region_cells_.size()) {
        return {};
    }
    return region_cells_[region - 1];
}

std::optional<glm::ivec2> nearest_walkable(const NavGrid& grid, const glm::vec3& position,
                                           float max_distance) {
    if (grid.empty()) {
        return std::nullopt;
    }
    // Off the grid entirely still gets the nearest cell on it, measured from
    // the grid's edge, so a player pressed into the boundary wall is not
    // unreachable.
    const glm::vec3 low = grid.center_of({0, 0});
    const glm::vec3 high = grid.center_of({grid.width() - 1, grid.depth() - 1});
    const glm::vec3 anchor{std::clamp(position.x, low.x, high.x), position.y,
                           std::clamp(position.z, low.z, high.z)};
    const auto cell = grid.cell_of(anchor);
    if (!cell) {
        return std::nullopt;
    }
    // A cell more than this above or below the position is somewhere else: a
    // wall's top beside a player on the floor, or the floor beneath one who
    // has just stepped off a gallery. Two metres clears a jump and the
    // tallest ledge in the arenas, and rejects every wall and pillar top.
    constexpr float kMaxHeightDifference = 2.0f;
    if (grid.walkable(*cell) &&
        std::abs(grid.floor_height(*cell) - anchor.y) <= kMaxHeightDifference) {
        return cell;
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(max_distance / grid.cell_size())));
    std::optional<glm::ivec2> best;
    float best_score = kInfinity;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const glm::ivec2 candidate{cell->x + dx, cell->y + dz};
            if (!grid.walkable(candidate)) {
                continue;
            }
            const glm::vec3 center = grid.center_of(candidate);
            const float sideways = horizontal_distance(center, anchor);
            const float vertical = std::abs(center.y - anchor.y);
            if (sideways > max_distance || vertical > kMaxHeightDifference) {
                continue;
            }
            // Height counts double: a player at a gallery's edge belongs to
            // the gallery, not to the floor cell a metre and a half below
            // that happens to be as close sideways.
            const float score = sideways * sideways + 4.0f * vertical * vertical;
            if (score < best_score) {
                best_score = score;
                best = candidate;
            }
        }
    }
    return best;
}

bool straight_walk_clear(const NavGrid& grid, glm::ivec2 from, glm::ivec2 to) {
    if (!grid.walkable(from) || !grid.walkable(to)) {
        return false;
    }
    if (from == to) {
        return true;
    }
    // Amanatides-Woo traversal from centre to centre, crossing one cell face
    // at a time, so every intermediate cell is visited -- a line that merely
    // sampled points could skip the corner of a pillar between two samples.
    // Each visited cell must be steppable from the one before it; a corner
    // crossed exactly is resolved by stepping X then Z, which checks both
    // cells the line touches there.
    const glm::vec3 start = grid.center_of(from);
    const glm::vec3 end = grid.center_of(to);
    const float dx = end.x - start.x;
    const float dz = end.z - start.z;
    const int step_x = dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0);
    const int step_z = dz > 0.0f ? 1 : (dz < 0.0f ? -1 : 0);
    const float cell = grid.cell_size();
    // Parametric distance along the line to the next X (or Z) cell boundary,
    // and the distance between successive boundaries. Starting from a cell
    // centre, the first boundary is half a cell away.
    float t_max_x = step_x != 0 ? (0.5f * cell) / std::abs(dx) : kInfinity;
    float t_max_z = step_z != 0 ? (0.5f * cell) / std::abs(dz) : kInfinity;
    const float t_delta_x = step_x != 0 ? cell / std::abs(dx) : kInfinity;
    const float t_delta_z = step_z != 0 ? cell / std::abs(dz) : kInfinity;

    glm::ivec2 current = from;
    // Bounded by the Manhattan distance: the traversal takes exactly that
    // many face crossings to arrive.
    const int crossings = std::abs(to.x - from.x) + std::abs(to.y - from.y);
    for (int i = 0; i < crossings; ++i) {
        glm::ivec2 next = current;
        if (t_max_x < t_max_z) {
            next.x += step_x;
            t_max_x += t_delta_x;
        } else if (t_max_z < t_max_x) {
            next.y += step_z;
            t_max_z += t_delta_z;
        } else {
            // Exactly through a corner. Take the X step now and let the Z
            // step come on the next iteration, which visits both cells.
            next.x += step_x;
            t_max_x += t_delta_x;
        }
        if (!grid.can_step(current, next)) {
            return false;
        }
        current = next;
    }
    return current == to;
}

namespace {

struct OpenNode {
    float f;
    float h;
    std::size_t index;
    // The heap pops the SMALLEST f. Ties go to the smaller h (closer to the
    // goal, so the search does not wander across a plateau), then to the
    // smaller index, so two runs over the same grid pop the same order and
    // the route is a function of its inputs rather than of heap internals.
    bool operator>(const OpenNode& other) const {
        if (f != other.f) {
            return f > other.f;
        }
        if (h != other.h) {
            return h > other.h;
        }
        return index > other.index;
    }
};

// Octile distance: the exact cost of a straight-and-diagonal walk across an
// empty grid, so it never overestimates.
float heuristic(glm::ivec2 a, glm::ivec2 b, float cell) {
    const float dx = static_cast<float>(std::abs(a.x - b.x));
    const float dz = static_cast<float>(std::abs(a.y - b.y));
    return (std::max(dx, dz) + (kSqrt2 - 1.0f) * std::min(dx, dz)) * cell;
}

// The cells of the route from `start` to `goal` inclusive, or empty.
std::vector<glm::ivec2> astar(const NavGrid& grid, glm::ivec2 start, glm::ivec2 goal) {
    const std::size_t count =
        static_cast<std::size_t>(grid.width()) * static_cast<std::size_t>(grid.depth());
    constexpr std::size_t kNoParent = std::numeric_limits<std::size_t>::max();
    std::vector<float> cost(count, kInfinity);
    std::vector<std::size_t> parent(count, kNoParent);
    std::vector<std::uint8_t> closed(count, 0);
    std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<OpenNode>> open;

    const std::size_t start_index = grid.index_of(start);
    const std::size_t goal_index = grid.index_of(goal);
    const float cell = grid.cell_size();
    cost[start_index] = 0.0f;
    open.push({heuristic(start, goal, cell), heuristic(start, goal, cell), start_index});

    constexpr glm::ivec2 kNeighbours[8] = {{1, 0}, {-1, 0}, {0, 1},  {0, -1},
                                           {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    bool found = false;
    while (!open.empty()) {
        const OpenNode node = open.top();
        open.pop();
        const std::size_t index = node.index;
        if (closed[index]) {
            continue;  // a stale duplicate: this cell was reached cheaper
        }
        closed[index] = 1;
        if (node.index == goal_index) {
            found = true;
            break;
        }
        const glm::ivec2 current = grid.cell_at_index(node.index);
        for (const glm::ivec2& offset : kNeighbours) {
            const glm::ivec2 next = current + offset;
            if (!grid.can_step(current, next)) {
                continue;
            }
            const std::size_t next_index = grid.index_of(next);
            if (closed[next_index]) {
                continue;
            }
            const bool diagonal = offset.x != 0 && offset.y != 0;
            const float next_cost = cost[index] + (diagonal ? kSqrt2 : 1.0f) * cell;
            if (next_cost < cost[next_index]) {
                cost[next_index] = next_cost;
                parent[next_index] = node.index;
                const float h = heuristic(next, goal, cell);
                open.push({next_cost + h, h, next_index});
            }
        }
    }
    if (!found) {
        return {};
    }
    std::vector<glm::ivec2> cells;
    for (std::size_t at = goal_index; at != kNoParent; at = parent[at]) {
        cells.push_back(grid.cell_at_index(at));
    }
    std::reverse(cells.begin(), cells.end());
    return cells;
}

}  // namespace

std::vector<glm::vec3> find_path(const NavGrid& grid, const glm::vec3& from, const glm::vec3& to) {
    const auto start = nearest_walkable(grid, from);
    const auto goal = nearest_walkable(grid, to);
    if (!start || !goal) {
        return {};
    }
    if (*start == *goal) {
        return {grid.center_of(*goal)};
    }
    // Different patches of floor never meet, and the search would prove it
    // only by visiting every cell of the start's patch -- every half second,
    // for as long as an enemy stands on top of a crate.
    if (grid.region(*start) != grid.region(*goal)) {
        return {};
    }
    const std::vector<glm::ivec2> cells = astar(grid, *start, *goal);
    if (cells.empty()) {
        return {};
    }
    // String pulling: from each anchor, keep extending the leg while the
    // straight line to the cell after it is still walkable, then emit the
    // furthest cell reached. Greedy, so a concave bend costs one extra
    // waypoint, which is nothing against walking 0.5 m cell to cell.
    std::vector<glm::vec3> points;
    std::size_t anchor = 0;
    while (anchor + 1 < cells.size()) {
        std::size_t far = anchor + 1;
        while (far + 1 < cells.size() && straight_walk_clear(grid, cells[anchor], cells[far + 1])) {
            ++far;
        }
        points.push_back(grid.center_of(cells[far]));
        anchor = far;
    }
    return points;
}

std::optional<glm::vec3> random_walkable_point(const NavGrid& grid, std::uint32_t seed,
                                               const glm::vec3& near) {
    const auto origin = nearest_walkable(grid, near);
    if (!origin) {
        return std::nullopt;
    }
    const auto cells = grid.region_cells(grid.region(*origin));
    if (cells.empty()) {
        return std::nullopt;
    }
    const std::size_t pick = static_cast<std::size_t>(hash_u32(seed)) % cells.size();
    return grid.center_of(grid.cell_at_index(cells[pick]));
}

}  // namespace game

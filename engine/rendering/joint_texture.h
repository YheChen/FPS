#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/glm.hpp>

namespace eng {

// Joint matrices for skinning, delivered as an RGBA32F texture rather than a
// uniform array.
//
// The obvious implementation is `uniform mat4 u_joint_matrices[N]` indexed by
// the vertex's joint id. It works, and on desktop GL it is fast. In WebGL 2 it
// is a performance cliff: dynamically indexing a uniform array dropped a
// single 288-vertex character draw to ~1500 ms per frame in Chromium, while
// the identical build ran at 400 fps natively. Fetching from a texture with
// texelFetch avoids dynamic uniform indexing entirely and costs nothing.
//
// Layout: one matrix per row, four RGBA texels per row (one per column), so
// joint j column c is texel (c, j).
class JointTexture {
public:
    static std::optional<JointTexture> create(std::size_t max_joints);

    ~JointTexture();
    JointTexture(JointTexture&& other) noexcept;
    JointTexture& operator=(JointTexture&& other) noexcept;
    JointTexture(const JointTexture&) = delete;
    JointTexture& operator=(const JointTexture&) = delete;

    // Uploads `matrices` (clamped to capacity) and binds to `unit`.
    void upload_and_bind(std::span<const glm::mat4> matrices, std::uint32_t unit) const;

    std::size_t capacity() const { return capacity_; }

private:
    JointTexture() = default;

    std::uint32_t texture_ = 0;
    std::size_t capacity_ = 0;
};

}  // namespace eng

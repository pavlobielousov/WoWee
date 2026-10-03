#pragma once

// Ray helpers shared by WMORenderer's collision queries (wmo_renderer_collision.cpp) and its
// debug dump (wmo_renderer.cpp). Moved from wmo_renderer.cpp with `static` turned into `inline`
// (VITA-51); no Vulkan.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

// Ray-AABB intersection (slab method)
// Returns true if the ray intersects the axis-aligned bounding box
/// Where a ray enters and leaves a box, as distances along it. False when it
/// misses. The two are what a query has to search between: for a ray that is
/// not axis-aligned in the space the box is in, the origin's XY says nothing
/// about where the ray actually crosses the box.
inline bool rayAABBRange(const glm::vec3& origin, const glm::vec3& dir,
                         const glm::vec3& bmin, const glm::vec3& bmax,
                         float& outMin, float& outMax) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 1e-8f) {
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            const float invD = 1.0f / dir[i];
            float t0 = (bmin[i] - origin[i]) * invD;
            float t1 = (bmax[i] - origin[i]) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0.0f) return false;
    outMin = std::max(tmin, 0.0f);
    outMax = tmax;
    return true;
}

/// The triangles a ray could hit inside one group, fetched from its grid.
///
/// Between where the ray enters the group's box and where it leaves it, not
/// around the ray's origin. Those are the same place only while the ray is
/// vertical in the group's own space, and a placement with pitch makes it lean
/// - see the note in getFloorHeight. False when the ray misses the box.
///
/// Written out three times before this, once per query, which is two more
/// chances to look in the wrong place.
template <typename Group>
inline bool trianglesAlongRay(const Group& group, const glm::vec3& localOrigin,
                              const glm::vec3& localDir, std::vector<uint32_t>& out) {
    float tEnter = 0.0f, tExit = 0.0f;
    if (!rayAABBRange(localOrigin, localDir, group.boundingBoxMin,
                      group.boundingBoxMax, tEnter, tExit)) {
        return false;
    }
    const glm::vec3 enter = localOrigin + localDir * tEnter;
    const glm::vec3 exit = localOrigin + localDir * tExit;
    group.getTrianglesInRange(std::min(enter.x, exit.x) - 1.0f,
                              std::min(enter.y, exit.y) - 1.0f,
                              std::max(enter.x, exit.x) + 1.0f,
                              std::max(enter.y, exit.y) + 1.0f, out);
    return true;
}

} // namespace rendering
} // namespace wowee

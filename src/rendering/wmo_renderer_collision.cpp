// WMORenderer's collision and containment queries, moved here unchanged from wmo_renderer.cpp
// (VITA-51): floor height, wall sweeps, containment, raycasts, the floor cache, the per-group
// triangle grid and the instance spatial index. None of it touches the GPU. A file of its own so
// that a build with no Vulkan (the PS Vita, ADR-001) compiles these against its own declaration of
// WMORenderer instead of the Vulkan one.
//
// Nothing in this file may name a Vk*/Vma* type or a GPU member of GroupResources/ModelData; see
// tools/vita/collision_check.sh, which compiles it with the Vita compiler and no Vulkan headers.
#include "rendering/collision_geometry.hpp"
#include "rendering/query_timer.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/wmo_ray_helpers.hpp"
#include "rendering/wmo_renderer.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace wowee {
namespace rendering {

namespace {
/// Where a WMO surface stops being a floor and becomes a wall, as the absolute
/// z of its normal: cos 49.46 degrees.
///
/// The static pass sorts triangles into floors and walls with it, and the
/// runtime wall check skips anything at or above it because grounding handles
/// those. The two had to agree and said so only in a comment; one definition
/// is what actually holds them together. A drifted pair here means a surface
/// the player can neither walk on nor be stopped by, which reads as falling
/// through a floor or as an invisible wall.
///
/// Not the slope-slide cutoff, which is cos 50 degrees (0.6428) and lives with
/// the sliding code, and not M2's: that classifies floors at 0.35 so steep
/// stairs stay walkable while still blocking, so its two cutoffs deliberately
/// overlap where these do not.
constexpr float kWallMaxAbsNormalZ = 0.65f;
} // namespace

// Thread-local scratch buffers for collision queries (allows concurrent getFloorHeight/checkWallCollision calls)
static thread_local std::vector<size_t> tl_candidateScratch;
static thread_local std::vector<uint32_t> tl_triScratch;
static thread_local std::unordered_set<uint32_t> tl_candidateIdScratch;

void WMORenderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocus.set(worldPos, radius);
}

bool WMORenderer::saveFloorCache() const {
    if (mapName_.empty()) {
        core::Logger::getInstance().warning("Cannot save floor cache: no map name set");
        return false;
    }

    std::string filepath = "cache/wmo_floor_" + mapName_ + ".bin";

    // Create directory if needed
    std::filesystem::path path(filepath);
    std::filesystem::path absPath = std::filesystem::absolute(path);
    core::Logger::getInstance().info("Saving floor cache to: ", absPath.string());

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            core::Logger::getInstance().error("Failed to create cache directory: ", ec.message());
        }
    }

    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        core::Logger::getInstance().error("Failed to open floor cache file for writing: ", filepath);
        return false;
    }

    // Write header: magic + version + count
    const uint32_t magic = 0x574D4F46;  // "WMOF"
    const uint32_t version = 1;
    const uint64_t count = precomputedFloorGrid.size();

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Write each entry: key (uint64) + height (float)
    for (const auto& [key, height] : precomputedFloorGrid) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key));
        file.write(reinterpret_cast<const char*>(&height), sizeof(height));
    }

    core::Logger::getInstance().info("Saved WMO floor cache (", mapName_, "): ", count, " entries");
    return true;
}

bool WMORenderer::loadFloorCache() {
    if (mapName_.empty()) {
        core::Logger::getInstance().warning("Cannot load floor cache: no map name set");
        return false;
    }

    std::string filepath = "cache/wmo_floor_" + mapName_ + ".bin";

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        core::Logger::getInstance().info("No existing floor cache for map: ", mapName_);
        return false;
    }

    // Read and validate header
    uint32_t magic = 0, version = 0;
    uint64_t count = 0;

    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != 0x574D4F46 || version != 1) {
        core::Logger::getInstance().warning("Invalid floor cache file format: ", filepath);
        return false;
    }

    // Read entries
    precomputedFloorGrid.clear();
    precomputedFloorGrid.reserve(count);

    for (uint64_t i = 0; i < count; i++) {
        uint64_t key;
        float height;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        file.read(reinterpret_cast<char*>(&height), sizeof(height));
        precomputedFloorGrid[key] = height;
    }

    core::Logger::getInstance().info("Loaded WMO floor cache (", mapName_, "): ", precomputedFloorGrid.size(), " entries");
    return true;
}

void WMORenderer::precomputeFloorCache() {
    if (instances.empty()) {
        core::Logger::getInstance().info("precomputeFloorCache: no instances to precompute");
        return;
    }

    size_t startSize = precomputedFloorGrid.size();
    size_t samplesChecked = 0;

    core::Logger::getInstance().info("Pre-computing floor cache for ", instances.size(), " WMO instances...");

    for (const auto& instance : instances) {
        // Get world bounds for this instance
        const glm::vec3& boundsMin = instance.worldBoundsMin;
        const glm::vec3& boundsMax = instance.worldBoundsMax;

        // Sample reference Z is above the structure
        float refZ = boundsMax.z + 10.0f;

        // Iterate over grid points within the bounds
        float startX = std::floor(boundsMin.x / FLOOR_GRID_CELL_SIZE) * FLOOR_GRID_CELL_SIZE;
        float startY = std::floor(boundsMin.y / FLOOR_GRID_CELL_SIZE) * FLOOR_GRID_CELL_SIZE;

        int stepsX = static_cast<int>((boundsMax.x - startX) / FLOOR_GRID_CELL_SIZE) + 1;
        int stepsY = static_cast<int>((boundsMax.y - startY) / FLOOR_GRID_CELL_SIZE) + 1;
        for (int ix = 0; ix < stepsX; ++ix) {
            float x = startX + ix * FLOOR_GRID_CELL_SIZE;
            for (int iy = 0; iy < stepsY; ++iy) {
                float y = startY + iy * FLOOR_GRID_CELL_SIZE;
                // Sample at grid cell center
                float sampleX = x + FLOOR_GRID_CELL_SIZE * 0.5f;
                float sampleY = y + FLOOR_GRID_CELL_SIZE * 0.5f;

                // Check if already cached
                uint64_t key = floorGridKey(sampleX, sampleY);
                if (precomputedFloorGrid.find(key) != precomputedFloorGrid.end()) {
                    continue;  // Already computed
                }

                samplesChecked++;

                // Query floor height and store result in the precomputed grid
                auto h = getFloorHeight(sampleX, sampleY, refZ);
                if (h) {
                    precomputedFloorGrid[key] = *h;
                }
            }
        }
    }

    size_t newEntries = precomputedFloorGrid.size() - startSize;
    core::Logger::getInstance().info("Floor cache precompute complete: ", samplesChecked, " samples checked, ",
                                     newEntries, " new entries, total ", precomputedFloorGrid.size());
}

void WMORenderer::rebuildSpatialIndex() {
    spatialGrid.clear();
    instanceIndexById.clear();
    instanceIndexById.reserve(instances.size());

    for (size_t i = 0; i < instances.size(); i++) {
        const auto& inst = instances[i];
        instanceIndexById[inst.id] = i;

        insertBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, inst.id);
    }
}

void WMORenderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                   std::vector<size_t>& outIndices) const {
    outIndices.clear();

    // Hidden instances are filtered here rather than in each caller: every
    // collision, floor and containment query in this file reaches the world
    // through this one function, and a transport that is on another continent
    // must not be standable on this one.
    gatherIds(spatialGrid, queryMin, queryMax, tl_candidateIdScratch,
              [&](uint32_t id) {
                  auto idxIt = instanceIndexById.find(id);
                  if (idxIt != instanceIndexById.end() && !instances[idxIt->second].hidden) {
                      outIndices.push_back(idxIt->second);
                  }
              });

    // Safety fallback: if the grid misses due streaming/index drift, avoid
    // tunneling by scanning all instances instead of returning no candidates.
    if (outIndices.empty() && !instances.empty()) {
        outIndices.reserve(instances.size());
        for (size_t i = 0; i < instances.size(); i++) {
            if (!instances[i].hidden) outIndices.push_back(i);
        }
    }
}

int WMORenderer::findContainingGroup(const ModelData& model, const glm::vec3& localPos) const {
    // Find which group's bounding box contains the position
    // Prefer interior groups (smaller volume) when multiple match
    int bestGroup = -1;
    float bestVolume = std::numeric_limits<float>::max();

    for (size_t gi = 0; gi < model.groups.size(); gi++) {
        const auto& group = model.groups[gi];
        if (localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
            localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
            localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
            glm::vec3 size = group.boundingBoxMax - group.boundingBoxMin;
            float volume = size.x * size.y * size.z;
            if (volume < bestVolume) {
                bestVolume = volume;
                bestGroup = static_cast<int>(gi);
            }
        }
    }
    return bestGroup;
}

static bool rayIntersectsAABB(const glm::vec3& origin, const glm::vec3& dir,
                               const glm::vec3& bmin, const glm::vec3& bmax) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 1e-8f) {
            // Ray is parallel to this slab - check if origin is inside
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            float invD = 1.0f / dir[i];
            float t0 = (bmin[i] - origin[i]) * invD;
            float t1 = (bmax[i] - origin[i]) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    return tmax >= 0.0f;  // At least part of the ray is forward
}

// Closest point on triangle (from Real-Time Collision Detection).
static glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a,
                                        const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ap = p - a;
    float d1 = glm::dot(ab, ap);
    float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp);
    float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp);
    float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

void WMORenderer::GroupResources::buildCollisionGrid() {
    if (collisionVertices.empty() || collisionIndices.size() < 3) {
        gridCellsX = 0;
        gridCellsY = 0;
        return;
    }

    gridOrigin = glm::vec2(boundingBoxMin.x, boundingBoxMin.y);
    float extentX = boundingBoxMax.x - boundingBoxMin.x;
    float extentY = boundingBoxMax.y - boundingBoxMin.y;

    gridCellsX = std::max(1, static_cast<int>(std::ceil(extentX / COLLISION_CELL_SIZE)));
    gridCellsY = std::max(1, static_cast<int>(std::ceil(extentY / COLLISION_CELL_SIZE)));

    // Cap grid size to avoid excessive memory for huge groups
    if (gridCellsX > 64) gridCellsX = 64;
    if (gridCellsY > 64) gridCellsY = 64;

    size_t totalCells = static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY);
    cellTriangles.resize(totalCells);
    cellFloorTriangles.resize(totalCells);
    cellWallTriangles.resize(totalCells);

    size_t numTriangles = collisionIndices.size() / 3;
    triBounds.resize(numTriangles);
    triNormals.resize(numTriangles);
    triVisited.resize(numTriangles, 0);

    float invCellW = gridCellsX / std::max(0.01f, extentX);
    float invCellH = gridCellsY / std::max(0.01f, extentY);

    for (size_t i = 0; i + 2 < collisionIndices.size(); i += 3) {
        const glm::vec3& v0 = collisionVertices[collisionIndices[i]];
        const glm::vec3& v1 = collisionVertices[collisionIndices[i + 1]];
        const glm::vec3& v2 = collisionVertices[collisionIndices[i + 2]];

        // Triangle XY bounding box
        float triMinX = std::min({v0.x, v1.x, v2.x});
        float triMinY = std::min({v0.y, v1.y, v2.y});
        float triMaxX = std::max({v0.x, v1.x, v2.x});
        float triMaxY = std::max({v0.y, v1.y, v2.y});

        // Per-triangle Z bounds
        float triMinZ = std::min({v0.z, v1.z, v2.z});
        float triMaxZ = std::max({v0.z, v1.z, v2.z});
        triBounds[i / 3] = { .minZ = triMinZ, .maxZ = triMaxZ };

        // Precompute and store unit normal
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 normal = glm::cross(edge1, edge2);
        float normalLen = glm::length(normal);
        if (normalLen > 0.001f) {
            normal /= normalLen;
        } else {
            normal = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        triNormals[i / 3] = normal;

        // Classify floor vs wall by normal; see kWallMaxAbsNormalZ.
        float absNz = std::abs(normal.z);
        bool isFloor = (absNz >= kWallMaxAbsNormalZ);
        bool isWall = (absNz < kWallMaxAbsNormalZ);

        int cellMinX = std::max(0, static_cast<int>((triMinX - gridOrigin.x) * invCellW));
        int cellMinY = std::max(0, static_cast<int>((triMinY - gridOrigin.y) * invCellH));
        int cellMaxX = std::min(gridCellsX - 1, static_cast<int>((triMaxX - gridOrigin.x) * invCellW));
        int cellMaxY = std::min(gridCellsY - 1, static_cast<int>((triMaxY - gridOrigin.y) * invCellH));

        uint32_t triIdx = static_cast<uint32_t>(i);
        for (int cy = cellMinY; cy <= cellMaxY; ++cy) {
            for (int cx = cellMinX; cx <= cellMaxX; ++cx) {
                int cellIdx = cy * gridCellsX + cx;
                cellTriangles[cellIdx].push_back(triIdx);
                if (isFloor) cellFloorTriangles[cellIdx].push_back(triIdx);
                if (isWall) cellWallTriangles[cellIdx].push_back(triIdx);
            }
        }
    }
}

/// The triangles of one cell array that a query box reaches.
///
/// Three queries walk this grid, for any triangle, for floors and for walls,
/// and they differed in one token: which of the three per-cell arrays they
/// read. Everything else, the cell rectangle, the reserve estimate, the
/// visited-bit dedup and the single-cell shortcut that skips it, was written
/// out three times.
///
/// The dedup matters because a triangle spanning several cells is filed under
/// each of them, and a caller that tests it twice counts two hits: a raycast
/// then reports an even number of crossings where there was one surface, which
/// is how a solid wall reads as empty air.
void WMORenderer::GroupResources::gatherCellTriangles(
        const std::vector<std::vector<uint32_t>>& cells,
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0 || cells.empty()) return;

    const auto range = cellRangeCovering(
        gridCellsX, gridCellsY,
        boundingBoxMax.x - boundingBoxMin.x, boundingBoxMax.y - boundingBoxMin.y,
        glm::vec2(gridOrigin.x, gridOrigin.y), minX, minY, maxX, maxY);
    if (!range) return;

    // About eight triangles a cell, which is what the meshes here average.
    out.reserve(range->count() * 8);

    // One cell cannot hand out the same triangle twice, so the dedup and the
    // bitset clear it needs are pure cost in the commonest case.
    const bool multiCell = (range->minX != range->maxX || range->minY != range->maxY);
    if (multiCell && !triVisited.empty()) {
        for (int cy = range->minY; cy <= range->maxY; ++cy) {
            for (int cx = range->minX; cx <= range->maxX; ++cx) {
                for (uint32_t tri : cells[cy * gridCellsX + cx]) {
                    const uint32_t index = tri / 3;
                    if (!triVisited[index]) {
                        triVisited[index] = 1;
                        out.push_back(tri);
                    }
                }
            }
        }
        for (uint32_t tri : out) triVisited[tri / 3] = 0;
    } else {
        for (int cy = range->minY; cy <= range->maxY; ++cy) {
            for (int cx = range->minX; cx <= range->maxX; ++cx) {
                const auto& cell = cells[cy * gridCellsX + cx];
                out.insert(out.end(), cell.begin(), cell.end());
            }
        }
    }
}

void WMORenderer::GroupResources::getTrianglesInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellTriangles, minX, minY, maxX, maxY, out);
}

void WMORenderer::GroupResources::getFloorTrianglesInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellFloorTriangles, minX, minY, maxX, maxY, out);
}

void WMORenderer::GroupResources::getWallTrianglesInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellWallTriangles, minX, minY, maxX, maxY, out);
}

std::optional<float> WMORenderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ, float referenceZ) const {
    const bool byReference = std::isfinite(referenceZ);
    // Per-frame cache disabled: camera and player query the same (x,y) at
    // different Z within a single frame. The allowAbove filter depends on glZ,
    // so caching by (x,y) alone returns wrong floors across Z contexts.

    QueryTimer timer(&queryTimeMs, &queryCallCount);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;
    bool bestFromLowPlatform = false;
    // For the "no floor where there plainly is one" report at the end.
    bool overlappedInXY = false;
    bool rejectedByZ = false;
    float rejectedZMin = 0.0f, rejectedZMax = 0.0f;

    // A moving transport (elevator, ship hull) IS ordinary collision - you walk
    // up a docked ship's hull and you stand on a lift with no attachment at all
    // (an elevator is neither an M2 nor a client-animated ship, so client-side
    // boarding never fires for it - see GameHandler::updateM2TransportBoarding).
    // Excluding transports outright therefore deletes the only floor those cases
    // have. But an unrestricted transport hit is the Undercity elevator yo-yo: as
    // the lift cycles through a bystander's (x,y) its deck enters the candidate
    // set metres above or below them and the pick flips to it and back.
    // So a transport deck counts only when it is genuinely underfoot, not
    // anywhere along the shaft. referenceZ is the true feet when the caller
    // supplies one (the player's physics query does); otherwise glZ stands in,
    // and that is the probe rather than the feet - the caller raised it by its
    // step-up budget - so back that lift out first.
    constexpr float kTransportProbeLift = 2.0f;
    constexpr float kTransportFloorReach = 3.0f;
    const float transportFloorMinZ =
        (byReference ? referenceZ : glZ - kTransportProbeLift) - kTransportFloorReach;

    // World-space ray: from high above, pointing straight down
    glm::vec3 worldOrigin(glX, glY, glZ + 500.0f);
    glm::vec3 worldDir(0.0f, 0.0f, -1.0f);

    // Lambda to test a single group for floor hits
    auto testGroupFloor = [&](const WMOInstance& instance, const ModelData& model,
                              const GroupResources& group,
                              const glm::vec3& localOrigin, const glm::vec3& localDir) {
        const auto& verts = group.collisionVertices;
        const auto& indices = group.collisionIndices;

        // Use unfiltered triangle list: a vertical ray naturally misses vertical
        // geometry via ray-triangle intersection, so pre-filtering by normal is
        // unnecessary and risks excluding legitimate floor geometry (steep ramps,
        // stair treads with non-trivial normals).
        // Search where the ray actually crosses this group, not where it
        // starts.
        //
        // The ray is straight down in world space and begins five hundred units
        // above the query. Transformed into the model's own space it stays
        // straight only while the model has no pitch and no roll - a yaw keeps
        // a vertical ray vertical, which is why every building placed flat has
        // always worked. Give it pitch and the local ray leans, and five
        // hundred units of lean puts the origin's XY nowhere near the deck it
        // passes through. Searching a metre either side of that origin found
        // nothing, every time, and the log said so: considered, and no triangle
        // hit.
        //
        // Darkshore's bridges are the placements with pitch. They are also the
        // ones you fall through.
        if (!trianglesAlongRay(group, localOrigin, localDir, tl_triScratch)) return;

        for (uint32_t triStart : tl_triScratch) {
            const glm::vec3& v0 = verts[indices[triStart]];
            const glm::vec3& v1 = verts[indices[triStart + 1]];
            const glm::vec3& v2 = verts[indices[triStart + 2]];

            // One test, not two: the intersection is two-sided, so the
            // reversed winding this used to retry gives the same answer.
            const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);

            if (t > 0.0f) {
                glm::vec3 hitLocal = localOrigin + localDir * t;
                glm::vec3 hitWorld = glm::vec3(instance.modelMatrix * glm::vec4(hitLocal, 1.0f));

                // Accept floors at or below glZ (the caller already elevates
                // glZ by stepUpBudget to handle step-up range).  Among those,
                // pick the highest (closest to feet).
                if (hitWorld.z <= glZ &&
                    (!instance.isTransport || hitWorld.z >= transportFloorMinZ)) {
                    const bool better = !bestFloor
                        ? true
                        : (byReference
                               ? std::abs(hitWorld.z - referenceZ) <
                                     std::abs(*bestFloor - referenceZ)
                               : hitWorld.z > *bestFloor);
                    if (better) {
                        bestFloor = hitWorld.z;
                        bestFromLowPlatform = model.isLowPlatform;

                        // Use precomputed normal, ensure upward, transform to world
                        glm::vec3 localNormal = group.triNormals[triStart / 3];
                        if (localNormal.z < 0.0f) localNormal = -localNormal;
                        glm::vec3 worldNormal = glm::normalize(
                            glm::vec3(instance.modelMatrix * glm::vec4(localNormal, 0.0f)));
                        bestNormalZ = std::abs(worldNormal.z);
                    }
                }
            }
        }
    };

    // Fast path: current active interior group and its neighbors are usually
    // the right answer for player-floor queries while moving in cities/buildings.
    if (activeGroup_.isValid() && activeGroup_.instanceIdx < instances.size()) {
        const auto& instance = instances[activeGroup_.instanceIdx];
        auto it = loadedModels.find(instance.modelId);
        if (it != loadedModels.end() && instance.modelId == activeGroup_.modelId) {
            const ModelData& model = it->second;
            glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(worldOrigin, 1.0f));
            glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

            auto testGroupIdx = [&](uint32_t gi) {
                if (gi >= model.groups.size()) return;
                if (gi < instance.worldGroupBounds.size()) {
                    const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                    if (glX < gMin.x || glX > gMax.x ||
                        glY < gMin.y || glY > gMax.y ||
                        glZ - 4.0f > gMax.z) {
                        return;
                    }
                }
                const auto& group = model.groups[gi];
                if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                    return;
                }
                testGroupFloor(instance, model, group, localOrigin, localDir);
            };

            if (activeGroup_.groupIdx >= 0) {
                testGroupIdx(static_cast<uint32_t>(activeGroup_.groupIdx));
            }
            for (uint32_t ngi : activeGroup_.neighborGroups) {
                testGroupIdx(ngi);
            }
        }
    }

    // Full scan: test all instances (active group result above is not
    // early-returned because overlapping WMO instances need full coverage).
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 8.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 10.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;
        float zMarginDown = model.isLowPlatform ? 20.0f : 2.0f;
        float zMarginUp = model.isLowPlatform ? 20.0f : 4.0f;

        // Broad-phase reject in world space to avoid expensive matrix transforms.
        if (bestFloor && instance.worldBoundsMax.z <= (*bestFloor + 0.05f)) {
            continue;
        }
        const bool insideXY = glX >= instance.worldBoundsMin.x && glX <= instance.worldBoundsMax.x &&
                              glY >= instance.worldBoundsMin.y && glY <= instance.worldBoundsMax.y;
        if (!withinWorldBounds(instance, glX, glY, glZ, zMarginDown, zMarginUp)) {
            // Over a building and rejected anyway: the only thing that can do
            // that here is the Z window, and it is worth naming when the answer
            // comes back "no floor" - falling through something you are
            // standing on looks the same whether it was never considered or
            // considered and missed.
            if (insideXY) {
                overlappedInXY = true;
                rejectedByZ = true;
                rejectedZMin = instance.worldBoundsMin.z;
                rejectedZMax = instance.worldBoundsMax.z;
            }
            continue;
        }
        if (insideXY) overlappedInXY = true;

        // World-space pre-pass: check which groups' world XY bounds contain
        // the query point. For a vertical ray this eliminates most groups
        // before any local-space math.
        bool anyGroupOverlaps = false;
        for (size_t gi = 0; gi < model.groups.size() && gi < instance.worldGroupBounds.size(); ++gi) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (glX >= gMin.x && glX <= gMax.x &&
                glY >= gMin.y && glY <= gMax.y &&
                glZ - 4.0f <= gMax.z) {
                anyGroupOverlaps = true;
                break;
            }
        }
        if (!anyGroupOverlaps) continue;

        // Use cached inverse matrix
        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(worldOrigin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            // World-space group cull - vertical ray at (glX, glY)
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (glX < gMin.x || glX > gMax.x ||
                    glY < gMin.y || glY > gMax.y ||
                    glZ - 4.0f > gMax.z) {
                    continue;
                }
            }

            const auto& group = model.groups[gi];
            if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                continue;
            }

            testGroupFloor(instance, model, group, localOrigin, localDir);
        }
    }

    // Persistent grid cache disabled (see above comment about stairs fall-through)

    // Standing over a building and told there is no floor.
    //
    // Falling through something looks the same whether the building was never
    // considered, considered and rejected by the height window, or considered
    // and simply missed by the ray - and those need different fixes.
    //
    // Once per place, not once a second. The floor is queried every frame, so
    // a player standing on a spot with no floor under it wrote the same line
    // with the same three coordinates for as long as they stood there - a
    // second apart, forever - and the second line said nothing the first did
    // not. A place is a couple of units across, and after enough of them the
    // report says so and stops: a run that is finding hundreds has one
    // problem, not hundreds.
    if (!bestFloor && overlappedInXY) {
        struct ReportedPlace { float x, y, z; };
        static std::vector<ReportedPlace> reported;
        static bool reportedCap = false;
        constexpr float kSamePlace = 2.0f;
        constexpr size_t kMaxPlaces = 32;
        const bool seenHere = std::any_of(
            reported.begin(), reported.end(), [&](const ReportedPlace& p) {
                const float dx = p.x - glX, dy = p.y - glY, dz = p.z - glZ;
                return dx * dx + dy * dy + dz * dz <= kSamePlace * kSamePlace;
            });
        if (!seenHere && reported.size() >= kMaxPlaces) {
            if (!reportedCap) {
                reportedCap = true;
                LOG_WARNING("No WMO floor: ", kMaxPlaces, " places reported and no more "
                            "will be - this is one problem, not ", kMaxPlaces, " of them");
            }
        } else if (!seenHere) {
            reported.push_back({glX, glY, glZ});
            if (rejectedByZ) {
                LOG_WARNING("No WMO floor at (", glX, ",", glY, ",", glZ,
                            ") - a building covers that spot but its height window "
                            "rejected the query: bounds z=[", rejectedZMin, "..",
                            rejectedZMax, "]");
            } else {
                LOG_WARNING("No WMO floor at (", glX, ",", glY, ",", glZ,
                            ") - a building covers that spot, was considered, and "
                            "no triangle was hit");
            }
        }
    }

    if (bestFloor && outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

std::optional<float> WMORenderer::getInstanceFloorHeight(uint32_t instanceId,
                                                         float glX, float glY, float glZ,
                                                         float* outNormalZ) const {
    const auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) {
        return std::nullopt;
    }

    const auto& instance = instances[idxIt->second];
    const auto modelIt = loadedModels.find(instance.modelId);
    if (modelIt == loadedModels.end()) return std::nullopt;
    const auto& model = modelIt->second;

    if (!withinWorldBounds(instance, glX, glY, glZ, 2.0f, 4.0f)) {
        return std::nullopt;
    }

    const glm::vec3 worldOrigin(glX, glY, glZ + 500.0f);
    const glm::vec3 worldDir(0.0f, 0.0f, -1.0f);
    const glm::vec3 localOrigin(instance.invModelMatrix * glm::vec4(worldOrigin, 1.0f));
    const glm::vec3 localDir = glm::normalize(
        glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        if (gi < instance.worldGroupBounds.size()) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (glX < gMin.x || glX > gMax.x ||
                glY < gMin.y || glY > gMax.y || glZ - 4.0f > gMax.z) {
                continue;
            }
        }

        const auto& group = model.groups[gi];
        if (!rayIntersectsAABB(localOrigin, localDir,
                               group.boundingBoxMin, group.boundingBoxMax)) {
            continue;
        }

        // Between where the ray enters this group and where it leaves, for the
        // reason getFloorHeight gives: a vertical world ray is not vertical in
        // the local space of anything with pitch, and its origin is five
        // hundred units away from what it crosses.
        if (!trianglesAlongRay(group, localOrigin, localDir, tl_triScratch)) continue;
        for (uint32_t triStart : tl_triScratch) {
            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            const glm::vec3& v0 = verts[indices[triStart]];
            const glm::vec3& v1 = verts[indices[triStart + 1]];
            const glm::vec3& v2 = verts[indices[triStart + 2]];

            const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
            if (t <= 0.0f) continue;

            const glm::vec3 hitLocal = localOrigin + localDir * t;
            const glm::vec3 hitWorld(instance.modelMatrix * glm::vec4(hitLocal, 1.0f));
            if (hitWorld.z > glZ || (bestFloor && hitWorld.z <= *bestFloor)) continue;

            bestFloor = hitWorld.z;
            glm::vec3 localNormal = group.triNormals[triStart / 3];
            if (localNormal.z < 0.0f) localNormal = -localNormal;
            const glm::vec3 worldNormal = glm::normalize(
                glm::vec3(instance.modelMatrix * glm::vec4(localNormal, 0.0f)));
            bestNormalZ = std::abs(worldNormal.z);
        }
    }

    if (bestFloor && outNormalZ) *outNormalZ = bestNormalZ;
    return bestFloor;
}

/// Whether solid WMO geometry stands between two points.
///
/// A line of sight test, and deliberately not checkWallCollision: that one
/// models a player cylinder stepping a fraction of a metre, and its group
/// pre-pass only keeps groups near `to` - handed a forty-unit ray from the
/// camera it would never look at the wall in the middle. This walks the whole
/// segment instead, and asks nothing about radius or step height. A sight line
/// is a line.
///
/// Both ends are excluded by a small margin. The far end because a player
/// standing against a wall is in front of it, not behind it, and the near end
/// because a camera clipped into geometry would otherwise call everything
/// blocked.
bool WMORenderer::segmentBlocked(const glm::vec3& from, const glm::vec3& to) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);

    const glm::vec3 delta = to - from;
    const float dist = glm::length(delta);
    if (dist < 1e-4f) return false;
    const glm::vec3 dir = delta / dist;

    // A metre of slack around the segment: gatherCandidates works in world
    // bounds and a wall's instance box can begin just outside them.
    const glm::vec3 queryMin = glm::min(from, to) - glm::vec3(1.0f);
    const glm::vec3 queryMax = glm::max(from, to) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;
        if (!rayIntersectsAABB(from, dir, instance.worldBoundsMin, instance.worldBoundsMax)) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;

        // Into the instance's own space, where its collision mesh lives. The
        // distance is measured there too: a scaled placement makes local units
        // and world units different lengths, and t comes back in local ones.
        const glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
        const glm::vec3 localTo   = glm::vec3(instance.invModelMatrix * glm::vec4(to, 1.0f));
        const glm::vec3 localDelta = localTo - localFrom;
        const float localDist = glm::length(localDelta);
        if (localDist < 1e-4f) continue;
        const glm::vec3 localDir = localDelta / localDist;
        const float nearMargin = 0.15f * (localDist / dist);
        const float farMargin  = 0.35f * (localDist / dist);

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (!rayIntersectsAABB(from, dir, gMin, gMax)) continue;
            }
            const auto& group = model.groups[gi];
            if (!trianglesAlongRay(group, localFrom, localDir, tl_triScratch)) continue;

            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            for (uint32_t triStart : tl_triScratch) {
                if (triStart + 2 >= indices.size()) continue;
                const glm::vec3& v0 = verts[indices[triStart]];
                const glm::vec3& v1 = verts[indices[triStart + 1]];
                const glm::vec3& v2 = verts[indices[triStart + 2]];
                const float t = rayTriangleIntersect(localFrom, localDir, v0, v1, v2);
                if (t > nearMargin && t < localDist - farMargin) return true;
            }
        }
    }
    return false;
}

bool WMORenderer::checkWallCollision(const glm::vec3& from, const glm::vec3& to, glm::vec3& adjustedPos, bool insideWMO) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    adjustedPos = to;
    bool blocked = false;

    glm::vec3 moveDir = to - from;
    float moveDistSq = glm::dot(moveDir, moveDir);
    if (moveDistSq < 1e-6f) return false;

    // Player collision parameters - WoW-style horizontal cylinder
    // Tighter radius when inside for more responsive indoor collision
    const float PLAYER_RADIUS = insideWMO ? 0.45f : 0.50f;
    const float PLAYER_HEIGHT = 2.0f;       // Cylinder height for Z bounds
    const float MAX_STEP_HEIGHT = 1.0f;     // Step-up threshold

    glm::vec3 queryMin = glm::min(from, to) - glm::vec3(8.0f, 8.0f, 5.0f);
    glm::vec3 queryMax = glm::max(from, to) + glm::vec3(8.0f, 8.0f, 5.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;

        const float broadMargin = PLAYER_RADIUS + 1.5f;
        if (from.x < instance.worldBoundsMin.x - broadMargin && to.x < instance.worldBoundsMin.x - broadMargin) continue;
        if (from.x > instance.worldBoundsMax.x + broadMargin && to.x > instance.worldBoundsMax.x + broadMargin) continue;
        if (from.y < instance.worldBoundsMin.y - broadMargin && to.y < instance.worldBoundsMin.y - broadMargin) continue;
        if (from.y > instance.worldBoundsMax.y + broadMargin && to.y > instance.worldBoundsMax.y + broadMargin) continue;
        if (from.z > instance.worldBoundsMax.z + PLAYER_HEIGHT && to.z > instance.worldBoundsMax.z + PLAYER_HEIGHT) continue;
        if (from.z + PLAYER_HEIGHT < instance.worldBoundsMin.z && to.z + PLAYER_HEIGHT < instance.worldBoundsMin.z) continue;

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;

        // World-space pre-pass: skip instances where no groups are near the movement
        const float wallMargin = PLAYER_RADIUS + 2.0f;
        bool anyGroupNear = false;
        for (size_t gi = 0; gi < model.groups.size() && gi < instance.worldGroupBounds.size(); ++gi) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (to.x >= gMin.x - wallMargin && to.x <= gMax.x + wallMargin &&
                to.y >= gMin.y - wallMargin && to.y <= gMax.y + wallMargin &&
                to.z + PLAYER_HEIGHT >= gMin.z && to.z <= gMax.z + wallMargin) {
                anyGroupNear = true;
                break;
            }
        }
        if (!anyGroupNear) continue;

        // Transform positions into local space using cached inverse
        glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
        glm::vec3 localTo = glm::vec3(instance.invModelMatrix * glm::vec4(to, 1.0f));
        float localFeetZ = localTo.z;
        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            // World-space group cull
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (to.x < gMin.x - wallMargin || to.x > gMax.x + wallMargin ||
                    to.y < gMin.y - wallMargin || to.y > gMax.y + wallMargin ||
                    to.z > gMax.z + PLAYER_HEIGHT || to.z + PLAYER_HEIGHT < gMin.z) {
                    continue;
                }
            }

            const auto& group = model.groups[gi];
            // Local-space AABB check
            float margin = PLAYER_RADIUS + 2.0f;
            if (localTo.x < group.boundingBoxMin.x - margin || localTo.x > group.boundingBoxMax.x + margin ||
                localTo.y < group.boundingBoxMin.y - margin || localTo.y > group.boundingBoxMax.y + margin ||
                localTo.z < group.boundingBoxMin.z - margin || localTo.z > group.boundingBoxMax.z + margin) {
                continue;
            }

            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;

            // Use spatial grid: query range covering the movement segment + player radius
            float rangeMinX = std::min(localFrom.x, localTo.x) - PLAYER_RADIUS - 1.5f;
            float rangeMinY = std::min(localFrom.y, localTo.y) - PLAYER_RADIUS - 1.5f;
            float rangeMaxX = std::max(localFrom.x, localTo.x) + PLAYER_RADIUS + 1.5f;
            float rangeMaxY = std::max(localFrom.y, localTo.y) + PLAYER_RADIUS + 1.5f;
            group.getTrianglesInRange(rangeMinX, rangeMinY, rangeMaxX, rangeMaxY, tl_triScratch);

            for (uint32_t triStart : tl_triScratch) {
                // Use pre-computed Z bounds for fast vertical reject
                const auto& tb = group.triBounds[triStart / 3];

                // Only collide with walls in player's vertical range
                if (tb.maxZ < localFeetZ + 0.3f) continue;
                if (tb.minZ > localFeetZ + PLAYER_HEIGHT) continue;

                // Skip low geometry that can be stepped over
                if (tb.maxZ <= localFeetZ + MAX_STEP_HEIGHT) continue;

                // Skip very short vertical surfaces (stair risers)
                float triHeight = tb.maxZ - tb.minZ;
                if (triHeight < 1.0f && tb.maxZ <= localFeetZ + 1.2f) continue;

                // Use MOPY flags to filter wall collision. Blocking set is the
                // union of both flag conventions seen in the assets:
                //  - explicit collision hulls (0x08), rendered or not - tunnel
                //    walls rely on invisible hulls;
                //  - rendered geometry (0x20) that is not detail (0x04) - the
                //    Deeprun Tram gates carry render flags without 0x08 and
                //    were walk-through when only 0x08 blocked.
                // Detail/decorative (0x04: gears, railings, webs) never blocks.
                uint32_t triIdx = triStart / 3;
                // Detail never blocks - unless it is all the group has.
                //
                // A group whose every triangle is detail has nothing left to
                // stand on or walk into once detail is excluded, and a group
                // that offers no collision at all is not what the flag means:
                // 0x04 marks the gears and railings *among* solid geometry, so
                // that they do not block. Darkshore's bridges are 428 triangles
                // and every one of them is detail, which is why they are walked
                // through.
                if (!group.noBlockingTriangles &&
                    !group.triMopyFlags.empty() && triIdx < group.triMopyFlags.size()) {
                    uint8_t mopy = group.triMopyFlags[triIdx];
                    if (mopy != 0) {
                        const bool collisionHull = (mopy & 0x08) != 0;
                        const bool renderedSolid = (mopy & 0x20) != 0 && !(mopy & 0x04);
                        if (!collisionHull && !renderedSolid) continue;
                    }
                }

                const glm::vec3& v0 = verts[indices[triStart]];
                const glm::vec3& v1 = verts[indices[triStart + 1]];
                const glm::vec3& v2 = verts[indices[triStart + 2]];

                // Use precomputed normal for swept test and push fallback
                glm::vec3 normal = group.triNormals[triStart / 3];
                if (glm::dot(normal, normal) < 0.5f) continue;  // degenerate

                // Recompute plane distances with current (possibly pushed) localTo
                float fromDist = glm::dot(localFrom - v0, normal);
                float toDist = glm::dot(localTo - v0, normal);

                // Swept test: prevent tunneling when crossing a wall between frames
                if ((fromDist > PLAYER_RADIUS && toDist < -PLAYER_RADIUS) ||
                    (fromDist < -PLAYER_RADIUS && toDist > PLAYER_RADIUS)) {
                    float denom = (fromDist - toDist);
                    if (std::abs(denom) > 1e-6f) {
                        float tHit = fromDist / denom;
                        if (tHit >= 0.0f && tHit <= 1.0f) {
                            glm::vec3 hitPoint = localFrom + (localTo - localFrom) * tHit;
                            glm::vec3 hitClosest = closestPointOnTriangle(hitPoint, v0, v1, v2);
                            float hitErrSq = glm::dot(hitClosest - hitPoint, hitClosest - hitPoint);
                            if (hitErrSq <= 0.25f * 0.25f) {
                                float side = fromDist > 0.0f ? 1.0f : -1.0f;
                                glm::vec3 safeLocal = hitPoint + normal * side * (PLAYER_RADIUS + 0.05f);
                                glm::vec3 pushLocal(safeLocal.x - localTo.x, safeLocal.y - localTo.y, 0.0f);
                                // Cap swept pushback so walls don't shove the player violently
                                float pushLenSq = pushLocal.x * pushLocal.x + pushLocal.y * pushLocal.y;
                                const float MAX_SWEPT_PUSH = insideWMO ? 0.45f : 0.25f;
                                if (pushLenSq > MAX_SWEPT_PUSH * MAX_SWEPT_PUSH) {
                                    float scale = MAX_SWEPT_PUSH * glm::inversesqrt(pushLenSq);
                                    pushLocal.x *= scale;
                                    pushLocal.y *= scale;
                                }
                                localTo.x += pushLocal.x;
                                localTo.y += pushLocal.y;
                                glm::vec3 pushWorld = glm::vec3(instance.modelMatrix * glm::vec4(pushLocal, 0.0f));
                                adjustedPos.x += pushWorld.x;
                                adjustedPos.y += pushWorld.y;
                                blocked = true;
                                continue;
                            }
                        }
                    }
                }

                // Horizontal cylinder collision: closest point + horizontal distance
                glm::vec3 closest = closestPointOnTriangle(localTo, v0, v1, v2);
                glm::vec3 delta = localTo - closest;
                float horizDistSq = delta.x * delta.x + delta.y * delta.y;

                if (horizDistSq <= PLAYER_RADIUS * PLAYER_RADIUS) {
                    // Skip floor-like surfaces - grounding handles them, not wall
                    // collision. The same cutoff the static pass sorted by.
                    float absNz = std::abs(normal.z);
                    if (absNz >= kWallMaxAbsNormalZ) continue;

                    const float SKIN = 0.005f;        // small separation so we don't re-collide immediately
                    // Push must cover full penetration to prevent gradual clip-through
                    const float MAX_PUSH = PLAYER_RADIUS;
                    float horizDist = std::sqrt(horizDistSq);
                    float penetration = (PLAYER_RADIUS - horizDist);
                    float pushDist = glm::clamp(penetration + SKIN, 0.0f, MAX_PUSH);
                    glm::vec2 pushDir2;
                    if (horizDistSq > 1e-8f) {
                        pushDir2 = glm::vec2(delta.x, delta.y) * (1.0f / horizDist);
                    } else {
                        glm::vec2 n2(normal.x, normal.y);
                        float n2LenSq = glm::dot(n2, n2);
                        if (n2LenSq < 1e-8f) continue;
                        pushDir2 = n2 * glm::inversesqrt(n2LenSq);
                    }
                    glm::vec3 pushLocal(pushDir2.x * pushDist, pushDir2.y * pushDist, 0.0f);

                    localTo.x += pushLocal.x;
                    localTo.y += pushLocal.y;
                    glm::vec3 pushWorld = glm::vec3(instance.modelMatrix * glm::vec4(pushLocal, 0.0f));
                    adjustedPos.x += pushWorld.x;
                    adjustedPos.y += pushWorld.y;
                    blocked = true;
                }
            }
        }
    }

    return blocked;
}

void WMORenderer::updateActiveGroup(float glX, float glY, float glZ) {
    // If active group is still valid, check if player is still inside it
    if (activeGroup_.isValid() && activeGroup_.instanceIdx < instances.size()) {
        const auto& instance = instances[activeGroup_.instanceIdx];
        if (instance.modelId == activeGroup_.modelId) {
            auto it = loadedModels.find(instance.modelId);
            if (it != loadedModels.end()) {
                const ModelData& model = it->second;
                glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

                // Still inside active group?
                if (activeGroup_.groupIdx >= 0 && static_cast<size_t>(activeGroup_.groupIdx) < model.groups.size()) {
                    const auto& group = model.groups[activeGroup_.groupIdx];
                    if (localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
                        localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
                        localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
                        return;  // Still in same group
                    }
                }

                // Check portal-neighbor groups
                for (uint32_t ngi : activeGroup_.neighborGroups) {
                    if (ngi < model.groups.size()) {
                        const auto& group = model.groups[ngi];
                        if (localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
                            localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
                            localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
                            // Moved to a neighbor group - update
                            activeGroup_.groupIdx = static_cast<int32_t>(ngi);
                            // Rebuild neighbors for new group
                            activeGroup_.neighborGroups.clear();
                            if (ngi < model.groupPortalRefs.size()) {
                                auto [portalStart, portalCount] = model.groupPortalRefs[ngi];
                                for (uint16_t pi = 0; pi < portalCount; pi++) {
                                    uint16_t refIdx = portalStart + pi;
                                    if (refIdx < model.portalRefs.size()) {
                                        uint32_t tgt = model.portalRefs[refIdx].groupIndex;
                                        if (tgt < model.groups.size()) {
                                            activeGroup_.neighborGroups.push_back(tgt);
                                        }
                                    }
                                }
                            }
                            return;
                        }
                    }
                }
            }
        }
    }

    // Full scan: find which instance/group contains the player
    activeGroup_.invalidate();

    glm::vec3 queryMin(glX - 0.5f, glY - 0.5f, glZ - 0.5f);
    glm::vec3 queryMax(glX + 0.5f, glY + 0.5f, glZ + 0.5f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (!withinWorldBounds(instance, glX, glY, glZ)) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;
        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

        int gi = findContainingGroup(model, localPos);
        if (gi >= 0) {
            activeGroup_.instanceIdx = static_cast<uint32_t>(idx);
            activeGroup_.modelId = instance.modelId;
            activeGroup_.groupIdx = gi;

            // Build neighbor list from portal refs
            activeGroup_.neighborGroups.clear();
            uint32_t groupIdx = static_cast<uint32_t>(gi);
            if (groupIdx < model.groupPortalRefs.size()) {
                auto [portalStart, portalCount] = model.groupPortalRefs[groupIdx];
                for (uint16_t pi = 0; pi < portalCount; pi++) {
                    uint16_t refIdx = portalStart + pi;
                    if (refIdx < model.portalRefs.size()) {
                        uint32_t tgt = model.portalRefs[refIdx].groupIndex;
                        if (tgt < model.groups.size()) {
                            activeGroup_.neighborGroups.push_back(tgt);
                        }
                    }
                }
            }
            return;
        }
    }
}

/// Whether a collision focus is set and this instance falls outside it.
///
/// Five queries ask this before they look at an instance at all, and each had
/// its own copy. The focus is what keeps a raycast from walking every building
/// in the zone when the caller only cares about what is near the player.
/// MOGP flag 0x2000: the group is indoors. It is what separates a
/// building's inside from the porch and the roof, which are groups of the
/// same model, and so what decides whether the sky is still drawn.
constexpr uint32_t kWMOGroupIndoor = 0x2000;

bool WMORenderer::outsideCollisionFocus(const WMOInstance& instance) const {
    return collisionFocus.excludes(instance.worldBoundsMin,
                                   instance.worldBoundsMax);
}

/// Whether a point is inside an instance's world bounds, with the Z window
/// widened by the margins the caller asks for.
///
/// The X and Y test is the same everywhere; the Z margins are not, and that is
/// why they are arguments rather than a constant. Three different pairs are in
/// use: none for the containment queries, the model-dependent pair the floor
/// query computes, and a fixed 2 down by 4 up. Two call sites spell out
/// numbers that match the two branches of the model-dependent rule, which may
/// mean they were copied from it before it learned about low platforms.
bool WMORenderer::withinWorldBounds(const WMOInstance& instance,
                                    float glX, float glY, float glZ,
                                    float zMarginDown, float zMarginUp) const {
    return glX >= instance.worldBoundsMin.x && glX <= instance.worldBoundsMax.x &&
           glY >= instance.worldBoundsMin.y && glY <= instance.worldBoundsMax.y &&
           glZ >= instance.worldBoundsMin.z - zMarginDown &&
           glZ <= instance.worldBoundsMax.z + zMarginUp;
}

/// Whether a point is inside any of a WMO's groups.
///
/// Asked two ways: of every group, which is what tells the client it is under
/// a roof at all, and of interior groups only, which is what decides whether
/// the sky and the outdoor lighting are drawn. The two differed by one line
/// and each had its own copy of the walk: the candidate gather, the focus
/// filter, the world-bounds reject, the per-group world-bounds pre-check and
/// the transform into model space.
///
/// The pre-check is not redundant with the transform below it. Inverting the
/// model matrix and multiplying is the expensive part, and a building whose
/// own bounds contain the point usually has no *group* that does.
bool WMORenderer::isInsideWMOGroups(float glX, float glY, float glZ,
                                    bool interiorOnly, uint32_t* outModelId) const {
    const glm::vec3 queryMin(glX - 0.5f, glY - 0.5f, glZ - 0.5f);
    const glm::vec3 queryMax(glX + 0.5f, glY + 0.5f, glZ + 0.5f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;
        if (!withinWorldBounds(instance, glX, glY, glZ)) continue;

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;

        bool anyGroupContains = false;
        for (size_t gi = 0; gi < model.groups.size() && gi < instance.worldGroupBounds.size(); ++gi) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (glX >= gMin.x && glX <= gMax.x &&
                glY >= gMin.y && glY <= gMax.y &&
                glZ >= gMin.z && glZ <= gMax.z) {
                anyGroupContains = true;
                break;
            }
        }
        if (!anyGroupContains) continue;

        const glm::vec3 localPos =
            glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));
        for (const auto& group : model.groups) {
            if (interiorOnly && !(group.groupFlags & kWMOGroupIndoor)) continue;
            if (localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
                localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
                localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
                if (outModelId) *outModelId = instance.modelId;
                return true;
            }
        }
    }
    return false;
}

bool WMORenderer::isInsideWMO(float glX, float glY, float glZ, uint32_t* outModelId) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    return isInsideWMOGroups(glX, glY, glZ, /*interiorOnly=*/false, outModelId);
}

bool WMORenderer::isInsideInteriorWMO(float glX, float glY, float glZ) const {
    return isInsideWMOGroups(glX, glY, glZ, /*interiorOnly=*/true, nullptr);
}

float WMORenderer::raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;
    // The camera is a solid sphere and every solid surface stops it: floors and
    // ceilings as much as walls. This used to consider the wall list alone,
    // which is pre-filtered to |normal.z| <= 0.65, and then narrowed that to
    // 0.20 again on the way past - so a storey's floor, a vaulted ceiling and
    // anything more than a few degrees off vertical simply was not there, and
    // pitching the camera down through the floor of an upper room or up through
    // a roof met nothing at all.
    //
    // Nothing replaces the normal test. The floor underfoot is only crossed by
    // this ray when the camera is actually being pushed down through it, which
    // is the case where it has to stop.
    //
    // Ignore whatever the pivot is standing in, though: the ray starts inside
    // the player, so a surface within a hand's width would otherwise jam the
    // camera at the minimum.
    constexpr float MIN_HIT_DISTANCE = 0.20f;
    // Only surfaces the ray is very nearly parallel to are dropped. This was
    // 0.25, which is a 75-degree cone: a wall approached at a shallow angle -
    // exactly when the camera is sliding along it and about to pass through -
    // failed the test and was not collided with at all.
    constexpr float MIN_SURFACE_ALIGNMENT = 0.05f;

    glm::vec3 rayEnd = origin + direction * maxDistance;
    glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;

        glm::vec3 center = (instance.worldBoundsMin + instance.worldBoundsMax) * 0.5f;
        glm::vec3 halfExtent = instance.worldBoundsMax - center;
        float radiusSq = glm::dot(halfExtent, halfExtent);
        glm::vec3 toCenter = center - origin;
        float distSq = glm::dot(toCenter, toCenter);
        float maxR = maxDistance + std::sqrt(radiusSq) + 1.0f;
        if (distSq > maxR * maxR) {
            continue;
        }

        glm::vec3 worldMin = instance.worldBoundsMin - glm::vec3(0.5f);
        glm::vec3 worldMax = instance.worldBoundsMax + glm::vec3(0.5f);
        if (!rayIntersectsAABB(origin, direction, worldMin, worldMax)) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;

        // Use cached inverse matrix
        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f)));

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            // World-space group cull - skip groups whose world AABB doesn't intersect the ray
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (!rayIntersectsAABB(origin, direction, gMin, gMax)) {
                    continue;
                }
            }

            const auto& group = model.groups[gi];
            // Local-space AABB cull
            if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                continue;
            }

            // Narrow-phase: triangle raycast using spatial grid (wall-only).
            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;

            // Compute local-space ray endpoint and query grid for XY range
            glm::vec3 localEnd = localOrigin + localDir * (closestHit / glm::length(
                glm::vec3(instance.modelMatrix * glm::vec4(localDir, 0.0f))));
            float rMinX = std::min(localOrigin.x, localEnd.x) - 1.0f;
            float rMinY = std::min(localOrigin.y, localEnd.y) - 1.0f;
            float rMaxX = std::max(localOrigin.x, localEnd.x) + 1.0f;
            float rMaxY = std::max(localOrigin.y, localEnd.y) + 1.0f;
            group.getTrianglesInRange(rMinX, rMinY, rMaxX, rMaxY, tl_triScratch);

            for (uint32_t triStart : tl_triScratch) {
                const glm::vec3& v0 = verts[indices[triStart]];
                const glm::vec3& v1 = verts[indices[triStart + 1]];
                const glm::vec3& v2 = verts[indices[triStart + 2]];
                glm::vec3 triNormal = group.triNormals[triStart / 3];
                if (glm::dot(triNormal, triNormal) < 0.5f) continue;  // degenerate
                // Ignore near-grazing intersections that tend to come from ramps/arches
                // and cause camera pull-in even when no meaningful wall is behind the player.
                if (std::abs(glm::dot(triNormal, localDir)) < MIN_SURFACE_ALIGNMENT) {
                    continue;
                }

                float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t <= 0.0f) {
                    // Two-sided collision.
                    t = rayTriangleIntersect(localOrigin, localDir, v0, v2, v1);
                }
                if (t <= 0.0f) continue;

                glm::vec3 localHit = localOrigin + localDir * t;
                glm::vec3 worldHit = glm::vec3(instance.modelMatrix * glm::vec4(localHit, 1.0f));
                // Deliberately no height band around the pivot here.
                //
                // There was one - hits more than 0.90 below or 0.80 above the
                // pivot were dropped - and it is what let the camera through
                // walls. The camera orbits and pitches, so the far end of this
                // ray is normally metres above or below the pivot: at any
                // appreciable pitch every hit near the camera fell outside that
                // 1.7-yard slice and the wall was not there at all. The band was
                // aimed at floor and ramp geometry underfoot, which the normal
                // test above already excludes, so it was rejecting only the
                // walls it was supposed to be finding.
                float worldDist = glm::length(worldHit - origin);
                if (worldDist < MIN_HIT_DISTANCE) continue;
                if (worldDist < closestHit && worldDist <= maxDistance) {
                    closestHit = worldDist;
                }
            }
        }
    }

    return closestHit;
}

} // namespace rendering
} // namespace wowee

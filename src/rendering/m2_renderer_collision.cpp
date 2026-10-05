// M2Renderer's collision queries, moved here unchanged from m2_renderer_instance.cpp and
// m2_renderer.cpp (VITA-51): floor height, collision sweeps, raycasts, the instance spatial index and
// the per-model collision mesh with its triangle grid. None of it touches the GPU. A file of its own
// so that a build with no Vulkan (the PS Vita, ADR-001) compiles these against its own declaration
// of M2Renderer instead of the Vulkan one.
//
// Nothing in this file may name a Vk*/Vma* type or a GPU member of M2ModelGPU; see
// tools/vita/collision_check.sh, which compiles it with the Vita compiler and no Vulkan headers.
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/collision_geometry.hpp"
#include "rendering/query_timer.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>

namespace wowee {
namespace rendering {

// Thread-local scratch buffers for collision queries (moved from header to
// avoid inline thread_local TLS init linker errors on Windows ARM64 / LLD).
namespace m2_internal {
thread_local std::vector<size_t> tl_m2_candidateScratch;
thread_local std::unordered_set<uint32_t> tl_m2_candidateIdScratch;
thread_local std::vector<uint32_t> tl_m2_collisionTriScratch;
} // namespace m2_internal

// ---------------------------------------------------------------------------
// M2 collision mesh: build spatial grid + classify triangles
// ---------------------------------------------------------------------------
void M2ModelGPU::CollisionMesh::build() {
    if (indices.size() < 3 || vertices.empty()) return;
    triCount = static_cast<uint32_t>(indices.size() / 3);

    // Bounding box for grid
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(-std::numeric_limits<float>::max());
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
    }

    gridOrigin = glm::vec2(bmin.x, bmin.y);
    gridCellsX = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.x - bmin.x) / CELL_SIZE))));
    gridCellsY = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.y - bmin.y) / CELL_SIZE))));

    cellFloorTris.resize(static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY));
    cellWallTris.resize(static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY));
    triBounds.resize(triCount);

    for (uint32_t ti = 0; ti < triCount; ti++) {
        uint16_t i0 = indices[ti * 3];
        uint16_t i1 = indices[ti * 3 + 1];
        uint16_t i2 = indices[ti * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

        const auto& v0 = vertices[i0];
        const auto& v1 = vertices[i1];
        const auto& v2 = vertices[i2];

        triBounds[ti].minZ = std::min({v0.z, v1.z, v2.z});
        triBounds[ti].maxZ = std::max({v0.z, v1.z, v2.z});

        glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
        float normalLen = glm::length(normal);
        float absNz = (normalLen > 0.001f) ? std::abs(normal.z / normalLen) : 0.0f;
        bool isFloor = (absNz >= 0.35f);  // ~70° max slope (relaxed for steep stairs)
        bool isWall  = (absNz < 0.65f);

        float triMinX = std::min({v0.x, v1.x, v2.x});
        float triMaxX = std::max({v0.x, v1.x, v2.x});
        float triMinY = std::min({v0.y, v1.y, v2.y});
        float triMaxY = std::max({v0.y, v1.y, v2.y});

        int cxMin = std::clamp(static_cast<int>((triMinX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cxMax = std::clamp(static_cast<int>((triMaxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cyMin = std::clamp(static_cast<int>((triMinY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
        int cyMax = std::clamp(static_cast<int>((triMaxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

        for (int cy = cyMin; cy <= cyMax; cy++) {
            for (int cx = cxMin; cx <= cxMax; cx++) {
                int ci = cy * gridCellsX + cx;
                if (isFloor) cellFloorTris[ci].push_back(ti);
                if (isWall)  cellWallTris[ci].push_back(ti);
            }
        }
    }
}

/// The triangles of one cell array that a query box reaches, deduplicated.
///
/// Floors and walls are asked separately and the two queries differed in one
/// token: which array they read. A triangle spanning several cells is filed
/// under each, so the sort and unique are not tidiness - a caller that tests
/// the same triangle twice counts two hits, and a raycast then reports an even
/// number of crossings where there was one surface.
void M2ModelGPU::CollisionMesh::gatherTrisInRange(
        const std::vector<std::vector<uint32_t>>& cells,
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0) return;

    const int cxMin = std::clamp(static_cast<int>((minX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    const int cxMax = std::clamp(static_cast<int>((maxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    const int cyMin = std::clamp(static_cast<int>((minY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    const int cyMax = std::clamp(static_cast<int>((maxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

    const size_t cellCount = static_cast<size_t>(cxMax - cxMin + 1) *
                             static_cast<size_t>(cyMax - cyMin + 1);
    out.reserve(cellCount * 8);
    for (int cy = cyMin; cy <= cyMax; cy++) {
        for (int cx = cxMin; cx <= cxMax; cx++) {
            const auto& cell = cells[cy * gridCellsX + cx];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void M2ModelGPU::CollisionMesh::getFloorTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellFloorTris, minX, minY, maxX, maxY, out);
}

void M2ModelGPU::CollisionMesh::getWallTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellWallTris, minX, minY, maxX, maxY, out);
}

void M2Renderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocus.set(worldPos, radius);
}

void M2Renderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
}

void M2Renderer::rebuildSpatialIndex() {
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    instanceIndexById.reserve(instances.size());
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();

    for (size_t i = 0; i < instances.size(); i++) {
        auto& inst = instances[i];
        instanceIndexById[inst.id] = i;

        // Re-cache model pointer (may have changed after model map modifications)
        auto mdlIt = models.find(inst.modelId);
        inst.cachedModel = (mdlIt != models.end()) ? &mdlIt->second : nullptr;

        // Rebuild dedup map (skip ground detail)
        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{.modelId = inst.modelId,
                        .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            instanceDedupMap_[dk] = inst.id;
        }

        if (inst.cachedIsSmoke) {
            smokeInstanceIndices_.push_back(i);
        }
        if (inst.cachedIsInstancePortal) {
            portalInstanceIndices_.push_back(i);
        }
        if (inst.cachedHasParticleEmitters) {
            particleInstanceIndices_.push_back(i);
        }
        if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
            animatedInstanceIndices_.push_back(i);
        } else if (inst.cachedHasParticleEmitters) {
            particleOnlyInstanceIndices_.push_back(i);
        }

        insertBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, inst.id);
    }
    spatialIndexDirty_ = false;
}

void M2Renderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                  std::vector<size_t>& outIndices) const {
    outIndices.clear();

    gatherIds(spatialGrid, queryMin, queryMax, tl_m2_candidateIdScratch,
              [&](uint32_t id) {
                  auto idxIt = instanceIndexById.find(id);
                  if (idxIt != instanceIndexById.end()) {
                      outIndices.push_back(idxIt->second);
                  }
              });

    // Safety fallback to preserve collision correctness if the spatial index
    // misses candidates (e.g. during streaming churn).
    if (outIndices.empty() && !instances.empty()) {
        outIndices.reserve(instances.size());
        for (size_t i = 0; i < instances.size(); i++) {
            outIndices.push_back(i);
        }
    }
}

void M2Renderer::debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const {
    LOG_WARNING("=== M2 Floor Debug at render(", glX, ", ", glY, ", ", glZ, ") ===");
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);
    int reported = 0;
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        const char* rejected = nullptr;
        if (collisionFocus.excludes(instance.worldBoundsMin, instance.worldBoundsMax)) {
            rejected = "outside the collision focus";
        } else if (!instance.cachedModel) {
            rejected = "no model";
        } else if (instance.scale <= 0.001f) {
            rejected = "zero scale";
        } else if (instance.skipCollision) {
            rejected = "skipCollision";
        }
        const M2ModelGPU* model = instance.cachedModel;
        const bool authored = model && model->collision.valid();
        if (!rejected && model) {
            if ((model->collisionNoBlock && !authored) || model->isInvisibleTrap ||
                model->isSpellEffect) {
                rejected = "classified as non-blocking";
            }
        }
        if (++reported > 12) { LOG_WARNING("  ... and more"); break; }
        LOG_WARNING("  '", model ? model->name : std::string("?"),
                    "' isGameObject=", instance.isGameObject ? 1 : 0,
                    " authoredCollision=", authored ? 1 : 0,
                    " tris=", authored ? model->collision.triCount : 0u,
                    " bounds z ", instance.worldBoundsMin.z, "..", instance.worldBoundsMax.z,
                    (rejected ? "  REJECTED: " : "  considered"), (rejected ? rejected : ""));
    }
    if (reported == 0) {
        LOG_WARNING("  nothing at all - no doodad instance overlaps this spot,"
                    " so a deck underfoot is not in the spatial grid here");
    }
    LOG_WARNING("=== Total: ", reported, " M2 candidates ===");
}

bool M2Renderer::getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin,
                                        glm::vec3& outMax) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    outMin = inst.worldBoundsMin;
    outMax = inst.worldBoundsMax;
    return true;
}

std::optional<float> M2Renderer::getInstanceFloorHeight(uint32_t instanceId,
                                                       float glX, float glY,
                                                       float glZ) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) {
        return std::nullopt;
    }
    const auto& instance = instances[idxIt->second];
    if (!instance.cachedModel || instance.scale <= 0.001f) return std::nullopt;
    const M2ModelGPU& model = *instance.cachedModel;
    if (!model.collision.valid()) return std::nullopt;

    // Same cast as getFloorHeight: world-down through the instance transform,
    // so a car placed with any pitch still sees a ray along gravity.
    const glm::vec3 localRayOrigin = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
    const glm::vec3 localRayEnd = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
    const glm::vec3 localRayVector = localRayEnd - localRayOrigin;
    const float localRayLength = glm::length(localRayVector);
    if (localRayLength <= 1e-5f) return std::nullopt;
    const glm::vec3 localRayDir = localRayVector / localRayLength;

    model.collision.getFloorTrisInRange(
        std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
        std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
        std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
        std::max(localRayOrigin.y, localRayEnd.y) + 1.0f,
        tl_m2_collisionTriScratch);

    std::optional<float> best;
    for (uint32_t ti : tl_m2_collisionTriScratch) {
        if (ti >= model.collision.triCount) continue;
        const auto& verts = model.collision.vertices;
        const auto& idx = model.collision.indices;
        const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir,
                                                verts[idx[ti * 3]],
                                                verts[idx[ti * 3 + 1]],
                                                verts[idx[ti * 3 + 2]]);
        if (tHit < 0.0f || tHit > localRayLength) continue;
        const glm::vec3 worldHit = glm::vec3(
            instance.modelMatrix * glm::vec4(localRayOrigin + localRayDir * tHit, 1.0f));
        // At or under the probe, and the highest such - the deck rather than
        // whatever structure the car carries beneath it.
        if (worldHit.z <= glZ && (!best || worldHit.z > *best)) best = worldHit.z;
    }
    return best;
}

std::optional<float> M2Renderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;  // Default to flat

    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        if (!instance.cachedModel) continue;
        if (instance.scale <= 0.001f) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A model that ships collision geometry blocks, whatever its name
        // suggests. collisionNoBlock is a guess from the name and the unscaled
        // bounds, and it is only there to stop collision being *invented* for
        // something soft - it has no business discarding what the artist
        // authored.
        //
        // It was discarding a great deal. hellfiretreethorns03 carries 15,376
        // collision triangles and is 2.87 wide unscaled, so it failed the
        // treeWithTrunk gate of horiz > 6, came out a softTree, and was walked
        // through. Every bush and rug beside it ships zero collision triangles,
        // so they stay soft on their own evidence and need no rule at all.
        const bool authoredCollision = model.collision.valid();
        if ((model.collisionNoBlock && !authoredCollision) ||
            model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision) continue;

        // --- Mesh-based floor: vertical ray vs collision triangles ---
        // A successful authored-mesh hit wins; AABB remains the fallback on a miss.
        if (model.collision.valid()) {
            // Cast world-down through the instance transform. Using local -Z
            // only works for yaw-only placements; pitched flat models (notably
            // Exodarplatform01, placed as the Azuremyst flight-master ramp)
            // otherwise see a ray that is diagonal relative to world gravity.
            const glm::vec3 localRayOrigin = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
            const glm::vec3 localRayEnd = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
            glm::vec3 localRayVector = localRayEnd - localRayOrigin;
            const float localRayLength = glm::length(localRayVector);
            if (localRayLength <= 1e-5f) continue;
            const glm::vec3 localRayDir = localRayVector / localRayLength;

            model.collision.getFloorTrisInRange(
                std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
                std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
                std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
                std::max(localRayOrigin.y, localRayEnd.y) + 1.0f,
                tl_m2_collisionTriScratch);

            float bestWorldHitZ = -std::numeric_limits<float>::max();
            float bestHitNormalZ = 1.0f;
            bool hitAny = false;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                // The intersection is already two-sided, so the reversed
                // winding this used to retry answers the same thing.
                const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2);
                if (tHit < 0.0f || tHit > localRayLength) continue;

                const glm::vec3 localHit = localRayOrigin + localRayDir * tHit;
                const glm::vec3 worldHit = glm::vec3(
                    instance.modelMatrix * glm::vec4(localHit, 1.0f));

                // Walkable normal check (world space)
                glm::vec3 worldN(0.0f, 0.0f, 1.0f);  // Default to flat
                glm::vec3 localN = glm::cross(v1 - v0, v2 - v0);
                float nLen = glm::length(localN);
                if (nLen > 0.001f) {
                    localN /= nLen;
                    if (localN.z < 0.0f) localN = -localN;
                    glm::vec3 transformedN = glm::vec3(
                        instance.modelMatrix * glm::vec4(localN, 0.0f));
                    float wnLen = glm::length(transformedN);
                    if (wnLen > 0.001f) {
                        worldN = transformedN / wnLen;
                    } // else: keep worldN = (0,0,1) flat default
                    if (std::abs(worldN.z) < 0.35f) continue; // too steep (~70° max slope)
                }

                if (worldHit.z <= glZ + 3.0f && worldHit.z > bestWorldHitZ) {
                    bestWorldHitZ = worldHit.z;
                    hitAny = true;
                    bestHitNormalZ = std::abs(worldN.z);
                }
            }

            if (hitAny) {
                if (!bestFloor || bestWorldHitZ > *bestFloor) {
                    bestFloor = bestWorldHitZ;
                    bestNormalZ = bestHitNormalZ;
                }
                // A successful authored-mesh hit is more accurate than the
                // model AABB top, especially for pitched ramps where the AABB
                // projection is not a world-vertical intersection.
                continue;
            }
            // Fall through to AABB floor - both contribute, highest wins
        }

        float zMargin = model.collisionBridge ? 25.0f : 2.0f;
        if (glX < instance.worldBoundsMin.x || glX > instance.worldBoundsMax.x ||
            glY < instance.worldBoundsMin.y || glY > instance.worldBoundsMax.y ||
            glZ < instance.worldBoundsMin.z - zMargin || glZ > instance.worldBoundsMax.z + zMargin) {
            continue;
        }
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);

        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

        // Must be within doodad footprint in local XY.
        // Stepped low platforms get a small pad so walk-up snapping catches edges.
        float footprintPad = 0.0f;
        if (model.collisionSteppedLowPlatform) {
            footprintPad = model.collisionPlanter ? 0.22f : 0.16f;
            if (model.collisionBridge) {
                footprintPad = 0.35f;
            }
        }
        if (localPos.x < localMin.x - footprintPad || localPos.x > localMax.x + footprintPad ||
            localPos.y < localMin.y - footprintPad || localPos.y > localMax.y + footprintPad) {
            continue;
        }

        // Construct "top" point at queried XY in local space, then transform back.
        float localTopZ = getEffectiveCollisionTopLocal(model, localPos, localMin, localMax);
        glm::vec3 localTop(localPos.x, localPos.y, localTopZ);
        glm::vec3 worldTop = glm::vec3(instance.modelMatrix * glm::vec4(localTop, 1.0f));

        // Reachability filter: allow a bit more climb for stepped low platforms.
        float maxStepUp = 1.0f;
        if (model.collisionStatue) {
            maxStepUp = 2.5f;
        } else if (model.collisionSmallSolidProp) {
            maxStepUp = 2.0f;
        } else if (model.collisionSteppedFountain) {
            maxStepUp = 2.5f;
        } else if (model.collisionSteppedLowPlatform) {
            maxStepUp = model.collisionPlanter ? 3.0f : 2.4f;
            if (model.collisionBridge) {
                maxStepUp = 25.0f;
            }
        }
        if (worldTop.z > glZ + maxStepUp) continue;

        if (!bestFloor || worldTop.z > *bestFloor) {
            bestFloor = worldTop.z;
        }
    }

    // Output surface normal if requested
    if (outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

bool M2Renderer::checkCollision(const glm::vec3& from, const glm::vec3& to,
                                 glm::vec3& adjustedPos, float playerRadius) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    adjustedPos = to;
    bool collided = false;

    glm::vec3 queryMin = glm::min(from, to) - glm::vec3(7.0f, 7.0f, 5.0f);
    glm::vec3 queryMax = glm::max(from, to) + glm::vec3(7.0f, 7.0f, 5.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    // Check against all M2 instances in local space (rotation-aware).
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        const float broadMargin = playerRadius + 1.0f;
        if (from.x < instance.worldBoundsMin.x - broadMargin && adjustedPos.x < instance.worldBoundsMin.x - broadMargin) continue;
        if (from.x > instance.worldBoundsMax.x + broadMargin && adjustedPos.x > instance.worldBoundsMax.x + broadMargin) continue;
        if (from.y < instance.worldBoundsMin.y - broadMargin && adjustedPos.y < instance.worldBoundsMin.y - broadMargin) continue;
        if (from.y > instance.worldBoundsMax.y + broadMargin && adjustedPos.y > instance.worldBoundsMax.y + broadMargin) continue;
        if (from.z > instance.worldBoundsMax.z + 2.5f && adjustedPos.z > instance.worldBoundsMax.z + 2.5f) continue;
        if (from.z + 2.5f < instance.worldBoundsMin.z && adjustedPos.z + 2.5f < instance.worldBoundsMin.z) continue;

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A model that ships collision geometry blocks, whatever its name
        // suggests. collisionNoBlock is a guess from the name and the unscaled
        // bounds, and it is only there to stop collision being *invented* for
        // something soft - it has no business discarding what the artist
        // authored.
        //
        // It was discarding a great deal. hellfiretreethorns03 carries 15,376
        // collision triangles and is 2.87 wide unscaled, so it failed the
        // treeWithTrunk gate of horiz > 6, came out a softTree, and was walked
        // through. Every bush and rug beside it ships zero collision triangles,
        // so they stay soft on their own evidence and need no rule at all.
        const bool authoredCollision = model.collision.valid();
        if ((model.collisionNoBlock && !authoredCollision) ||
            model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision || instance.skipWallCollision) continue;
        if (instance.scale <= 0.001f) continue;

        // --- Mesh-based wall collision: closest-point push ---
        if (model.collision.valid()) {
            glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
            glm::vec3 localPos  = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
            float localRadius = playerRadius / instance.scale;

            model.collision.getWallTrisInRange(
                std::min(localFrom.x, localPos.x) - localRadius - 1.0f,
                std::min(localFrom.y, localPos.y) - localRadius - 1.0f,
                std::max(localFrom.x, localPos.x) + localRadius + 1.0f,
                std::max(localFrom.y, localPos.y) + localRadius + 1.0f,
                tl_m2_collisionTriScratch);

            constexpr float PLAYER_HEIGHT = 2.0f;
#ifdef __vita__
            // The upstream push (0.015 yards a step, 0.02 in total per instance) is a nudge: at walking speed a prop with
            // authored collision only slowed the player (measured on the Vita: a point walked into a fence ended 0.05 yards
            // from where it was heading). Resolve the overlap instead, up to the player's radius.
            constexpr float MAX_TOTAL_PUSH = 0.6f;
            constexpr float MAX_STEP_PUSH = 0.30f;
#else
            constexpr float MAX_TOTAL_PUSH = 0.02f; // Cap total push per instance
            constexpr float MAX_STEP_PUSH = 0.015f;
#endif
            bool pushed = false;
            float totalPushX = 0.0f, totalPushY = 0.0f;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;
                if (localPos.z + PLAYER_HEIGHT < model.collision.triBounds[ti].minZ ||
                    localPos.z > model.collision.triBounds[ti].maxZ) continue;

                // Step-up: only skip wall when player is rising (jumping over it)
                constexpr float MAX_STEP_UP = 1.2f;
                bool rising = (localPos.z > localFrom.z + 0.05f);
                if (rising && localPos.z + MAX_STEP_UP >= model.collision.triBounds[ti].maxZ) continue;

                // Early out if we already pushed enough this instance
                float totalPushSoFar = std::sqrt(totalPushX * totalPushX + totalPushY * totalPushY);
                if (totalPushSoFar >= MAX_TOTAL_PUSH) break;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                glm::vec3 closest = closestPointOnTriangle(localPos, v0, v1, v2);
                glm::vec3 diff = localPos - closest;
                float distXY = std::sqrt(diff.x * diff.x + diff.y * diff.y);

                if (distXY < localRadius && distXY > 1e-4f) {
                    // Gentle push - very small fraction of penetration
                    float penetration = localRadius - distXY;
#ifdef __vita__
                    float pushDist = std::clamp(penetration, 0.001f, MAX_STEP_PUSH);
#else
                    float pushDist = std::clamp(penetration * 0.08f, 0.001f, MAX_STEP_PUSH);
#endif
                    float dx = (diff.x / distXY) * pushDist;
                    float dy = (diff.y / distXY) * pushDist;
                    localPos.x += dx;
                    localPos.y += dy;
                    totalPushX += dx;
                    totalPushY += dy;
                    pushed = true;
                } else if (distXY < 1e-4f) {
                    // On the plane - soft push along triangle normal XY
                    glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
                    float nxyLen = std::sqrt(n.x * n.x + n.y * n.y);
                    if (nxyLen > 1e-4f) {
                        float pushDist = std::min(localRadius, MAX_STEP_PUSH);
                        float dx = (n.x / nxyLen) * pushDist;
                        float dy = (n.y / nxyLen) * pushDist;
                        localPos.x += dx;
                        localPos.y += dy;
                        totalPushX += dx;
                        totalPushY += dy;
                        pushed = true;
                    }
                }
            }

            if (pushed) {
                glm::vec3 worldPos = glm::vec3(instance.modelMatrix * glm::vec4(localPos, 1.0f));
                adjustedPos.x = worldPos.x;
                adjustedPos.y = worldPos.y;
                collided = true;
                // Which doodad is in the way, once per model.
                //
                // A model that blocks and should not is reported as "I am
                // stuck on this bush", and the one thing needed to fix it -
                // the model's name - is the one thing nobody can see. The
                // classifier decides collision from that name, so without it
                // the only way forward is guessing tokens, which is how the
                // folder-token regression happened.
                //
                // Once per distinct model, not per frame: walking into
                // something touches it every frame for as long as you lean on
                // it.
                static std::set<std::string> saidBlocked;
                if (!model.name.empty() && saidBlocked.insert(model.name).second) {
                    LOG_WARNING("Collision: blocked by '", model.name,
                                "' - if this should be walked through, that is "
                                "the name the classifier needs");
                }
            }
            continue;
        }

        glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
        float radiusScale = model.collisionNarrowVerticalProp ? 0.45f : 1.0f;
        float localRadius = (playerRadius * radiusScale) / instance.scale;

        glm::vec3 rawMin, rawMax;
        getTightCollisionBounds(model, rawMin, rawMax);
        glm::vec3 localMin = rawMin - glm::vec3(localRadius);
        glm::vec3 localMax = rawMax + glm::vec3(localRadius);
        float effectiveTop = getEffectiveCollisionTopLocal(model, localPos, rawMin, rawMax) + localRadius;
        glm::vec2 localCenter((localMin.x + localMax.x) * 0.5f, (localMin.y + localMax.y) * 0.5f);
        float fromR = glm::length(glm::vec2(localFrom.x, localFrom.y) - localCenter);
        float toR = glm::length(glm::vec2(localPos.x, localPos.y) - localCenter);

        // Feet-based vertical overlap test: ignore objects fully above/below us.
        constexpr float PLAYER_HEIGHT = 2.0f;
        if (localPos.z + PLAYER_HEIGHT < localMin.z || localPos.z > effectiveTop) {
            continue;
        }

        bool fromInsideXY =
            (localFrom.x >= localMin.x && localFrom.x <= localMax.x &&
             localFrom.y >= localMin.y && localFrom.y <= localMax.y);
        bool fromInsideZ = (localFrom.z + PLAYER_HEIGHT >= localMin.z && localFrom.z <= effectiveTop);
        bool escapingOverlap = (fromInsideXY && fromInsideZ && (toR > fromR + 1e-4f));
        bool allowEscapeRelax = escapingOverlap && !model.collisionSmallSolidProp;

        // Swept hard clamp for taller blockers only.
        // Low/stepable objects should be climbable and not "shove" the player off.
        float maxStepUp = 1.20f;
        if (model.collisionStatue) {
            maxStepUp = 2.5f;
        } else if (model.collisionSmallSolidProp) {
            // Keep box/crate-class props hard-solid to prevent phase-through.
            maxStepUp = 0.75f;
        } else if (model.collisionSteppedFountain) {
            maxStepUp = 2.5f;
        } else if (model.collisionSteppedLowPlatform) {
            maxStepUp = model.collisionPlanter ? 2.8f : 2.4f;
            if (model.collisionBridge) {
                maxStepUp = 25.0f;
            }
        }
        bool stepableLowObject = (effectiveTop <= localFrom.z + maxStepUp);
        bool climbingAttempt = (localPos.z > localFrom.z + 0.18f);
        bool nearTop = (localFrom.z >= effectiveTop - 0.30f);
        float climbAllowance = model.collisionPlanter ? 0.95f : 0.60f;
        if (model.collisionSteppedLowPlatform && !model.collisionPlanter) {
            // Let low curb/planter blocks be stepable without sticky side shoves.
            climbAllowance = 1.00f;
        }
        if (model.collisionBridge) {
            climbAllowance = 3.0f;
        }
        if (model.collisionSmallSolidProp) {
            climbAllowance = 1.05f;
        }
        bool climbingTowardTop = climbingAttempt && (localFrom.z + climbAllowance >= effectiveTop);
        bool forceHardLateral =
            model.collisionSmallSolidProp &&
            !nearTop && !climbingTowardTop;
        if ((!stepableLowObject || forceHardLateral) && !allowEscapeRelax) {
            float tEnter = 0.0f;
            glm::vec3 sweepMax = localMax;
            sweepMax.z = std::min(sweepMax.z, effectiveTop);
            if (segmentIntersectsAABB(localFrom, localPos, localMin, sweepMax, tEnter)) {
                float tSafe = std::clamp(tEnter - 0.03f, 0.0f, 1.0f);
                glm::vec3 localSafe = localFrom + (localPos - localFrom) * tSafe;
                glm::vec3 worldSafe = glm::vec3(instance.modelMatrix * glm::vec4(localSafe, 1.0f));
                adjustedPos.x = worldSafe.x;
                adjustedPos.y = worldSafe.y;
                collided = true;
                // The other way a doodad blocks, and the one that was missing.
                //
                // The first of these lines went inside the branch for models
                // that carry a collision mesh. A model without one is stopped
                // by its bounding box here instead, so the doodads reported as
                // wrongly solid - grass among them - were exactly the ones the
                // diagnostic could not see.
                static std::set<std::string> saidBoxBlocked;
                if (!model.name.empty() && saidBoxBlocked.insert(model.name).second) {
                    LOG_WARNING("Collision: blocked by '", model.name,
                                "' (bounding box, no collision mesh) - if this "
                                "should be walked through, that is the name the "
                                "classifier needs");
                }
                continue;
            }
        }

        if (localPos.x < localMin.x || localPos.x > localMax.x ||
            localPos.y < localMin.y || localPos.y > localMax.y) {
            continue;
        }

        float pushLeft  = localPos.x - localMin.x;
        float pushRight = localMax.x - localPos.x;
        float pushBack  = localPos.y - localMin.y;
        float pushFront = localMax.y - localPos.y;

        float minPush = std::min({pushLeft, pushRight, pushBack, pushFront});
        if (allowEscapeRelax) {
            continue;
        }
        if (stepableLowObject && localFrom.z >= effectiveTop - 0.35f) {
            // Already on/near top surface: don't apply lateral push that ejects
            // the player from the object (carpets, platforms, etc).
            continue;
        }
        // Gentle fallback push for overlapping cases.
        float pushAmount;
        if (model.collisionNarrowVerticalProp) {
            pushAmount = std::clamp(minPush * 0.10f, 0.001f, 0.010f);
        } else if (model.collisionSteppedLowPlatform) {
            if (model.collisionPlanter && stepableLowObject) {
                pushAmount = std::clamp(minPush * 0.06f, 0.001f, 0.006f);
            } else {
            pushAmount = std::clamp(minPush * 0.12f, 0.003f, 0.012f);
            }
        } else if (stepableLowObject) {
            pushAmount = std::clamp(minPush * 0.12f, 0.002f, 0.015f);
        } else {
            pushAmount = std::clamp(minPush * 0.28f, 0.010f, 0.045f);
        }
        glm::vec3 localPush(0.0f);
        if (minPush == pushLeft) {
            localPush.x = -pushAmount;
        } else if (minPush == pushRight) {
            localPush.x = pushAmount;
        } else if (minPush == pushBack) {
            localPush.y = -pushAmount;
        } else {
            localPush.y = pushAmount;
        }

        glm::vec3 worldPush = glm::vec3(instance.modelMatrix * glm::vec4(localPush, 0.0f));
        adjustedPos.x += worldPush.x;
        adjustedPos.y += worldPush.y;
        collided = true;
    }

    return collided;
}

float M2Renderer::raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;

    glm::vec3 rayEnd = origin + direction * maxDistance;
    glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        // Cheap world-space broad-phase.
        float tEnter = 0.0f;
        glm::vec3 worldMin = instance.worldBoundsMin - glm::vec3(0.35f);
        glm::vec3 worldMax = instance.worldBoundsMax + glm::vec3(0.35f);
        if (!segmentIntersectsAABB(origin, origin + direction * maxDistance, worldMin, worldMax, tEnter)) {
            continue;
        }

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A model that ships collision geometry blocks, whatever its name
        // suggests. collisionNoBlock is a guess from the name and the unscaled
        // bounds, and it is only there to stop collision being *invented* for
        // something soft - it has no business discarding what the artist
        // authored.
        //
        // It was discarding a great deal. hellfiretreethorns03 carries 15,376
        // collision triangles and is 2.87 wide unscaled, so it failed the
        // treeWithTrunk gate of horiz > 6, came out a softTree, and was walked
        // through. Every bush and rug beside it ships zero collision triangles,
        // so they stay soft on their own evidence and need no rule at all.
        const bool authoredCollision = model.collision.valid();
        if ((model.collisionNoBlock && !authoredCollision) ||
            model.isInvisibleTrap || model.isSpellEffect) continue;
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);
        // Skip tiny doodads for camera occlusion; they cause jitter and false hits.
        glm::vec3 extents = (localMax - localMin) * instance.scale;
        if (glm::dot(extents, extents) < 0.5625f) continue;

        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f)));
        if (!std::isfinite(localDir.x) || !std::isfinite(localDir.y) || !std::isfinite(localDir.z)) {
            continue;
        }

        // Local-space AABB slab intersection.
        glm::vec3 invDir = 1.0f / localDir;
        glm::vec3 tMin = (localMin - localOrigin) * invDir;
        glm::vec3 tMax = (localMax - localOrigin) * invDir;
        glm::vec3 t1 = glm::min(tMin, tMax);
        glm::vec3 t2 = glm::max(tMin, tMax);

        float tNear = std::max({t1.x, t1.y, t1.z});
        float tFar = std::min({t2.x, t2.y, t2.z});
        if (tNear > tFar || tFar <= 0.0f) continue;

        float tHit = tNear > 0.0f ? tNear : tFar;
        glm::vec3 localHit = localOrigin + localDir * tHit;
        glm::vec3 worldHit = glm::vec3(instance.modelMatrix * glm::vec4(localHit, 1.0f));
        float worldDist = glm::length(worldHit - origin);
        if (worldDist > 0.0f && worldDist < closestHit) {
            closestHit = worldDist;
        }
    }

    return closestHit;
}

} // namespace rendering
} // namespace wowee

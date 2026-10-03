#include <cstdlib>
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <set>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace wowee {
namespace rendering {

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return;
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];

    // The box the instance is filed under, which is what it has to be taken
    // out of once it has moved.
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;

    inst.position = position;
    inst.updateModelMatrix();
    // Use cachedModel instead of a fresh models.find() - the pointer was set
    // at addInstance and stays valid as long as the instance exists.
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }

    // Incrementally update spatial grid
    refileBounds(spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
}

void M2Renderer::setInstanceFade(uint32_t instanceId, float alpha) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].fade = std::clamp(alpha, 0.0f, 1.0f);
}

void M2Renderer::setInstanceHighlight(uint32_t instanceId, float amount) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].highlight = amount;
}

void M2Renderer::clearInstanceHighlights() {
    // A walk of every instance rather than a record of the lit one, because a
    // press can end with the instance gone - the object despawned, the tile
    // unloaded - and a remembered id would then light whatever took its slot.
    for (auto& inst : instances) inst.highlight = 0.0f;
}

void M2Renderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animSpeed = frozen ? 0.0f : 1.0f;
    inst.holdAtEnd = false;
    if (frozen) {
        inst.animTime = 0.0f;  // Reset to bind pose
    }
}

void M2Renderer::restartInstanceAnimation(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animTime = 0.0f;
    inst.animTimeAlt = 0.0f;
    inst.animDir = 1.0f;
    if (inst.cachedModel) computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
}

std::optional<uint32_t> M2Renderer::soleSequenceId(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel || inst.cachedModel->sequences.size() != 1) return std::nullopt;
    return inst.cachedModel->sequences[0].id;
}

void M2Renderer::setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                          bool skipToEnd) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != animationId) continue;
        inst.currentSequenceIndex = i;
        inst.animDuration = static_cast<float>(seqs[i].duration);
        inst.playingVariation = false;   // not a variation: it does not go back
        inst.holdAtEnd = true;
        if (skipToEnd) {
            inst.animTime = inst.animDuration;
            inst.animSpeed = 0.0f;
        } else {
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
        }
        return;
    }
}

void M2Renderer::setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    // Find the first sequence matching the requested animation ID
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id == animationId) {
            inst.currentSequenceIndex = i;
            inst.animDuration = static_cast<float>(seqs[i].duration);
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
            // Use playingVariation=true for one-shot (returns to idle when done)
            inst.playingVariation = !loop;
            inst.holdAtEnd = false;
            return;
        }
    }
}

bool M2Renderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    for (const auto& seq : inst.cachedModel->sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

float M2Renderer::getInstanceAnimDuration(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return 0.0f;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return 0.0f;
    const auto& seqs = inst.cachedModel->sequences;
    if (seqs.empty()) return 0.0f;
    int seqIdx = inst.currentSequenceIndex;
    if (seqIdx < 0 || seqIdx >= static_cast<int>(seqs.size())) seqIdx = 0;
    return seqs[seqIdx].duration; // in milliseconds
}

bool M2Renderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    outCenter = inst.cachedCullCenter;
    outRadius = inst.cachedVisualRadius;
    return outRadius > 0.0f;
}

void M2Renderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    // Reject NaN matrix - would propagate into the model matrix uniform
    // and the spatial-grid bounds, leaving stale grid cells pointing at
    // a NaN-bounded instance.
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(transform[c][r])) return;
    auto& inst = instances[idxIt->second];

    // Remove old grid cells before updating bounds
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;
    const glm::vec3 oldPosition = inst.position;

    // Update model matrix directly
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    // Extract position from transform for bounds
    inst.position = glm::vec3(transform[3]);

    // The dedup map is keyed on position, so it has to move with the instance.
    // It did not, and only rebuildSpatialIndex ever put it right - which this
    // path deliberately avoids. A ship's doodads are created at the origin and
    // moved here a frame later, so the origin key stayed pointing at them and
    // the next ship of the same class was handed the first ship's sails instead
    // of its own. Both hulls then wrote their transform to the one instance, so
    // it rendered at whichever ship updated last, and whichever hull unloaded
    // first destroyed it for the other.
    if (!inst.cachedIsGroundDetail) {
        auto keyFor = [&](const glm::vec3& p) {
            return DedupKey{.modelId = inst.modelId,
                            .qx = static_cast<int32_t>(std::round(p.x * 10.0f)),
                            .qy = static_cast<int32_t>(std::round(p.y * 10.0f)),
                            .qz = static_cast<int32_t>(std::round(p.z * 10.0f))};
        };
        const DedupKey oldKey = keyFor(oldPosition);
        const DedupKey newKey = keyFor(inst.position);
        if (!(oldKey == newKey)) {
            auto oldIt = instanceDedupMap_.find(oldKey);
            // Only drop the entry if it still names this instance.
            if (oldIt != instanceDedupMap_.end() && oldIt->second == instanceId) {
                instanceDedupMap_.erase(oldIt);
            }
            instanceDedupMap_.emplace(newKey, instanceId);
        }
    }

    // Update bounds via the cached model pointer
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }

    // Incrementally update spatial grid (remove old cells, add new cells)
    refileBounds(spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
    // No spatialIndexDirty_ = true - handled incrementally
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    size_t idx = idxIt->second;
    if (idx >= instances.size()) return;

    auto& inst = instances[idx];

    // Remove from spatial grid incrementally (same pattern as the move-update path)
    eraseBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, instanceId);

    // Remove from dedup map
    if (!inst.cachedIsGroundDetail) {
        DedupKey dk{.modelId = inst.modelId,
                    .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
        instanceDedupMap_.erase(dk);
    }

    destroyInstanceBones(inst, /*defer=*/true);

    // Swap-remove: move last element to the hole and pop_back to avoid O(n) shift
    instanceIndexById.erase(instanceId);
    if (idx < instances.size() - 1) {
        uint32_t movedId = instances.back().id;
        instances[idx] = std::move(instances.back());
        instances.pop_back();
        instanceIndexById[movedId] = idx;
    } else {
        instances.pop_back();
    }

    // Rebuild the lightweight auxiliary index vectors (smoke, portal, etc.)
    // These are small vectors of indices that are rebuilt cheaply.
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    for (size_t i = 0; i < instances.size(); i++) {
        auto& ri = instances[i];
        if (ri.cachedIsSmoke) smokeInstanceIndices_.push_back(i);
        if (ri.cachedIsInstancePortal) portalInstanceIndices_.push_back(i);
        if (ri.cachedHasParticleEmitters) particleInstanceIndices_.push_back(i);
        if (ri.cachedHasAnimation && !ri.cachedDisableAnimation)
            animatedInstanceIndices_.push_back(i);
        else if (ri.cachedHasParticleEmitters)
            particleOnlyInstanceIndices_.push_back(i);
    }
}

void M2Renderer::setInstanceIsGameObject(uint32_t instanceId, bool isGameObject) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return;
    instances[idxIt->second].isGameObject = isGameObject;
}

void M2Renderer::setSkipCollision(uint32_t instanceId, bool skip) {
    for (auto& inst : instances) {
        if (inst.id == instanceId) {
            inst.skipCollision = skip;
            return;
        }
    }
}

void M2Renderer::setSkipWallCollision(uint32_t instanceId, bool skip) {
    for (auto& inst : instances) {
        if (inst.id == instanceId) {
            inst.skipWallCollision = skip;
            return;
        }
    }
}

void M2Renderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || instances.empty()) {
        return;
    }

    std::unordered_set<uint32_t> toRemove(instanceIds.begin(), instanceIds.end());
    const size_t oldSize = instances.size();
    for (auto& inst : instances) {
        if (toRemove.count(inst.id)) {
            destroyInstanceBones(inst, /*defer=*/true);
        }
    }
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                   [&toRemove](const M2Instance& inst) {
                       return toRemove.find(inst.id) != toRemove.end();
                   }),
                   instances.end());

    if (instances.size() != oldSize) {
        rebuildSpatialIndex();
    }
}

void M2Renderer::clear() {
    if (vkCtx_) {
        vkDeviceWaitIdle(vkCtx_->getDevice());
        for (auto& [id, model] : models) {
            destroyModelGPU(model);
        }
        for (auto& inst : instances) {
            destroyInstanceBones(inst);
        }
        // Reset descriptor pools so new allocations succeed after reload.
        // destroyModelGPU/destroyInstanceBones don't free individual sets,
        // so the pools fill up across map changes without this reset.
        VkDevice device = vkCtx_->getDevice();
        if (materialDescPool_) {
            vkResetDescriptorPool(device, materialDescPool_, 0);
            // Re-allocate the glow texture descriptor set (pre-allocated during init,
            // invalidated by pool reset).
            // Cleared before the test, not inside it: the pool reset above
            // invalidated whatever was here, so a run that does not re-allocate
            // was leaving a dangling set behind to be bound.
            glowTexDescSet_ = VK_NULL_HANDLE;
            // Valid, not merely present. A texture whose upload or view
            // creation failed writes a null view into this set, and the glow
            // pass samples it - the same shape as the particle and ribbon sets.
            if (glowTexture_ && glowTexture_->isValid() && particleTexLayout_) {
                VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = materialDescPool_;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &particleTexLayout_;
                if (vkAllocateDescriptorSets(device, &ai, &glowTexDescSet_) == VK_SUCCESS) {
                    VkDescriptorImageInfo imgInfo = glowTexture_->descriptorInfo();
                    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = glowTexDescSet_;
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &imgInfo;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                }
            }
        }
        if (boneDescPool_) {
            if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
            vkResetDescriptorPool(device, boneDescPool_, 0);
            // Re-allocate the dummy bone set (invalidated by pool reset)
            dummyBoneSet_ = allocateBoneSet();
            if (dummyBoneSet_ && dummyBoneBuffer_) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = dummyBoneBuffer_;
                bufInfo.offset = 0;
                bufInfo.range = sizeof(glm::mat4);
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = dummyBoneSet_;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
            // Re-allocate mega bone sets (invalidated by pool reset)
            for (int i = 0; i < 2; i++) {
                megaBoneSet_[i] = allocateBoneSet();
                if (megaBoneSet_[i] && megaBoneBuffer_[i]) {
                    VkDescriptorBufferInfo mbInfo{};
                    mbInfo.buffer = megaBoneBuffer_[i];
                    mbInfo.offset = 0;
                    mbInfo.range = VkDeviceSize(MEGA_BONE_MATRIX_CAPACITY) * sizeof(glm::mat4);
                    VkWriteDescriptorSet mw{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    mw.dstSet = megaBoneSet_[i];
                    mw.dstBinding = 0;
                    mw.descriptorCount = 1;
                    mw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    mw.pBufferInfo = &mbInfo;
                    vkUpdateDescriptorSets(device, 1, &mw, 0, nullptr);
                }
            }
        }
    }
    models.clear();
    pinnedModelIds_.clear();
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    for (auto& ids : cullSubmittedIds_) ids.clear();
    for (auto& ids : cullReadableIds_) ids.clear();
    smokeParticles.clear();
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    smokeEmitAccum = 0.0f;

    // Clear texture cache so stale textures don't block loads for the next
    // character/map.  Without this, the old session's textures fill the cache
    // budget and failedTextureRetryAt_ blocks legitimate reloads, causing an
    // infinite model-load loop on character switch.
    textureCache.clear();
    texturePropsByPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
}

void M2Renderer::clearInstances() {
    if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
    for (auto& inst : instances) destroyInstanceBones(inst);
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    for (auto& ids : cullSubmittedIds_) ids.clear();
    for (auto& ids : cullReadableIds_) ids.clear();
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    smokeParticles.clear();
    smokeEmitAccum = 0.0f;
}

void M2Renderer::setModelPinned(uint32_t modelId, bool pinned) {
    if (pinned) {
        pinnedModelIds_.insert(modelId);
        modelUnusedSince_.erase(modelId);
    } else {
        pinnedModelIds_.erase(modelId);
    }
}

void M2Renderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : instances) {
        usedModelIds.insert(instance.modelId);
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(60);

    // Find models with no instances that have exceeded the grace period.
    // Models that just lost their last instance get tracked but not evicted
    // immediately - this prevents thrashing when GO models are briefly
    // instance-free between despawn and respawn cycles.
    std::vector<uint32_t> toRemove;
    for (const auto& [id, model] : models) {
        if (usedModelIds.find(id) != usedModelIds.end() ||
            pinnedModelIds_.find(id) != pinnedModelIds_.end()) {
            // Model still in use or pinned - clear any pending unused timestamp
            modelUnusedSince_.erase(id);
            continue;
        }
        auto unusedIt = modelUnusedSince_.find(id);
        if (unusedIt == modelUnusedSince_.end()) {
            // First cycle with no instances - start the grace timer
            modelUnusedSince_[id] = now;
        } else if (now - unusedIt->second >= kGracePeriod) {
            // Grace period expired - mark for removal
            toRemove.push_back(id);
            modelUnusedSince_.erase(unusedIt);
        }
    }

    // Delete GPU resources and remove from map.
    // Wait for the GPU to finish all in-flight frames before destroying any
    // buffers - the previous frame's command buffer may still be referencing
    // vertex/index buffers that are about to be freed. Without this wait,
    // the GPU reads freed memory, which can cause VK_ERROR_DEVICE_LOST.
    // Suspected (not yet confirmed) contributor to multi-second freezes seen
    // right after taxi landings - timed here so the next repro pins it down.
    if (!toRemove.empty() && vkCtx_) {
        const auto waitStart = std::chrono::steady_clock::now();
        vkDeviceWaitIdle(vkCtx_->getDevice());
        const float waitMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        LOG_DEBUG("M2 cleanup: vkDeviceWaitIdle took ", waitMs, "ms (", toRemove.size(), " models to remove)");
    }
    for (uint32_t id : toRemove) {
        auto it = models.find(id);
        if (it != models.end()) {
            destroyModelGPU(it->second);
            models.erase(it);
            // Record the eviction so owners caching "uploaded" model IDs can
            // drop it; otherwise their next spawn skips the load as a stale hit
            // and pushes an empty placeholder (missing doodads on revisit).
            reapedModelIds_.push_back(id);
        }
    }

    if (!toRemove.empty()) {
        LOG_INFO("M2 cleanup: removed ", toRemove.size(), " unused models, ", models.size(), " remaining");
    }
}

std::vector<uint32_t> M2Renderer::drainReapedModelIds() {
    std::vector<uint32_t> out;
    out.swap(reapedModelIds_);
    return out;
}

void M2Renderer::unloadModel(uint32_t modelId) {
    auto it = models.find(modelId);
    if (it == models.end()) return;
    if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
    destroyModelGPU(it->second);
    models.erase(it);
    modelUnusedSince_.erase(modelId);
    pinnedModelIds_.erase(modelId);
    reapedModelIds_.push_back(modelId);
}

size_t M2Renderer::evictUnreferencedTextures(size_t bytesNeeded) {
    if (textureCache.empty() || bytesNeeded == 0) return 0;

    // Free a slab rather than exactly what was asked for.
    //
    // Working out what is evictable means walking every loaded model, because
    // the in-use set is what those models point at. Doing that per texture is
    // a walk per load for as long as the cache sits at its ceiling - which is
    // exactly when textures are streaming in fastest. One sweep that frees a
    // slab serves the next few hundred loads instead.
    constexpr size_t kEvictionSlab = 64ull * 1024 * 1024;
    bytesNeeded = std::max(bytesNeeded, kEvictionSlab);

    // What a loaded model still points at. A batch, a particle emitter and a
    // ribbon each keep a raw pointer into this cache, and the per-frame
    // particle and ribbon groups are rebuilt from those - so anything a model
    // refers to is off limits however old it is.
    std::unordered_set<const VkTexture*> inUse;
    inUse.reserve(textureCache.size());
    for (const auto& [modelId, model] : models) {
        for (const auto& batch : model.batches) {
            if (batch.texture) inUse.insert(batch.texture);
        }
        for (const VkTexture* tex : model.particleTextures) {
            if (tex) inUse.insert(tex);
        }
        for (const VkTexture* tex : model.ribbonTextures) {
            if (tex) inUse.insert(tex);
        }
    }

    std::vector<std::pair<uint64_t, std::string>> candidates;
    candidates.reserve(textureCache.size());
    for (const auto& [key, entry] : textureCache) {
        if (!entry.texture || inUse.count(entry.texture.get()) != 0) continue;
        candidates.emplace_back(entry.lastUse, key);
    }
    if (candidates.empty()) return 0;
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    size_t freed = 0;
    size_t dropped = 0;
    for (const auto& [lastUse, key] : candidates) {
        if (freed >= bytesNeeded) break;
        auto it = textureCache.find(key);
        if (it == textureCache.end()) continue;
        freed += it->second.approxBytes;
        textureCacheBytes_ -= std::min(textureCacheBytes_, it->second.approxBytes);
        texturePropsByPtr_.erase(it->second.texture.get());
        // A command buffer submitted a frame or two ago may still be reading
        // it, so the texture outlives this call and dies once every frame slot
        // has fenced. std::function needs a copyable capture, hence the shared
        // pointer around what was a unique one.
        auto doomed = std::shared_ptr<VkTexture>(it->second.texture.release());
        vkCtx_->deferAfterAllFrameFences([doomed]() mutable { doomed.reset(); });
        textureCache.erase(it);
        ++dropped;
    }

    if (freed > 0) {
        // Whatever was refused while the cache was full can be asked for again
        // now. Only those: a texture that failed because its file is missing
        // is not worth retrying every time something else is evicted.
        for (const auto& key : budgetRejected_) {
            failedTextureCache_.erase(key);
            failedTextureRetryAt_.erase(key);
        }
        budgetRejected_.clear();
        LOG_INFO("M2 texture cache: evicted ", dropped, " unreferenced texture(s), freed ",
                 freed / (1024 * 1024), " MB (now ", textureCacheBytes_ / (1024 * 1024),
                 " MB / ", textureCacheBudgetBytes_ / (1024 * 1024), " MB)");
    }
    return freed;
}

VkTexture* M2Renderer::loadTexture(const std::string& path, uint32_t texFlags) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = normalizeKey(path);
    const uint64_t lookupSerial = ++textureLookupSerial_;

    // Check cache
    auto it = textureCache.find(key);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_.get();
    }

    // The black key discards every pixel darker than the threshold, so a
    // texture it is applied to wrongly loses its dark areas.
    //
    // Both the list and what it is matched against now live in
    // assetNameLooksLikeFlame. This asked the whole path, which is what made
    // Outland's sky flicker: HellFireSkyNebula01 has "fire" in it, so every
    // layer of the Hellfire sky was keyed as a flame and lost whichever of its
    // dark pixels fell under the threshold that frame.
    const bool colorKeyBlackHint = assetNameLooksLikeFlame(key);

    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blp = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blp.isValid()) {
        // M2 skins are sampled and their transparency is read from the
        // blocks, so they need no decode.
        blp = assetManager->loadTexture(key, true);
    }
    if (!blp.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureCache_.insert(key);
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            LOG_WARNING("M2: Failed to load texture: ", path);
        }
        return whiteTexture_.get();
    }

    const size_t approxBytes = blp.approxUploadBytes();
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        // Make room before giving up. lastUse has been recorded on every hit
        // and every insert since this cache was written and nothing ever read
        // it: over budget, the newest texture the player looked at was refused
        // and handed back the white one, permanently, while whatever filled
        // the cache first kept its place. A tree whose leaf sheet was refused
        // renders nothing in the main pass and casts the shadow of a solid
        // quad, and the retry timer below is what made that come and go.
        evictUnreferencedTextures(approxBytes);
    }
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        static constexpr size_t kMaxFailedTextureCache = 200000;
        budgetRejected_.insert(key);
        if (failedTextureCache_.size() < kMaxFailedTextureCache) {
            // Cache budget-rejected keys too; without this we repeatedly decode/load
            // the same textures every frame once budget is saturated.
            failedTextureCache_.insert(key);
            failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            LOG_WARNING("M2 texture cache full (", textureCacheBytes_ / (1024 * 1024),
                        " MB / ", textureCacheBudgetBytes_ / (1024 * 1024),
                        " MB), rejecting texture: ", path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    // Whether the texture actually uses alpha. Reads the DXT blocks when the
    // loader kept them, and every fourth decoded byte when it did not; the two
    // agree, which tests/test_blp_alpha_scan.cpp checks against the assets.
    // This was the only thing forcing M2 textures to be decoded.
    const bool hasAlpha = blp.hasTransparency();

    // And whether that alpha is a silhouette or an atlas leftover, which is
    // what an opaque batch has to know before anything keys on it.
    const bool alphaIsSilhouette = blp.alphaIsSilhouette();

    // Create Vulkan texture
    auto tex = std::make_unique<VkTexture>();
    tex->uploadBLP(*vkCtx_, blp);

    // M2Texture flags: bit 0 = WrapS (1=repeat, 0=clamp), bit 1 = WrapT
    VkSamplerAddressMode wrapS = (texFlags & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode wrapT = (texFlags & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // WOWEE_SKY_MIP_BIAS pushes the sky's textures toward smaller mips.
    //
    // Every per-frame quantity behind the sky has now been measured and holds
    // still while it flickers: the lighting, the clock, the culling, the frame
    // time, and every one of the thirty-four batches' material flags. Nothing
    // is alpha tested, colour keyed, or drawn as a glow card. What is left is
    // the texture sample, and the sky is the one model where that is loud:
    // eighteen of its layers are additive, so each layer's sampling noise adds
    // to the last rather than replacing it, and the dome is at its most
    // foreshortened exactly where a turn sweeps fastest.
    //
    // A knob rather than a fixed value, because if this is the cause the right
    // amount is a thing to measure and not to guess.
    static const float skyMipBias = [] {
        const char* set = std::getenv("WOWEE_SKY_MIP_BIAS");
        return set ? std::strtof(set, nullptr) : 0.0f;
    }();
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, wrapS, wrapT,
                       16.0f, skyMode_ ? skyMipBias : 0.0f);

    VkTexture* texPtr = tex.get();

    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    textureCacheBytes_ += e.approxBytes;
    textureCache[key] = std::move(e);
    failedTextureCache_.erase(key);
    failedTextureRetryAt_.erase(key);
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha,
                                  .alphaIsSilhouette = alphaIsSilhouette,
                                  .colorKeyBlack = colorKeyBlackHint};
    LOG_DEBUG("M2: Loaded texture: ", path, " (", blp.width, "x", blp.height, ")");

    return texPtr;
}

uint32_t M2Renderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : instances) {
        if (instance.cachedModel) {
            total += instance.cachedModel->indexCount / 3;
        }
    }
    return total;
}

void M2Renderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layouts)
    destroy(device, opaquePipeline_);
    destroy(device, cutoutPipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, alphaPipeline_);
    destroy(device, additivePipeline_);
    destroy(device, particlePipeline_);
    destroy(device, particleAdditivePipeline_);
    destroy(device, smokePipeline_);
    destroy(device, ribbonPipeline_);
    destroy(device, ribbonAdditivePipeline_);

    // The same ten pipelines initialize() builds, built by the same
    // function. The layouts are untouched above, so it makes none.
    buildMainPassPipelines(perFrameLayout_);

    core::Logger::getInstance().info("M2Renderer: pipelines recreated");
}

void M2Renderer::collectGrassClearings(float minX, float minY, float maxX, float maxY,
                                       std::vector<pipeline::GrassClearingSource>& out) const {
    constexpr float kEase = 4.0f;
    for (const auto& inst : instances) {
        // The ground detail IS the grass's kin - clutter clearing the ground
        // around clutter would eat the whole field.
        if (inst.cachedIsGroundDetail) continue;

        // Footprint radius from the horizontal bounds, capped: a fence post
        // clears its foot, a wagon its wheelbase, and a tree its trunk - not
        // the shadow of its crown, or every forest would be bald.
        const float ex = inst.worldBoundsMax.x - inst.worldBoundsMin.x;
        const float ey = inst.worldBoundsMax.y - inst.worldBoundsMin.y;
        const float clearing = std::clamp(0.35f * std::max(ex, ey), 0.4f, 3.0f);
        const float reach = clearing + kEase;

        const float cx = 0.5f * (inst.worldBoundsMin.x + inst.worldBoundsMax.x);
        const float cy = 0.5f * (inst.worldBoundsMin.y + inst.worldBoundsMax.y);
        if (cx + reach < minX || cx - reach > maxX ||
            cy + reach < minY || cy - reach > maxY) {
            continue;
        }
        out.push_back({cx, cy, cx, cy, clearing, kEase});
    }
}

} // namespace rendering
} // namespace wowee

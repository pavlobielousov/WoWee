// Vita CharacterRenderer (VITA-20, phase A): creatures and NPCs standing in the world, in the bind pose of their model, drawn
// with the static-M2 programs (no bones yet). The class is declared by the shadow header
// (cmake/vita/shadow/rendering/character_renderer.hpp); the data it keeps (`models`, `instances`) is upstream's, the GL half
// is `Gl` below. What upstream's Vulkan CharacterRenderer does per batch (geoset filter, texture choice by type and slot
// override, hair and alpha handling) is copied from character_renderer.cpp so the shared code's calls mean the same thing.
#include "rendering/character_renderer.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/frustum.hpp"
#include "rendering/gl/gl_program.hpp"
#include "rendering/gl/gl_stats.hpp"
#include "rendering/gl/gl_texture.hpp"
#include "rendering/gl/shader_sources.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <vitaGL.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace wowee::rendering {

// The renderer's texture object as the shared code sees it: a name for a GL texture, compared by pointer (white = "no texture").
class GpuTexture {
public:
    GLuint id = 0;
};

namespace gl {
// gl.cfg switches for telling the two ways a character is drawn apart (set by Renderer, read every frame): `noanim` draws every
// character from the shared Stand-pose buffer, `animall` skins every character in range each frame.
bool g_charNoAnim = false, g_charAnimAll = false;
}  // namespace gl

// The GL vertex: position, normal, first UV set.
struct CharacterGlVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

struct CharacterRenderer::Gl {
    struct Uniforms {
        GLint viewProj = -1, model = -1, uvOffset = -1, lightDir = -1, lightColor = -1, ambient = -1, eye = -1, fog = -1,
              lit = -1, texture = -1, fogColor = -1, tint = -1, params = -1;
    };
    using Vertex = CharacterGlVertex;
    // The vertex and index buffers of one M2 file, shared by every model id that was loaded from it (an NPC model is loaded per
    // display id: eight guards in eight skins are eight ids and one set of buffers).
    struct Buffers {
        GLuint vbo = 0, ibo = 0;
        std::size_t bytes = 0;
        std::size_t* total = nullptr;
        ~Buffers() {
            if (vbo) glDeleteBuffers(1, &vbo);
            if (ibo) glDeleteBuffers(1, &ibo);
            if (total) *total -= std::min(*total, bytes);
        }
    };
    struct Model {
        std::shared_ptr<Buffers> buf;
        uint32_t canonical = 0;               // the id whose M2Model (`models`) and buffers this id shares
        std::vector<GpuTexture*> textureIds;  // per texture slot; nullptr for the types that name no file (skin, hair, ...)
    };
    struct InstanceExtra {
        uint32_t texModel = 0;                // the id the shared code created the instance with: its textures are per id
        std::unordered_map<uint16_t, GpuTexture*> slotOverrides;
        std::unordered_map<uint16_t, GpuTexture*> groupOverrides;
    };

    pipeline::AssetManager* assets = nullptr;
    GLuint program[3] = {0, 0, 0};
    Uniforms u[3];
    gl::TextureCache textures;
    std::unordered_map<std::string, std::unique_ptr<GpuTexture>> textureObjects;  // by path; "" is the white one
    std::unordered_map<uint32_t, Model> glModels;
    std::unordered_map<uint32_t, InstanceExtra> extras;
    std::unordered_map<std::string, uint32_t> byFile;  // M2 identity -> the id that holds its data
    std::unordered_set<uint32_t> loggedFallback;
    bool ready = false;
    std::size_t gpuBytes = 0;
    int drawn = 0, shown = 0, visible = 0;
    // Animated characters (phase B): the closest few are skinned on the CPU each frame into a ring of two vertex buffers (one
    // being filled while the GPU still reads the other); everyone else keeps the shared Stand-pose buffer baked at load.
    int maxAnimated = 4;
    float animRange = 25.0f;  // yards
    // One vertex buffer per animated slot and frame parity, always drawn from offset 0 (a shared buffer with per-instance offsets
    // drew every instance but the first one distorted on the Vita).
    std::vector<GLuint> slotVbo;  // [parity * maxAnimated + slot]
    std::size_t slotVerts = 0;    // capacity of one slot buffer, in vertices
    int parity = 0;
    std::vector<Vertex> staging;
    std::vector<uint32_t> slotOwner;  // instance id holding each slot (0 = free)
    std::vector<int> slotParity;      // the parity buffer of each slot that was written last (-1 = never)
    uint32_t frameCounter = 0;
    long rangeCalls = 0;
    double skinMs = 0.0, boneMs = 0.0, uploadMs = 0.0, totalMs = 0.0;
    long skinVerts = 0, skinFrames = 0, animatedNow = 0;
    int maxDrawn = 24;       // the closest N characters are drawn, the rest are nameplates only
    float reach = 150.0f;    // yards
    GpuTexture white;

    GpuTexture* object(const std::string& key, GLuint id) {
        auto it = textureObjects.find(key);
        if (it != textureObjects.end()) {
            it->second->id = id;
            return it->second.get();
        }
        auto obj = std::make_unique<GpuTexture>();
        obj->id = id;
        GpuTexture* raw = obj.get();
        textureObjects.emplace(key, std::move(obj));
        return raw;
    }
};


// ---- skinning (CPU) ----

// Sequence index of the Stand animation (id 0, primary variation first), -1 when the model has none.
static int standSequence(const pipeline::M2Model& model) {
    int first = -1;
    for (std::size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id != 0) continue;
        if (first < 0) first = static_cast<int>(i);
        if (model.sequences[i].variationIndex == 0) return static_cast<int>(i);
    }
    return first;
}

// Blend the vertices [first, first + count) over their bones (up to four, by weight) into `out` (indexed by vertex). The bone
// matrices already hold the pivot bracket of an M2 bone, so a bind-pose position goes straight through them.
static void skinVertices(const pipeline::M2Model& model, const std::vector<glm::mat4>& bones, CharacterGlVertex* out,
                         uint32_t first, uint32_t count) {
    const std::size_t nb = bones.size();
    const uint32_t end = std::min<uint32_t>(first + count, static_cast<uint32_t>(model.vertices.size()));
#if defined(__ARM_NEON)
    // Everything stays in NEON registers: moving a float between the VFP and NEON units costs a Cortex-A9 about twenty cycles, and
    // the first version did it six times a bone weight (0.8 microseconds a vertex). The weights of an M2 vertex add to 255.
    static_assert(offsetof(pipeline::M2Vertex, position) == 0 && sizeof(glm::vec3) == 12, "M2Vertex layout");
    const float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f);
    for (uint32_t i = first; i < end; ++i) {
        const pipeline::M2Vertex& v = model.vertices[i];
        uint32_t wbits;
        std::memcpy(&wbits, v.boneWeights, 4);
        if (wbits == 0) {  // no bone: the bind position
            out[i].pos[0] = v.position.x; out[i].pos[1] = v.position.y; out[i].pos[2] = v.position.z;
            out[i].normal[0] = v.normal.x; out[i].normal[1] = v.normal.y; out[i].normal[2] = v.normal.z;
            out[i].uv[0] = v.texCoords[0].x; out[i].uv[1] = v.texCoords[0].y;
            continue;
        }
        const float32x4_t pos = vld1q_f32(&v.position.x);   // x y z and the first weight bytes (lane 3 is not used)
        const float32x4_t nrm = vld1q_f32(&v.normal.x);     // nx ny nz and the first texture coordinate (not used)
        const float32x2_t pxy = vget_low_f32(pos), pz = vget_high_f32(pos), nxy = vget_low_f32(nrm), nz = vget_high_f32(nrm);
        const float32x4_t wf = vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(wbits))))), inv255);
        const float32x2_t w01 = vget_low_f32(wf), w23 = vget_high_f32(wf);
        float32x4_t p = vdupq_n_f32(0.0f), n = vdupq_n_f32(0.0f);
        for (int k = 0; k < 4; ++k) {
            const uint32_t idx = v.boneIndices[k];
            if (v.boneWeights[k] == 0 || idx >= nb) continue;
            const float* m = &bones[idx][0][0];
            const float32x4_t c0 = vld1q_f32(m), c1 = vld1q_f32(m + 4), c2 = vld1q_f32(m + 8), c3 = vld1q_f32(m + 12);
            const float32x4_t pa = vmlaq_lane_f32(vmulq_lane_f32(c0, pxy, 0), c1, pxy, 1);
            const float32x4_t pb = vmlaq_lane_f32(c3, c2, pz, 0);
            const float32x4_t na = vmlaq_lane_f32(vmulq_lane_f32(c0, nxy, 0), c1, nxy, 1);
            const float32x4_t nb2 = vmulq_lane_f32(c2, nz, 0);
            const float32x4_t ps = vaddq_f32(pa, pb), ns = vaddq_f32(na, nb2);
            switch (k) {
                case 0: p = vmlaq_lane_f32(p, ps, w01, 0); n = vmlaq_lane_f32(n, ns, w01, 0); break;
                case 1: p = vmlaq_lane_f32(p, ps, w01, 1); n = vmlaq_lane_f32(n, ns, w01, 1); break;
                case 2: p = vmlaq_lane_f32(p, ps, w23, 0); n = vmlaq_lane_f32(n, ns, w23, 0); break;
                default: p = vmlaq_lane_f32(p, ps, w23, 1); n = vmlaq_lane_f32(n, ns, w23, 1); break;
            }
        }
        // Stores: position's fourth lane lands on normal[0] and is overwritten by the next store.
        vst1q_f32(&out[i].pos[0], p);
        vst1_f32(&out[i].normal[0], vget_low_f32(n));
        vst1_lane_f32(&out[i].normal[2], vget_high_f32(n), 0);
        vst1_f32(&out[i].uv[0], vld1_f32(&v.texCoords[0].x));
    }
#else
    for (uint32_t i = first; i < end; ++i) {
        const pipeline::M2Vertex& v = model.vertices[i];
        const int wsum = v.boneWeights[0] + v.boneWeights[1] + v.boneWeights[2] + v.boneWeights[3];
        out[i].uv[0] = v.texCoords[0].x;
        out[i].uv[1] = v.texCoords[0].y;
        if (wsum <= 0) {  // no bone: the bind position
            out[i].pos[0] = v.position.x; out[i].pos[1] = v.position.y; out[i].pos[2] = v.position.z;
            out[i].normal[0] = v.normal.x; out[i].normal[1] = v.normal.y; out[i].normal[2] = v.normal.z;
            continue;
        }
        const float norm = 1.0f / static_cast<float>(wsum);
        glm::vec3 p(0.0f), n(0.0f);
        for (int k = 0; k < 4; ++k) {
            if (v.boneWeights[k] == 0 || v.boneIndices[k] >= nb) continue;
            const float w = static_cast<float>(v.boneWeights[k]) * norm;
            const glm::mat4& m = bones[v.boneIndices[k]];
            p += w * glm::vec3(m[0] * v.position.x + m[1] * v.position.y + m[2] * v.position.z + m[3]);
            n += w * glm::vec3(m[0] * v.normal.x + m[1] * v.normal.y + m[2] * v.normal.z);
        }
        out[i].pos[0] = p.x; out[i].pos[1] = p.y; out[i].pos[2] = p.z;
        out[i].normal[0] = n.x; out[i].normal[1] = n.y; out[i].normal[2] = n.z;
    }
#endif
}

void CharacterRenderer::calculateBoneMatrices(CharacterInstance& instance) {
    auto mit = models.find(instance.modelId);
    if (mit == models.end()) return;
    const pipeline::M2Model& model = mit->second.data;
    const std::size_t n = model.bones.size();
    if (n == 0) return;
    instance.boneMatrices.resize(n);
    const auto& gsd = model.globalSequenceDurations;
    for (std::size_t i = 0; i < n; ++i) {
        const pipeline::M2Bone& bone = model.bones[i];
        glm::mat4 local = getBoneTransform(bone, instance.animationTime, instance.globalSequenceTime, instance.currentSequenceIndex, gsd);
        if (bone.keyBoneId == 4 && instance.torsoYawOverrideRad != 0.0f) {  // the lower spine
            local = glm::translate(glm::mat4(1.0f), bone.pivot) * glm::rotate(glm::mat4(1.0f), instance.torsoYawOverrideRad, glm::vec3(0.0f, 0.0f, 1.0f)) *
                    glm::translate(glm::mat4(1.0f), -bone.pivot) * local;
        }
        if (bone.parentBone >= 0 && static_cast<std::size_t>(bone.parentBone) < i) instance.boneMatrices[i] = instance.boneMatrices[bone.parentBone] * local;
        else instance.boneMatrices[i] = local;
    }
}

// M2 bone transform T(pivot) * T(trans) * R(rot) * S(scale) * T(-pivot), built directly (upstream's own).
glm::mat4 CharacterRenderer::getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime, int sequenceIndex,
                                              const std::vector<uint32_t>& globalSeqDurations) {
    const glm::vec3 translation = m2_track::sampleVec3(bone.translation, sequenceIndex, animTime, globalSeqTime, globalSeqDurations, glm::vec3(0.0f));
    const glm::quat rotation = m2_track::sampleQuat(bone.rotation, sequenceIndex, animTime, globalSeqTime, globalSeqDurations);
    const glm::vec3 scale = m2_track::sampleVec3(bone.scale, sequenceIndex, animTime, globalSeqTime, globalSeqDurations, glm::vec3(1.0f));
    const glm::mat3 R = glm::mat3_cast(rotation);
    const glm::vec3 c0 = R[0] * scale.x, c1 = R[1] * scale.y, c2 = R[2] * scale.z;
    const glm::vec3 t = (bone.pivot + translation) - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
    glm::mat4 m;
    m[0] = glm::vec4(c0, 0.0f);
    m[1] = glm::vec4(c1, 0.0f);
    m[2] = glm::vec4(c2, 0.0f);
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

CharacterRenderer::CharacterRenderer() = default;

CharacterRenderer::~CharacterRenderer() {
    if (gl_ && gl_->ready) shutdown();
}

bool CharacterRenderer::glReady() const { return gl_ && gl_->ready; }

bool CharacterRenderer::glInitialize(pipeline::AssetManager* assets) {
    if (glReady()) return true;
    gl_ = std::make_unique<Gl>();
    gl_->assets = assets;
    assetManager = assets;
    gl_->textures.setAssetManager(assets);
    {
        // Character skins are 256 on a side or less; a cap keeps the odd 512 or 1024 texture from taking the GPU pools.
        const char* v = std::getenv("WOWEE_TEXTURE_MAX");
        gl_->textures.setMaxDimension(v ? std::atoi(v) : 256);
        if (const char* n = std::getenv("WOWEE_CHAR_MAX")) gl_->maxDrawn = std::clamp(std::atoi(n), 0, 200);
        if (const char* a = std::getenv("WOWEE_CHAR_ANIM_MAX")) gl_->maxAnimated = std::clamp(std::atoi(a), 0, 32);
        if (const char* r = std::getenv("WOWEE_CHAR_ANIM_DIST")) gl_->animRange = std::clamp(static_cast<float>(std::atof(r)), 5.0f, 100.0f);
        if (const char* r = std::getenv("WOWEE_CHAR_RANGE")) gl_->reach = std::clamp(static_cast<float>(std::atof(r)), 20.0f, 1000.0f);
    }
    for (int k = 0; k < 3; ++k) {
        gl_->program[k] = gl::linkProgram(gl::m2Program(static_cast<gl::M2Kind>(k)));
        if (gl_->program[k] == 0) {
            LOG_ERROR("Characters (GL): program ", k, " did not link");
            return false;
        }
        Gl::Uniforms& u = gl_->u[k];
        const GLuint p = gl_->program[k];
        u.viewProj = glGetUniformLocation(p, "uViewProj");
        u.model = glGetUniformLocation(p, "uModel");
        u.uvOffset = glGetUniformLocation(p, "uUVOffset");
        u.lightDir = glGetUniformLocation(p, "uLightDir");
        u.lightColor = glGetUniformLocation(p, "uLightColor");
        u.ambient = glGetUniformLocation(p, "uAmbient");
        u.eye = glGetUniformLocation(p, "uEye");
        u.fog = glGetUniformLocation(p, "uFog");
        u.lit = glGetUniformLocation(p, "uLit");
        u.texture = glGetUniformLocation(p, "uTexture");
        u.fogColor = glGetUniformLocation(p, "uFogColor");
        u.tint = glGetUniformLocation(p, "uTint");
        u.params = glGetUniformLocation(p, "uParams");
    }
    gl_->white.id = gl_->textures.white();
    if (gl_->maxAnimated > 0) {
        gl_->maxAnimated = std::min(gl_->maxAnimated, 8);
        gl_->slotVerts = 8192;  // 256 KB a slot: the biggest humanoid has 6300 vertices
        gl_->staging.resize(gl_->slotVerts);
        gl_->slotVbo.resize(static_cast<std::size_t>(2 * gl_->maxAnimated));
        gl_->slotOwner.assign(static_cast<std::size_t>(gl_->maxAnimated), 0);
        gl_->slotParity.assign(static_cast<std::size_t>(gl_->maxAnimated), -1);
        glGenBuffers(static_cast<GLsizei>(gl_->slotVbo.size()), gl_->slotVbo.data());
        for (GLuint b : gl_->slotVbo) {
            glBindBuffer(GL_ARRAY_BUFFER, b);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(gl_->slotVerts * sizeof(Gl::Vertex)), nullptr, GL_DYNAMIC_DRAW);
        }
    }
    gl_->ready = true;
    LOG_WARNING("Character renderer (GL) ready: draws at most ", gl_->maxDrawn, " within ", gl_->reach, " yards");
    return true;
}

void CharacterRenderer::shutdown() {
    clear();
    if (gl_) {
        for (GLuint& p : gl_->program) {
            if (p) glDeleteProgram(p);
            p = 0;
        }
        gl_->textures.clear();
        if (!gl_->slotVbo.empty()) glDeleteBuffers(static_cast<GLsizei>(gl_->slotVbo.size()), gl_->slotVbo.data());
        gl_->slotVbo.clear();
        gl_->ready = false;
    }
}

void CharacterRenderer::clear() {
    if (!gl_) return;
    gl_->glModels.clear();
    gl_->byFile.clear();
    gl_->extras.clear();
    gl_->gpuBytes = 0;
    models.clear();
    instances.clear();
}

// ---- textures ----

GpuTexture* CharacterRenderer::loadTexture(const std::string& path) {
    if (!glReady()) return nullptr;
    if (path.empty()) return &gl_->white;
    auto it = gl_->textureObjects.find(path);
    if (it != gl_->textureObjects.end()) return it->second.get();
    if (predecodedBLPCache_) {
        auto pre = predecodedBLPCache_->find(path);
        if (pre != predecodedBLPCache_->end()) gl_->textures.adopt(path, pre->second);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const GLuint id = gl_->textures.get(path);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 25.0) LOG_WARNING("Character texture '", path, "' took ", ms, " ms", predecodedBLPCache_ ? " (predecode cache set)" : "");
    if (id == gl_->textures.white()) return &gl_->white;  // a missing file is the white texture, as upstream
    return gl_->object(path, id);
}

// Phase A has no compositing (VITA-20 phase C): the base layer alone, which is the bare skin of a humanoid.
GpuTexture* CharacterRenderer::compositeTextures(const std::vector<std::string>& layerPaths) {
    return layerPaths.empty() ? nullptr : loadTexture(layerPaths.front());
}

GpuTexture* CharacterRenderer::compositeWithRegions(const std::string& basePath, [[maybe_unused]] const std::vector<std::string>& baseLayers,
                                                    [[maybe_unused]] const std::vector<std::pair<int, std::string>>& regionLayers) {
    return loadTexture(basePath);
}

void CharacterRenderer::clearCompositeCache() { }

void CharacterRenderer::processPendingNormalMaps([[maybe_unused]] int budget) { }

void CharacterRenderer::setModelTexture(uint32_t modelId, uint32_t textureSlot, GpuTexture* texture) {
    if (!gl_) return;
    auto it = gl_->glModels.find(modelId);
    if (it == gl_->glModels.end() || textureSlot >= it->second.textureIds.size()) return;
    it->second.textureIds[textureSlot] = texture;
}

void CharacterRenderer::setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, GpuTexture* texture) {
    if (gl_ && instances.count(instanceId)) gl_->extras[instanceId].slotOverrides[textureSlot] = texture;
}

void CharacterRenderer::clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot) {
    if (!gl_) return;
    auto it = gl_->extras.find(instanceId);
    if (it != gl_->extras.end()) it->second.slotOverrides.erase(textureSlot);
}

void CharacterRenderer::setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, GpuTexture* texture) {
    if (gl_ && instances.count(instanceId)) gl_->extras[instanceId].groupOverrides[geosetGroup] = texture;
}

// ---- models ----

bool CharacterRenderer::loadModel(const pipeline::M2Model& model, uint32_t id) {
    if (!glReady() || !model.isValid() || model.vertices.empty() || model.indices.empty()) return false;
    if (gl_->glModels.count(id)) unloadModelIfUnused(id);
    if (gl_->glModels.count(id)) return true;  // still in use: keep what is there

    const auto t0 = std::chrono::steady_clock::now();
    auto msSince = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };

    // The same M2 file loaded under another id (another display of the same model) shares its data and buffers: only the
    // texture slots are per id. Identity: name and sizes (a path is not in the M2Model).
    const std::string key = model.name + "|" + std::to_string(model.vertices.size()) + "|" + std::to_string(model.indices.size()) + "|" +
                            std::to_string(model.bones.size()) + "|" + std::to_string(model.sequences.size());
    Gl::Model out;
    auto known = gl_->byFile.find(key);
    if (known != gl_->byFile.end()) {
        auto canon = gl_->glModels.find(known->second);
        if (canon != gl_->glModels.end() && models.count(known->second)) {
            out.buf = canon->second.buf;
            out.canonical = known->second;
        }
    }
    const bool shared = out.buf != nullptr;
    std::size_t maxBones = 0;
    if (!shared) {
        out.buf = std::make_shared<Gl::Buffers>();
        out.canonical = id;
        const auto t2 = std::chrono::steady_clock::now();
        M2ModelGPU gpu;
        gpu.data = model;
        gpu.indexCount = static_cast<uint32_t>(model.indices.size());
        gpu.vertexCount = static_cast<uint32_t>(model.vertices.size());
        gpu.visualBoundMin = glm::vec3(std::numeric_limits<float>::max());
        gpu.visualBoundMax = glm::vec3(-std::numeric_limits<float>::max());
        for (const pipeline::M2Vertex& v : model.vertices) {
            if (!std::isfinite(v.position.x + v.position.y + v.position.z)) continue;
            gpu.visualBoundMin = glm::min(gpu.visualBoundMin, v.position);
            gpu.visualBoundMax = glm::max(gpu.visualBoundMax, v.position);
        }
        if (gpu.visualBoundMin.x > gpu.visualBoundMax.x) gpu.visualBoundMin = gpu.visualBoundMax = glm::vec3(0.0f);
        gpu.visualBoundRadius = glm::length(gpu.visualBoundMax - gpu.visualBoundMin) * 0.5f;
        gpu.sortedBatchIndices.resize(gpu.data.batches.size());
        std::iota(gpu.sortedBatchIndices.begin(), gpu.sortedBatchIndices.end(), size_t{0});
        std::stable_sort(gpu.sortedBatchIndices.begin(), gpu.sortedBatchIndices.end(), [&b = gpu.data.batches](size_t x, size_t y) {
            if (b[x].priorityPlane != b[y].priorityPlane) return b[x].priorityPlane < b[y].priorityPlane;
            return b[x].materialLayer < b[y].materialLayer;
        });
        // The number the skinning decision needs: the most bones any one batch uses (vertex bone indices are bone indices).
        for (const pipeline::M2Batch& b : model.batches) {
            if (static_cast<std::size_t>(b.indexStart) + b.indexCount > model.indices.size()) continue;
            std::unordered_set<uint8_t> used;
            for (uint32_t i = b.indexStart; i < b.indexStart + b.indexCount; ++i) {
                const uint16_t vi = model.indices[i];
                if (vi >= model.vertices.size()) continue;
                const pipeline::M2Vertex& v = model.vertices[vi];
                for (int k = 0; k < 4; ++k) {
                    if (v.boneWeights[k] != 0) used.insert(v.boneIndices[k]);
                }
            }
            maxBones = std::max(maxBones, used.size());
        }
        models[id] = std::move(gpu);

        // The shared buffer holds the model in its Stand pose (frame 0 of animation 0), skinned once here: bind pose is arms out.
        std::vector<Gl::Vertex> verts(model.vertices.size());
        for (std::size_t i = 0; i < verts.size(); ++i) {
            const pipeline::M2Vertex& v = model.vertices[i];
            verts[i] = Gl::Vertex{{v.position.x, v.position.y, v.position.z}, {v.normal.x, v.normal.y, v.normal.z},
                                  {v.texCoords[0].x, v.texCoords[0].y}};
        }
        bool baked = false;
        {
            CharacterInstance tmp;
            tmp.modelId = id;
            tmp.currentSequenceIndex = standSequence(model);
            if (tmp.currentSequenceIndex >= 0 && !model.bones.empty()) {
                calculateBoneMatrices(tmp);
                skinVertices(model, tmp.boneMatrices, verts.data(), 0, static_cast<uint32_t>(verts.size()));
                baked = true;
            }
        }
        glGenBuffers(1, &out.buf->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, out.buf->vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Gl::Vertex)), verts.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &out.buf->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.buf->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(model.indices.size() * sizeof(uint16_t)), model.indices.data(),
                     GL_STATIC_DRAW);
        out.buf->bytes = verts.size() * sizeof(Gl::Vertex) + model.indices.size() * sizeof(uint16_t);
        out.buf->total = &gl_->gpuBytes;
        gl_->gpuBytes += out.buf->bytes;
        gl_->byFile[key] = id;
        LOG_WARNING("Character model ", id, " '", model.name, "': ", model.vertices.size(), " verts, ", model.indices.size() / 3, " tris, ",
                    model.batches.size(), " batches, ", model.bones.size(), " bones (at most ", maxBones, " in one batch), ",
                    model.sequences.size(), " sequences, ", model.textures.size(), " textures; stand pose ", baked ? "baked" : "missing",
                    ", prepare ", msSince(t2), " ms");
    }
    const double uploadMs = msSince(t0);

    const auto t1 = std::chrono::steady_clock::now();
    for (const pipeline::M2Texture& tex : model.textures) {
        out.textureIds.push_back(tex.type == 0 ? loadTexture(tex.filename) : nullptr);
    }
    const double textureMs = msSince(t1);

    gl_->glModels.emplace(id, std::move(out));
    const double total = msSince(t0);
    if (total > 30.0 || shared) {
        LOG_WARNING("Character loadModel ", id, " '", model.name, "'", shared ? " (shares the buffers of an earlier load)" : "", ": ", total,
                    " ms (buffers ", uploadMs, ", textures ", textureMs, "), models ", models.size(), " ids ", gl_->glModels.size());
    }
    return true;
}

void CharacterRenderer::unloadModelIfUnused(uint32_t modelId) {
    if (!gl_) return;
    for (const auto& e : gl_->extras) {
        if (e.second.texModel == modelId) return;
    }
    auto it = gl_->glModels.find(modelId);
    if (it == gl_->glModels.end()) return;
    const uint32_t canonical = it->second.canonical;
    gl_->glModels.erase(it);
    // The shared data goes with the last id that points at it.
    for (const auto& m : gl_->glModels) {
        if (m.second.canonical == canonical) return;
    }
    models.erase(canonical);
    for (auto b = gl_->byFile.begin(); b != gl_->byFile.end();) b = b->second == canonical ? gl_->byFile.erase(b) : std::next(b);
}

const pipeline::M2Model* CharacterRenderer::getModelData(uint32_t modelId) const {
    if (!gl_) return nullptr;
    auto g = gl_->glModels.find(modelId);
    auto it = models.find(g == gl_->glModels.end() ? modelId : g->second.canonical);
    return it == models.end() ? nullptr : &it->second.data;
}

const pipeline::M2Model* CharacterRenderer::getInstanceModelData(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    return it == instances.end() ? nullptr : getModelData(it->second.modelId);
}

// ---- instances ----

uint32_t CharacterRenderer::createInstance(uint32_t modelId, const glm::vec3& position, const glm::vec3& rotation, float scale) {
    if (!glReady()) return 0;
    auto g = gl_->glModels.find(modelId);
    if (g == gl_->glModels.end() || !models.count(g->second.canonical)) return 0;
    CharacterInstance inst;
    inst.id = nextInstanceId++;
    inst.modelId = g->second.canonical;  // the data is shared; the textures stay per id (InstanceExtra::texModel)
    inst.position = position;
    inst.rotation = rotation;
    inst.scale = scale;
    const uint32_t id = inst.id;
    instances[id] = std::move(inst);
    gl_->extras[id].texModel = modelId;
    return id;
}

void CharacterRenderer::removeInstance(uint32_t instanceId) {
    instances.erase(instanceId);
    if (gl_) gl_->extras.erase(instanceId);
}

void CharacterRenderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    CharacterInstance& i = it->second;
    i.position = i.moveStart = i.moveEnd = position;
    i.moveElapsed = i.moveDuration = 0.0f;
    i.isMoving = false;
}

void CharacterRenderer::setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.rotation = rotation;
}

void CharacterRenderer::setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.torsoYawOverrideRad = deltaYawRad;
}

void CharacterRenderer::setInstanceVisible(uint32_t instanceId, bool visible) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.visible = visible;
}

void CharacterRenderer::setInstanceSceneModel(uint32_t instanceId, bool isScene) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.isSceneModel = isScene;
}

void CharacterRenderer::setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.activeGeosets = geosets;
}

void CharacterRenderer::setDrawSkinExtra(uint32_t instanceId, bool enabled) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.drawSkinExtra = enabled;
}

void CharacterRenderer::startFadeIn(uint32_t instanceId, float durationSeconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.opacity = 0.0f;
    it->second.fadeInTime = 0.0f;
    it->second.fadeInDuration = durationSeconds;
}

void CharacterRenderer::setInstanceOpacity(uint32_t instanceId, float opacity) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.opacity = std::clamp(opacity, 0.0f, 1.0f);
    it->second.fadeInDuration = 0.0f;
}

glm::mat4 CharacterRenderer::getModelMatrix(const CharacterInstance& instance) const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), instance.position);
    m = glm::rotate(m, instance.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));  // yaw
    m = glm::rotate(m, instance.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));  // pitch
    m = glm::rotate(m, instance.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));  // roll
    return glm::scale(m, glm::vec3(instance.scale));
}

void CharacterRenderer::moveInstanceTo(uint32_t instanceId, const glm::vec3& destination, float durationSeconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    CharacterInstance& inst = it->second;
    if (inst.isDead) return;  // corpses do not slide

    const float dx = destination.x - inst.position.x, dy = destination.y - inst.position.y;
    const float planarSq = dx * dx + dy * dy;
    bool synthesized = false;
    if (durationSeconds <= 0.0f) {
        if (planarSq < 1e-4f) {
            inst.position = destination;
            inst.isMoving = false;
            if (inst.currentAnimationId == anim::WALK || inst.currentAnimationId == anim::RUN) playAnimation(instanceId, anim::STAND, true);
            return;
        }
        // Some cores send movement deltas without a spline duration: a short one keeps the facing and the move animation going.
        durationSeconds = std::clamp(std::sqrt(planarSq) / 7.0f, 0.05f, 0.20f);
        synthesized = true;
    }
    inst.moveStart = inst.position;
    inst.moveEnd = destination;
    inst.moveDuration = durationSeconds;
    inst.moveElapsed = 0.0f;
    inst.isMoving = true;
    if (planarSq > 1e-6f) inst.rotation.z = std::atan2(dy, dx);

    const float speed = std::sqrt(planarSq) / std::max(durationSeconds, 0.001f);
    const bool preferRun = !synthesized && speed >= 4.5f;
    uint32_t moveAnim = 0;
    if (preferRun) moveAnim = hasAnimation(instanceId, anim::RUN) ? anim::RUN : (hasAnimation(instanceId, anim::WALK) ? anim::WALK : 0);
    else moveAnim = hasAnimation(instanceId, anim::WALK) ? anim::WALK : (hasAnimation(instanceId, anim::RUN) ? anim::RUN : 0);
    if (moveAnim != 0 && inst.currentAnimationId != moveAnim) playAnimation(instanceId, moveAnim, true);
}

// ---- animation state (no bones yet: the clock runs so the shared code sees animations start and end) ----

void CharacterRenderer::playAnimation(uint32_t instanceId, uint32_t animationId, bool loop, uint32_t oneShotReturnAnim) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    CharacterInstance& inst = it->second;
    auto mit = models.find(inst.modelId);
    if (mit == models.end()) return;
    const auto& sequences = mit->second.data.sequences;

    inst.oneShotReturnAnim = loop ? 0 : oneShotReturnAnim;
    if (animationId == anim::DEATH) inst.isDead = true;
    else if (inst.isDead && animationId == anim::STAND) inst.isDead = false;

    inst.currentAnimationId = animationId;
    inst.currentSequenceIndex = -1;
    inst.armSequenceIndex[0] = inst.armSequenceIndex[1] = -1;
    inst.animationTime = 0.0f;
    inst.animationLoop = loop;

    int firstMatch = -1;
    for (std::size_t i = 0; i < sequences.size(); ++i) {
        if (sequences[i].id != animationId) continue;
        if (firstMatch < 0) firstMatch = static_cast<int>(i);
        if (sequences[i].variationIndex == 0) {
            inst.currentSequenceIndex = static_cast<int>(i);
            break;
        }
    }
    if (inst.currentSequenceIndex < 0) inst.currentSequenceIndex = firstMatch;
    if (inst.currentSequenceIndex < 0 && !sequences.empty()) {
        inst.currentSequenceIndex = 0;
        inst.currentAnimationId = sequences[0].id;
    }
}

void CharacterRenderer::setArmAnimations([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t leftArmAnim,
                                         [[maybe_unused]] uint32_t rightArmAnim) { }

bool CharacterRenderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    for (const auto& seq : mit->second.data.sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

bool CharacterRenderer::getAnimationState(uint32_t instanceId, uint32_t& animationId, float& animationTimeMs, float& animationDurationMs) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    const auto& sequences = mit->second.data.sequences;
    const CharacterInstance& inst = it->second;
    if (inst.currentSequenceIndex < 0 || inst.currentSequenceIndex >= static_cast<int>(sequences.size())) return false;
    animationId = inst.currentAnimationId;
    animationTimeMs = inst.animationTime;
    animationDurationMs = static_cast<float>(sequences[inst.currentSequenceIndex].duration);
    return true;
}

bool CharacterRenderer::getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const {
    out.clear();
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    out = mit->second.data.sequences;
    return !out.empty();
}

const std::vector<uint32_t>* CharacterRenderer::getFootstepEventTimes(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return nullptr;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return nullptr;
    const auto& times = mit->second.data.footstepEventTimes;
    const int seq = it->second.currentSequenceIndex;
    if (seq < 0 || seq >= static_cast<int>(times.size()) || times[seq].empty()) return nullptr;
    return &times[seq];
}

bool CharacterRenderer::getInstanceModelName(uint32_t instanceId, std::string& modelName) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    modelName = mit->second.data.name;
    return true;
}

void CharacterRenderer::update(float deltaTime, const glm::vec3& cameraPos) {
    // Animations tick within this many yards of the camera; the rest hold their pose (a cost that scales with what is near).
    static const float kAnimRadiusSq = [] {
        const char* v = std::getenv("WOWEE_CHAR_ANIM_RADIUS");
        const float r = v ? static_cast<float>(std::atof(v)) : 120.0f;
        return r * r;
    }();
    for (auto& pair : instances) {
        CharacterInstance& inst = pair.second;
        if (inst.fadeInDuration > 0.0f && inst.opacity < 1.0f) {
            inst.fadeInTime += deltaTime;
            inst.opacity = std::min(1.0f, inst.fadeInTime / inst.fadeInDuration);
            if (inst.opacity >= 1.0f) inst.fadeInDuration = 0.0f;
        }
        if (inst.isMoving) {
            inst.moveElapsed += deltaTime;
            const float t = inst.moveElapsed / inst.moveDuration;
            if (t >= 1.0f) {
                inst.position = inst.moveEnd;
                inst.isMoving = false;
            } else {
                inst.position = glm::mix(inst.moveStart, inst.moveEnd, t);
            }
        }
        if (inst.hasOverrideModelMatrix) continue;
        const glm::vec3 toCam = inst.position - cameraPos;
        if (glm::dot(toCam, toCam) > kAnimRadiusSq) continue;
        auto mit = models.find(inst.modelId);
        if (mit == models.end() || mit->second.data.sequences.empty()) continue;
        const auto& sequences = mit->second.data.sequences;
        if (inst.currentSequenceIndex < 0) {
            inst.currentSequenceIndex = 0;
            inst.currentAnimationId = sequences[0].id;
        }
        const auto& seq = sequences[inst.currentSequenceIndex];
        inst.globalSequenceTime += deltaTime * 1000.0f;
        inst.animationTime += deltaTime * 1000.0f;
        if (seq.duration > 0 && inst.animationTime >= static_cast<float>(seq.duration)) {
            if (inst.animationLoop) {
                inst.animationTime = std::fmod(inst.animationTime, static_cast<float>(seq.duration));
            } else if (inst.currentAnimationId != anim::DEATH) {
                playAnimation(pair.first, inst.oneShotReturnAnim != 0 ? inst.oneShotReturnAnim : anim::STAND, true);
            } else {
                inst.animationTime = static_cast<float>(seq.duration);
            }
        }
    }
}

// ---- geometry queries ----

bool CharacterRenderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    const M2ModelGPU& gm = mit->second;
    glm::vec3 bmin = gm.visualBoundMin, bmax = gm.visualBoundMax;
    float radius = gm.visualBoundRadius;
    if (radius <= 0.001f) {
        bmin = gm.data.boundMin;
        bmax = gm.data.boundMax;
        radius = gm.data.boundRadius > 0.001f ? gm.data.boundRadius : glm::length(bmax - bmin) * 0.5f;
    }
    const CharacterInstance& inst = it->second;
    const float scale = std::max(0.001f, inst.scale);
    glm::mat4 rot(1.0f);
    rot = glm::rotate(rot, inst.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    rot = glm::rotate(rot, inst.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    rot = glm::rotate(rot, inst.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    outCenter = inst.position + glm::vec3(rot * glm::vec4((bmin + bmax) * 0.5f * scale, 0.0f));
    outRadius = std::max(0.5f, radius * scale);
    return true;
}

bool CharacterRenderer::getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId, float& outZ) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    for (const auto& bone : mit->second.data.bones) {
        if (bone.keyBoneId != keyBoneId) continue;
        outZ = bone.pivot.z * std::max(0.001f, it->second.scale);
        return true;
    }
    return false;
}

bool CharacterRenderer::getInstanceHeight(uint32_t instanceId, float& outHeight) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    float top = mit->second.visualBoundMax.z;
    if (top <= 0.001f) top = mit->second.data.boundMax.z;
    if (top <= 0.001f) return false;
    outHeight = top * std::max(0.001f, it->second.scale);
    return true;
}

bool CharacterRenderer::getInstanceFootZ(uint32_t instanceId, float& outFootZ) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mit = models.find(it->second.modelId);
    if (mit == models.end()) return false;
    outFootZ = it->second.position.z + mit->second.data.boundMin.z * std::max(0.001f, it->second.scale);
    return true;
}

bool CharacterRenderer::getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    outPos = it->second.position;
    return true;
}

// ---- drawing ----

void CharacterRenderer::glRender(const gl::SceneParams& scene) {
    if (!glReady() || instances.empty() || gl_->maxDrawn == 0) return;
    static gl::FrameStats stats("Characters");
    stats.begin();
    Frustum frustum;
    frustum.extractFromMatrix(scene.cullViewProj);

    struct Item {
        uint32_t id;
        const CharacterInstance* inst;
        const M2ModelGPU* gm;
        const Gl::Model* mesh;
        float dist;
    };
    std::vector<Item> items;
    const float reach = std::min(gl_->reach, scene.fogEnd);
    int visible = 0;
    for (const auto& kv : instances) {
        const CharacterInstance& inst = kv.second;
        if (!inst.visible || inst.opacity <= 0.0f || inst.hasOverrideModelMatrix) continue;
        auto mit = models.find(inst.modelId);
        auto ex0 = gl_->extras.find(kv.first);
        auto git = ex0 == gl_->extras.end() ? gl_->glModels.end() : gl_->glModels.find(ex0->second.texModel);
        if (mit == models.end() || git == gl_->glModels.end()) continue;
        const float scale = std::max(0.001f, inst.scale);
        const glm::vec3 center = inst.position + (mit->second.visualBoundMin + mit->second.visualBoundMax) * 0.5f * scale;
        const float radius = std::max(0.5f, mit->second.visualBoundRadius * scale);
        const float dist = glm::length(center - scene.eye);
        if (dist - radius > reach || !frustum.intersectsSphere(center, radius)) continue;
        ++visible;
        items.push_back({kv.first, &inst, &mit->second, &git->second, dist});
    }
    gl_->visible = visible;
    if (items.empty()) {
        stats.end(0, 0);
        return;
    }
    if (static_cast<int>(items.size()) > gl_->maxDrawn) {
        std::partial_sort(items.begin(), items.begin() + gl_->maxDrawn, items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
        items.resize(static_cast<std::size_t>(gl_->maxDrawn));
    }
    // Which batches an instance draws (the geoset filter, as in the draw loop below): only their vertices are worth skinning.
    auto geosetsFilter = [](const CharacterInstance& inst, const pipeline::M2Model& data) {
        if (inst.activeGeosets.empty()) return false;
        for (const auto& b : data.batches) {
            if (inst.activeGeosets.count(b.submeshId)) return true;
        }
        return false;  // matches nothing: upstream draws everything
    };
    auto batchDrawn = [](const CharacterInstance& inst, bool filter, const pipeline::M2Batch& b) {
        if (filter) return inst.activeGeosets.count(b.submeshId) != 0;
        const uint16_t grp = b.submeshId / 100;
        return !(grp == 17 || grp == 18 || grp == 15);
    };

    // Animation: the closest few characters are skinned on the CPU, only the vertices their drawn batches use, into a vertex buffer
    // of their own (a slot; two buffers per slot so the one the GPU still reads is not the one written). Those beyond 6 yards are
    // re-skinned every other frame and draw the buffer written last in between.
    std::unordered_map<uint32_t, std::pair<int, int>> animSlot;  // instance id -> slot, parity to draw
    if (gl_->maxAnimated > 0 && !gl_->slotVbo.empty() && !gl::g_charNoAnim) {
        const auto s0 = std::chrono::steady_clock::now();
        ++gl_->frameCounter;
        std::vector<const Item*> selected;
        {
            std::vector<const Item*> byDistance;
            byDistance.reserve(items.size());
            for (const Item& it : items) byDistance.push_back(&it);
            std::sort(byDistance.begin(), byDistance.end(), [](const Item* a, const Item* b) { return a->dist < b->dist; });
            for (const Item* it : byDistance) {
                if (static_cast<int>(selected.size()) >= gl_->maxAnimated || (!gl::g_charAnimAll && it->dist > gl_->animRange)) break;
                const pipeline::M2Model& data = it->gm->data;
                if (data.bones.empty() || data.sequences.empty() || data.vertices.size() > gl_->slotVerts) continue;
                if (instances[it->id].currentSequenceIndex < 0) continue;
                selected.push_back(it);
            }
        }
        // Slots follow their character: free the ones whose owner is no longer among the closest, then give new ones a free slot.
        for (std::size_t sl = 0; sl < gl_->slotOwner.size(); ++sl) {
            if (gl_->slotOwner[sl] == 0) continue;
            bool keep = false;
            for (const Item* it : selected) keep = keep || it->id == gl_->slotOwner[sl];
            if (!keep) { gl_->slotOwner[sl] = 0; gl_->slotParity[sl] = -1; }
        }
        long verts = 0;
        for (const Item* it : selected) {
            int slot = -1;
            for (std::size_t sl = 0; sl < gl_->slotOwner.size(); ++sl) if (gl_->slotOwner[sl] == it->id) slot = static_cast<int>(sl);
            if (slot < 0) {
                for (std::size_t sl = 0; sl < gl_->slotOwner.size() && slot < 0; ++sl) if (gl_->slotOwner[sl] == 0) slot = static_cast<int>(sl);
                if (slot < 0) continue;
                gl_->slotOwner[static_cast<std::size_t>(slot)] = it->id;
                gl_->slotParity[static_cast<std::size_t>(slot)] = -1;
            }
            int& lastParity = gl_->slotParity[static_cast<std::size_t>(slot)];
            const uint32_t every = it->dist < 6.0f ? 1u : (it->dist < 15.0f ? 2u : 3u);  // frames between two skinnings
            const bool due = lastParity < 0 || ((gl_->frameCounter + it->id) % every) == 0;
            if (due) {
                const pipeline::M2Model& data = it->gm->data;
                CharacterInstance& inst = instances[it->id];
                const auto tb = std::chrono::steady_clock::now();
                calculateBoneMatrices(inst);
                const auto tk = std::chrono::steady_clock::now();
                gl_->boneMs += std::chrono::duration<double, std::milli>(tk - tb).count();
                const uint32_t count = static_cast<uint32_t>(data.vertices.size());
                const bool filter = geosetsFilter(inst, data);
                std::vector<std::pair<uint32_t, uint32_t>> ranges;  // [start, end) of the vertices the drawn batches use, merged
                for (const auto& b : data.batches) {
                    if (!batchDrawn(inst, filter, b)) continue;
                    const uint32_t start = b.vertexStart, end = std::min<uint32_t>(count, static_cast<uint32_t>(b.vertexStart) + b.vertexCount);
                    if (start < end) ranges.emplace_back(start, end);
                }
                std::sort(ranges.begin(), ranges.end());
                const int parity = lastParity < 0 ? 0 : lastParity ^ 1;
                glBindBuffer(GL_ARRAY_BUFFER, gl_->slotVbo[static_cast<std::size_t>(parity * gl_->maxAnimated + slot)]);
                // Ranges closer than 48 vertices are one: a skinned vertex costs less than a call that uploads a few.
                std::vector<std::pair<uint32_t, uint32_t>> merged;
                for (const auto& rg : ranges) {
                    if (!merged.empty() && rg.first <= merged.back().second + 48) merged.back().second = std::max(merged.back().second, rg.second);
                    else merged.push_back(rg);
                }
                const auto t1 = std::chrono::steady_clock::now();
                for (const auto& rg : merged) skinVertices(data, inst.boneMatrices, gl_->staging.data(), rg.first, rg.second - rg.first);
                const auto t2 = std::chrono::steady_clock::now();
                for (const auto& rg : merged) {
                    glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(rg.first * sizeof(CharacterGlVertex)),
                                    static_cast<GLsizeiptr>((rg.second - rg.first) * sizeof(CharacterGlVertex)), gl_->staging.data() + rg.first);
                    verts += rg.second - rg.first;
                }
                gl_->skinMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
                gl_->uploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
                gl_->rangeCalls += static_cast<long>(merged.size());
                lastParity = parity;
            }
            animSlot[it->id] = {slot, lastParity};
        }
        gl_->totalMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
        gl_->skinVerts += verts;
        gl_->animatedNow += static_cast<long>(animSlot.size());
        ++gl_->skinFrames;
    }
    // Draw in model order: fewer buffer binds (a street of guards is one model).
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.mesh->buf < b.mesh->buf; });

    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    int current = -1;
    bool frameSet[3] = {false, false, false};
    const glm::mat4 viewProj = scene.projection * scene.view;
    auto useProgram = [&](int k) {
        if (current != k) {
            current = k;
            glUseProgram(gl_->program[k]);
        }
        if (frameSet[k]) return;
        frameSet[k] = true;
        const Gl::Uniforms& u = gl_->u[k];
        glUniformMatrix4fv(u.viewProj, 1, GL_FALSE, &viewProj[0][0]);
        glUniform4f(u.lightDir, scene.lightDir.x, scene.lightDir.y, scene.lightDir.z, 0.0f);
        glUniform3f(u.lightColor, scene.lightColor.x, scene.lightColor.y, scene.lightColor.z);
        glUniform3f(u.ambient, scene.ambient.x, scene.ambient.y, scene.ambient.z);
        glUniform4f(u.eye, scene.eye.x, scene.eye.y, scene.eye.z, 0.0f);
        glUniform4f(u.fog, scene.fogStart, scene.fogEnd, 0.0f, 0.0f);
        glUniform3f(u.fogColor, scene.fogColor.x, scene.fogColor.y, scene.fogColor.z);
        glUniform2f(u.uvOffset, 0.0f, 0.0f);
        glUniform1i(u.texture, 0);
        glUniform3f(u.tint, 1.0f, 1.0f, 1.0f);
        glActiveTexture(GL_TEXTURE0);
    };

    GpuTexture* const white = &gl_->white;
    // Which texture a batch draws with: upstream's resolveBatchTexture (a batch lists up to 8 textures, one is bound).
    auto resolveTexture = [&](const Item& it, const Gl::InstanceExtra* extra, const pipeline::M2Batch& b) -> GpuTexture* {
        const auto& data = it.gm->data;
        if (b.textureIndex == 0xFFFF || data.textureLookup.empty() || it.mesh->textureIds.empty()) return white;
        const uint32_t combo = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
        GpuTexture* first = white;
        bool hasFirst = false;
        GpuTexture* firstNonWhite = nullptr;
        for (uint32_t i = 0; i < combo; ++i) {
            const uint32_t pos = static_cast<uint32_t>(b.textureIndex) + i;
            if (pos >= data.textureLookup.size()) break;
            const uint16_t slot = data.textureLookup[pos];
            if (slot >= it.mesh->textureIds.size()) continue;
            GpuTexture* tex = it.mesh->textureIds[slot];
            const uint32_t type = slot < data.textures.size() ? data.textures[slot].type : 0;
            if (extra) {
                auto o = extra->slotOverrides.find(slot);
                if (o != extra->slotOverrides.end() && o->second) {
                    if (type == 1) {  // a skin override only applies to the skin groups, so it does not bleed onto cloak or hair
                        const uint16_t grp = b.submeshId / 100;
                        if (grp == 0 || grp == 3 || grp == 4 || grp == 5 || grp == 8 || grp == 9 || grp == 13 || grp == 15 || grp == 20) tex = o->second;
                    } else {
                        tex = o->second;
                    }
                }
            }
            if (!hasFirst) {
                first = tex;
                hasFirst = true;
            }
            if (!tex || tex == white) continue;
            if (type == 6) return tex;  // the hair texture wins whenever it is in the combo
            if (!firstNonWhite) firstNonWhite = tex;
        }
        if (firstNonWhite) return firstNonWhite;
        return hasFirst && first ? first : white;
    };
    auto usesTextureType = [](const M2ModelGPU& gm, const pipeline::M2Batch& b, uint32_t wanted) {
        if (b.textureIndex == 0xFFFF || gm.data.textureLookup.empty()) return false;
        const uint32_t combo = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
        for (uint32_t i = 0; i < combo; ++i) {
            const uint32_t pos = static_cast<uint32_t>(b.textureIndex) + i;
            if (pos >= gm.data.textureLookup.size()) break;
            const uint16_t slot = gm.data.textureLookup[pos];
            if (slot < gm.data.textures.size() && gm.data.textures[slot].type == wanted) return true;
        }
        return false;
    };

    for (int pass = 0; pass < 2; ++pass) {  // 0: opaque and alpha-tested, 1: blended
        if (pass == 0) { glDepthMask(GL_TRUE); glDisable(GL_BLEND); }
        else { glDepthMask(GL_FALSE); glEnable(GL_BLEND); }
        const Gl::Buffers* bound = nullptr;
        GLuint boundVbo = 0;
        std::size_t boundBase = ~std::size_t{0};
        GLuint boundTexture = 0;
        for (const Item& it : items) {
            const CharacterInstance& inst = *it.inst;
            const auto& data = it.gm->data;
            auto ex = gl_->extras.find(it.id);
            const Gl::InstanceExtra* extra = ex == gl_->extras.end() ? nullptr : &ex->second;

            bool filter = !inst.activeGeosets.empty();
            if (filter) {
                bool any = false;
                for (const auto& b : data.batches) {
                    if (inst.activeGeosets.count(b.submeshId)) { any = true; break; }
                }
                if (!any) {  // upstream draws everything rather than nothing
                    if (gl_->loggedFallback.insert(it.id).second) LOG_WARNING("Characters: geoset filter matched nothing for instance ", it.id, ", drawing all batches");
                    filter = false;
                }
            }
            auto ro = animSlot.find(it.id);
            const GLuint vbo = ro == animSlot.end() ? it.mesh->buf->vbo
                                                    : gl_->slotVbo[static_cast<std::size_t>(ro->second.second * gl_->maxAnimated + ro->second.first)];
            const std::size_t base = 0;
            const glm::mat4 matrix = getModelMatrix(inst);
            bool matrixSet[3] = {false, false, false};

            for (std::size_t bi : it.gm->sortedBatchIndices) {
                const pipeline::M2Batch& b = data.batches[bi];
                if (b.indexCount == 0 || static_cast<std::size_t>(b.indexStart) + b.indexCount > data.indices.size()) continue;
                uint16_t blendMode = 0, flags = 0;
                if (b.materialIndex < data.materials.size()) {
                    blendMode = data.materials[b.materialIndex].blendMode;
                    flags = data.materials[b.materialIndex].flags;
                }
                if ((blendMode >= 2) != (pass == 1)) continue;
                if (filter) {
                    if (!inst.activeGeosets.count(b.submeshId)) continue;
                } else {
                    const uint16_t grp = b.submeshId / 100;  // eye glow and the cape group are off unless asked for
                    if (grp == 17 || grp == 18 || grp == 15) continue;
                }
                GpuTexture* tex = resolveTexture(it, extra, b);
                const uint16_t group = b.submeshId / 100;
                if (extra) {
                    auto g = extra->groupOverrides.find(group);
                    if (g != extra->groupOverrides.end() && g->second) tex = g->second;
                }
                const bool hairGeoset = (group >= 1 && group <= 3) || (group == 0 && b.submeshId > 0 && b.submeshId <= 99);
                const bool hairMaterial = !inst.isSceneModel && (usesTextureType(*it.gm, b, 6) || (hairGeoset && (blendMode != 0 || b.textureCount > 1)));
                gl::M2Kind kind = blendMode == 0 ? gl::M2Kind::Opaque : (blendMode == 1 ? gl::M2Kind::AlphaTest : gl::M2Kind::Blend);
                if (hairMaterial && kind == gl::M2Kind::Opaque) kind = gl::M2Kind::AlphaTest;
                if (pass == 1) kind = gl::M2Kind::Blend;
                else if (kind == gl::M2Kind::Blend) kind = gl::M2Kind::AlphaTest;

                if (bound != it.mesh->buf.get() || boundVbo != vbo || boundBase != base) {
                    bound = it.mesh->buf.get();
                    boundVbo = vbo;
                    boundBase = base;
                    glBindBuffer(GL_ARRAY_BUFFER, vbo);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it.mesh->buf->ibo);
                    glEnableVertexAttribArray(0);
                    glEnableVertexAttribArray(1);
                    glEnableVertexAttribArray(2);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(base + offsetof(Gl::Vertex, pos)));
                    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(base + offsetof(Gl::Vertex, normal)));
                    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(base + offsetof(Gl::Vertex, uv)));
                }
                const int k = static_cast<int>(kind);
                useProgram(k);
                const Gl::Uniforms& u = gl_->u[k];
                if (pass == 1) {
                    switch (blendMode) {
                        case 3: case 6: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
                        case 4: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
                        case 5: glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;
                        default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
                    }
                }
                if (!matrixSet[k]) {
                    matrixSet[k] = true;
                    glUniformMatrix4fv(u.model, 1, GL_FALSE, &matrix[0][0]);
                }
                const bool unlit = (flags & 0x01) != 0 || blendMode >= 3;
                glUniform2f(u.lit, unlit ? 0.0f : 1.0f, 1.0f);
                const float fogToColour = (blendMode == 3 || blendMode == 4 || blendMode == 6) ? 0.0f : 1.0f;
                glUniform4f(u.params, 0.5f, 0.0f, inst.opacity, fogToColour);
                const GLuint texId = tex ? tex->id : white->id;
                if (boundTexture != texId) {
                    boundTexture = texId;
                    glBindTexture(GL_TEXTURE_2D, texId);
                }
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(b.indexCount), GL_UNSIGNED_SHORT,
                               reinterpret_cast<const void*>(static_cast<uintptr_t>(b.indexStart) * sizeof(uint16_t)));
                ++gl_->drawn;
            }
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    stats.end(gl_->drawn, static_cast<long>(items.size()));
    gl_->drawn = 0;
    if (stats.frames == 0) {
        LOG_WARNING("GL memory Characters: textures ", gl_->textures.bytes() / 1024, " KB in ", gl_->textures.count(), ", buffers ",
                    gl_->gpuBytes / 1024, " KB, models ", gl_->glModels.size(), ", instances ", instances.size(), " (", gl_->visible,
                    " in range, ", items.size(), " drawn); animated ", gl_->skinFrames ? static_cast<double>(gl_->animatedNow) / gl_->skinFrames : 0.0,
                    " a frame, animation ", gl_->skinFrames ? gl_->totalMs / gl_->skinFrames : 0.0, " ms (bones ", gl_->skinFrames ? gl_->boneMs / gl_->skinFrames : 0.0,
                    ", skin ", gl_->skinFrames ? gl_->skinMs / gl_->skinFrames : 0.0,
                    ", upload ", gl_->skinFrames ? gl_->uploadMs / gl_->skinFrames : 0.0, " in ", gl_->skinFrames ? static_cast<double>(gl_->rangeCalls) / gl_->skinFrames : 0.0, " calls) and ",
                    gl_->skinFrames ? static_cast<double>(gl_->skinVerts) / gl_->skinFrames : 0.0, " vertices a frame");
        gl_->rangeCalls = 0;
        gl_->skinMs = gl_->boneMs = gl_->uploadMs = gl_->totalMs = 0.0;
        gl_->skinVerts = gl_->skinFrames = gl_->animatedNow = 0;
    }
}

// What the Vita draws no part of yet (weapons, helms and effects arrive with VITA-20 phase D).

bool CharacterRenderer::attachWeapon([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId, [[maybe_unused]] const pipeline::M2Model& weaponModel, [[maybe_unused]] uint32_t weaponModelId, [[maybe_unused]] const std::string& texturePath, [[maybe_unused]] const glm::mat4& localTransform) { return false; }

bool CharacterRenderer::attachWeaponEffect([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId, [[maybe_unused]] uint32_t visualSlot, [[maybe_unused]] const pipeline::M2Model& effectModel, [[maybe_unused]] uint32_t effectModelId) { return false; }

void CharacterRenderer::detachWeapon([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId) { }

void CharacterRenderer::detachWeaponEffects([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId) { }

bool CharacterRenderer::getAttachmentTransform([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t attachmentId, [[maybe_unused]] glm::mat4& outTransform) { return false; }

}  // namespace wowee::rendering

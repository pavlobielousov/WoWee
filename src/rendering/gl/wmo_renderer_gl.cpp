// Vita skeleton of WMORenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/wmo_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/wmo_renderer.hpp"

#include "pipeline/wmo_loader.hpp"
#include "rendering/gl/gl_program.hpp"
#include "rendering/gl/gl_stats.hpp"
#include "rendering/gl/gl_texture.hpp"
#include "rendering/gl/scene_params.hpp"
#include "rendering/gl/shader_sources.hpp"
#include "rendering/frustum.hpp"
#include "rendering/placement_transform.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"

#include <vitaGL.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <malloc.h>
#include <exception>
#include <future>
#include "core/thread_budget.hpp"
#include "pipeline/wmo_group_path.hpp"
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace wowee::rendering {

// Buildings (VITA-19): every group of a WMO is one vertex buffer and one index buffer; a group is culled by its box, a
// batch is one texture. No portals, no collision, no doodads of its own (those are M2 instances), no liquids, no normal maps.
struct WMORenderer::Gl {
    struct Uniforms {
        GLint viewProj = -1, model = -1, lightDir = -1, lightColor = -1, ambient = -1, wmoAmbient = -1, eye = -1, fog = -1,
              mode = -1, texture = -1, fogColor = -1, tint = -1, params = -1;
    };
    struct Vertex {  // 36 bytes
        float pos[3];
        float normal[3];
        float uv[2];
        uint8_t color[4];
    };
    struct Batch {
        uint32_t firstIndexBytes = 0;
        uint32_t indexCount = 0;
        GLuint texture = 0;
        gl::M2Kind kind = gl::M2Kind::Opaque;
        uint32_t blendMode = 0;
        bool unlit = false;
    };
    struct Group {
        GLuint vbo = 0, ibo = 0;
        std::vector<Batch> batches;
        glm::vec3 bmin{0.0f}, bmax{0.0f};
        bool interior = false;
        bool hasColors = false;
    };
    struct Model {
        std::vector<Group> groups;
        glm::vec3 ambient{0.15f};
        glm::vec3 bmin{0.0f}, bmax{0.0f};
        std::vector<GLuint> materialTexture;  // per material
        std::size_t bytes = 0;
        std::size_t nextGroup = 0;
        bool setupDone = false;
        std::string sourcePath;   // to parse it again for collision
        int collision = 0;        // 0 none, 1 being built, 2 in loadedModels
        unsigned retryTick = 0;   // after a build that ran out of heap: not before this collisionTick
    };
    struct Instance {
        uint32_t model = 0;
        glm::mat4 matrix{1.0f};
        glm::vec3 center{0.0f};
        float radius = 1.0f;
        bool hidden = false;
    };

    pipeline::AssetManager* assets = nullptr;
    GLuint program[3] = {0, 0, 0};
    Uniforms u[3];
    gl::TextureCache textures;
    std::unordered_map<uint32_t, Model> models;  // complete and in progress; main thread only
    std::mutex idMutex;
    std::unordered_set<uint32_t> completeIds;  // read from the tile workers (isModelLoaded)
    std::unordered_map<uint32_t, Instance> instances;
    uint32_t nextInstance = 1;
    bool ready = false;
    std::size_t gpuBytes = 0;
    int drawnGroups = 0, culledGroups = 0;
    // Collision is built for the buildings near the player only, on a worker thread, and dropped when they are far: the
    // grids cost about as much heap as the whole rest of the game around Goldshire (the heap hit 294 of 288 MB when every
    // loaded building had them).
    std::future<std::unique_ptr<WMORenderer::ModelData>> job;
    uint32_t jobModel = 0;
    unsigned collisionTick = 0;
    static void fillCollisionGroup(const pipeline::WMOGroup& g, WMORenderer::GroupResources& gr);
    static std::unique_ptr<WMORenderer::ModelData> buildCollisionModel(pipeline::AssetManager* assets, const std::string& path, uint32_t id);
    static std::unique_ptr<WMORenderer::ModelData> buildCollisionModelUnchecked(pipeline::AssetManager* assets, const std::string& path, uint32_t id);
};

WMORenderer::WMORenderer() = default;

WMORenderer::~WMORenderer() {
    if (gl_ && gl_->ready) shutdown();
}

bool WMORenderer::glReady() const { return gl_ && gl_->ready; }

bool WMORenderer::glInitialize(pipeline::AssetManager* assets) {
    if (glReady()) return true;
    gl_ = std::make_unique<Gl>();
    gl_->assets = assets;
    gl_->textures.setAssetManager(assets);
    gl_->textures.setAsync(true);  // BLPs are read on a worker; get() answers with a placeholder name (gl_texture.hpp)
    {
        // Building textures are capped at 128 on a side by default (WOWEE_WMO_TEXTURE_MAX in env.txt, 0 = keep all; each halving
        // of the cap takes three quarters of the memory of the textures above it: VITA-20 measured 128 on the device).
        const char* v = std::getenv("WOWEE_WMO_TEXTURE_MAX");
        if (!v) v = std::getenv("WOWEE_TEXTURE_MAX");
        gl_->textures.setMaxDimension(v ? std::atoi(v) : 128);
    }
    for (int k = 0; k < 3; ++k) {
        gl_->program[k] = gl::linkProgram(gl::wmoProgram(static_cast<gl::M2Kind>(k)));
        if (gl_->program[k] == 0) {
            LOG_ERROR("WMO (GL): program ", k, " did not link");
            return false;
        }
        Gl::Uniforms& u = gl_->u[k];
        const GLuint p = gl_->program[k];
        u.viewProj = glGetUniformLocation(p, "uViewProj");
        u.model = glGetUniformLocation(p, "uModel");
        u.lightDir = glGetUniformLocation(p, "uLightDir");
        u.lightColor = glGetUniformLocation(p, "uLightColor");
        u.ambient = glGetUniformLocation(p, "uAmbient");
        u.wmoAmbient = glGetUniformLocation(p, "uWmoAmbient");
        u.eye = glGetUniformLocation(p, "uEye");
        u.fog = glGetUniformLocation(p, "uFog");
        u.mode = glGetUniformLocation(p, "uMode");
        u.texture = glGetUniformLocation(p, "uTexture");
        u.fogColor = glGetUniformLocation(p, "uFogColor");
        u.tint = glGetUniformLocation(p, "uTint");
        u.params = glGetUniformLocation(p, "uParams");
    }
    gl_->ready = true;
    initialized_ = true;
    LOG_WARNING("WMO renderer (GL) ready");
    return true;
}

void WMORenderer::shutdown() {
    clearAll();
    if (gl_) {
        for (GLuint& p : gl_->program) {
            if (p) glDeleteProgram(p);
            p = 0;
        }
        gl_->textures.clear();
        gl_->ready = false;
    }
    initialized_ = false;
}

static void freeModel(WMORenderer::Gl& gl, std::size_t& gpuBytes, WMORenderer::Gl::Model& m) {
    (void)gl;
    for (auto& g : m.groups) {
        if (g.vbo) glDeleteBuffers(1, &g.vbo);
        if (g.ibo) glDeleteBuffers(1, &g.ibo);
    }
    gpuBytes -= std::min(gpuBytes, m.bytes);
}

void WMORenderer::clearInstances() {
    if (gl_) gl_->instances.clear();
    instances.clear();
    instanceIndexById.clear();
    spatialGrid.clear();
}

void WMORenderer::clearAll() {
    if (!gl_) return;
    for (auto& kv : gl_->models) freeModel(*gl_, gl_->gpuBytes, kv.second);
    gl_->models.clear();
    gl_->instances.clear();
    instances.clear();
    instanceIndexById.clear();
    spatialGrid.clear();
    loadedModels.clear();
    loadingModels_.clear();
    std::lock_guard<std::mutex> lock(gl_->idMutex);
    gl_->completeIds.clear();
}

bool WMORenderer::isModelLoaded(uint32_t id) const {
    if (!gl_) return false;
    std::lock_guard<std::mutex> lock(gl_->idMutex);
    return gl_->completeIds.count(id) != 0;
}

bool WMORenderer::loadModel(const pipeline::WMOModel& model, uint32_t id) {
    for (;;) {
        const ModelLoadResult r = loadModelIncremental(model, id, 0.0f);
        if (r == ModelLoadResult::InProgress) continue;
        return r == ModelLoadResult::Complete;
    }
}

WMORenderer::ModelLoadResult WMORenderer::loadModelIncremental(const pipeline::WMOModel& model, uint32_t id, float budgetMs) {
    if (!glReady()) return ModelLoadResult::Failed;
    if (isModelLoaded(id)) return ModelLoadResult::Complete;
    if (model.groups.empty()) return ModelLoadResult::Failed;
    const auto start = std::chrono::steady_clock::now();

    Gl::Model& out = gl_->models[id];
    if (!out.setupDone) {
        out.setupDone = true;
        out.ambient = glm::max(model.ambientColor, glm::vec3(0.15f));
        out.bmin = model.boundingBoxMin;
        out.bmax = model.boundingBoxMax;
        out.groups.reserve(model.groups.size());
        // Material -> texture: texture1 is a byte offset into MOTX (textureOffsetToIndex), or a plain index in some files.
        const std::size_t nTex = model.textures.size();
        auto resolve = [&](uint32_t field) -> uint32_t {
            auto it = model.textureOffsetToIndex.find(field);
            if (it != model.textureOffsetToIndex.end()) return it->second;
            return field < nTex ? field : 0xFFFFFFFFu;
        };
        out.materialTexture.assign(model.materials.size(), 0);
        for (std::size_t m = 0; m < model.materials.size(); ++m) {
            const pipeline::WMOMaterial& mat = model.materials[m];
            uint32_t pick = 0xFFFFFFFFu;
            for (uint32_t f : {mat.texture1, mat.texture2, mat.texture3}) {
                const uint32_t t = resolve(f);
                if (t < nTex && !model.textures[t].empty()) { pick = t; break; }
            }
            if (pick == 0xFFFFFFFFu) continue;
            std::string key = model.textures[pick];
            const std::size_t nul = key.find('\0');
            if (nul != std::string::npos) key.resize(nul);
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key.size() >= 4) {
                const std::string ext = key.substr(key.size() - 4);
                if (ext == ".tga" || ext == ".dds") key = key.substr(0, key.size() - 4) + ".blp";
            }
            if (key.empty()) continue;
            if (predecodedBLPCache_) {
                auto it = predecodedBLPCache_->find(key);
                if (it != predecodedBLPCache_->end()) gl_->textures.adopt(key, it->second);
            }
            out.materialTexture[m] = gl_->textures.get(key);
        }
    }

    while (out.nextGroup < model.groups.size()) {
        const pipeline::WMOGroup& g = model.groups[out.nextGroup++];
        Gl::Group group;
        group.bmin = g.boundingBoxMin;
        group.bmax = g.boundingBoxMax;
        group.interior = (g.flags & 0x2000) != 0;
        if (!g.vertices.empty() && !g.indices.empty() && !g.batches.empty()) {
            std::vector<Gl::Vertex> verts(g.vertices.size());
            bool anyColor = false;
            for (std::size_t i = 0; i < verts.size(); ++i) {
                const pipeline::WMOVertex& v = g.vertices[i];
                Gl::Vertex& o = verts[i];
                o.pos[0] = v.position.x; o.pos[1] = v.position.y; o.pos[2] = v.position.z;
                o.normal[0] = v.normal.x; o.normal[1] = v.normal.y; o.normal[2] = v.normal.z;
                o.uv[0] = v.texCoord.x; o.uv[1] = v.texCoord.y;
                for (int c = 0; c < 4; ++c) o.color[c] = static_cast<uint8_t>(std::clamp(v.color[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                if (v.color.x != 0.0f || v.color.y != 0.0f || v.color.z != 0.0f) anyColor = true;
            }
            group.hasColors = anyColor;
            if (!anyColor) for (auto& v : verts) { v.color[0] = v.color[1] = v.color[2] = 255; v.color[3] = 255; }
            glGenBuffers(1, &group.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, group.vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Gl::Vertex)), verts.data(), GL_STATIC_DRAW);
            glGenBuffers(1, &group.ibo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, group.ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(g.indices.size() * 2), g.indices.data(), GL_STATIC_DRAW);
            out.bytes += verts.size() * sizeof(Gl::Vertex) + g.indices.size() * 2;
            for (const pipeline::WMOBatch& b : g.batches) {
                if (b.indexCount == 0 || static_cast<std::size_t>(b.startIndex) + b.indexCount > g.indices.size()) continue;
                Gl::Batch batch;
                batch.firstIndexBytes = b.startIndex * 2;
                batch.indexCount = b.indexCount;
                if (b.materialId < model.materials.size()) {
                    const pipeline::WMOMaterial& mat = model.materials[b.materialId];
                    batch.blendMode = mat.blendMode;
                    batch.unlit = (mat.flags & 0x01) != 0;
                    batch.texture = out.materialTexture[b.materialId];
                }
                if (batch.texture == 0) batch.texture = gl_->textures.white();
                batch.kind = batch.blendMode == 0 ? gl::M2Kind::Opaque : (batch.blendMode == 1 ? gl::M2Kind::AlphaTest : gl::M2Kind::Blend);
                group.batches.push_back(batch);
            }
        }
        out.groups.push_back(std::move(group));
        if (budgetMs > 0.0f &&
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs &&
            out.nextGroup < model.groups.size()) {
            return ModelLoadResult::InProgress;
        }
    }
    gl_->gpuBytes += out.bytes;
    out.sourcePath = model.sourcePath;
    {
        std::lock_guard<std::mutex> lock(gl_->idMutex);
        gl_->completeIds.insert(id);
    }
    return ModelLoadResult::Complete;
}

static void worldBox(const glm::mat4& m, const glm::vec3& lo, const glm::vec3& hi, glm::vec3& outLo, glm::vec3& outHi) {
    outLo = glm::vec3(1e30f);
    outHi = glm::vec3(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
        outLo = glm::min(outLo, w);
        outHi = glm::max(outHi, w);
    }
}


// What upstream's createGroupResources keeps for collision, and the model-level fields the collision code reads
// (wmo_renderer.cpp); built from a freshly parsed WMO on a worker thread.
void WMORenderer::Gl::fillCollisionGroup(const pipeline::WMOGroup& g, WMORenderer::GroupResources& gr) {
    gr.groupFlags = g.flags;
    gr.boundingBoxMin = g.boundingBoxMin;
    gr.boundingBoxMax = g.boundingBoxMax;
    if (g.vertices.empty() || g.indices.empty()) return;
    gr.collisionVertices.reserve(g.vertices.size());
    for (const pipeline::WMOVertex& v : g.vertices) gr.collisionVertices.push_back(v.position);
    gr.collisionIndices = g.indices;
    if (!g.triFlags.empty()) {
        const std::size_t numTris = g.indices.size() / 3;
        gr.triMopyFlags.resize(numTris, 0);
        for (std::size_t t = 0; t < numTris; ++t) gr.triMopyFlags[t] = t < g.triFlags.size() ? g.triFlags[t] : 0;
    }
    gr.boundingBoxMin = gr.collisionVertices[0];
    gr.boundingBoxMax = gr.collisionVertices[0];
    for (const glm::vec3& v : gr.collisionVertices) {
        gr.boundingBoxMin = glm::min(gr.boundingBoxMin, v);
        gr.boundingBoxMax = glm::max(gr.boundingBoxMax, v);
    }
    gr.buildCollisionGrid();
    std::size_t hull = 0, renderedSolid = 0;
    for (uint8_t mopy : gr.triMopyFlags) {
        if (mopy & 0x08) ++hull;
        if ((mopy & 0x20) && !(mopy & 0x04)) ++renderedSolid;
    }
    const std::size_t tris = gr.collisionIndices.size() / 3;
    gr.noBlockingTriangles = (tris > 0 && hull == 0 && renderedSolid == 0);
}

std::unique_ptr<WMORenderer::ModelData> WMORenderer::Gl::buildCollisionModel(pipeline::AssetManager* assets, const std::string& path, uint32_t id) {
    core::enterThread(core::ThreadRole::AsyncObjectLoad);
    try {
        return buildCollisionModelUnchecked(assets, path, id);
    } catch (const std::exception& e) {
        // Out of memory in the middle of a big building: no collision for it, not an exit of the whole app.
        LOG_WARNING("WMO collision for model ", id, " failed: ", e.what());
        return std::make_unique<WMORenderer::ModelData>();
    }
}

#ifdef __vita__
extern "C" int _newlib_heap_size_user;
#endif

// Set by a collision build that gave up for lack of heap (not for being too big): the building is tried again later.
static std::atomic<bool> g_collisionFailedForMemory{false};

// Free heap, from the allocator (the heap is a fixed 288 MB block on the Vita).
static std::size_t freeHeapBytes() {
#ifdef __vita__
    const struct mallinfo mi = mallinfo();
    const std::size_t total = static_cast<std::size_t>(_newlib_heap_size_user);
    return total > static_cast<std::size_t>(mi.uordblks) ? total - static_cast<std::size_t>(mi.uordblks) : 0;
#else
    return ~static_cast<std::size_t>(0);
#endif
}

std::unique_ptr<WMORenderer::ModelData> WMORenderer::Gl::buildCollisionModelUnchecked(pipeline::AssetManager* assets, const std::string& path, uint32_t id) {
    auto md = std::make_unique<WMORenderer::ModelData>();
    if (!assets || path.empty()) return md;
    // 32 MB: the 60 MB this was with the 288 MB heap skipped the blacksmith and the inn of Goldshire at world entry once the heap was
    // 256 MB (about 200 MB in use there), and they stayed walk-through for the session (VITA-20). A building is parsed one group at a
    // time and the build gives up below kStopBelow, so a smaller head start is safe.
    constexpr std::size_t kNeedAtStart = 32u * 1024 * 1024;   // do not begin a building with less free heap than this
    constexpr std::size_t kStopBelow = 25u * 1024 * 1024;     // give up mid-way rather than run the heap out
    constexpr std::size_t kMaxTriangles = 90000;              // Stormwind has 730k: a city needs its own plan, not a grid per triangle
    if (freeHeapBytes() < kNeedAtStart) {
        LOG_WARNING("WMO collision for ", path, " skipped: only ", freeHeapBytes() / (1024 * 1024), " MB of heap free (will try again)");
        g_collisionFailedForMemory.store(true);
        return md;
    }
    const std::vector<uint8_t> data = assets->readFile(path);
    if (data.empty()) return md;
    pipeline::WMOModel model = pipeline::WMOLoader::load(data);
    md->id = id;
    md->boundingBoxMin = model.boundingBoxMin;
    md->boundingBoxMax = model.boundingBoxMax;
    md->wmoAmbientColor = model.ambientColor;
    const glm::vec3 ext = model.boundingBoxMax - model.boundingBoxMin;
    md->isLowPlatform = (ext.z < 6.0f && std::max(ext.x, ext.y) > 20.0f);
    md->groupPortalRefs.assign(model.nGroups, {0, 0});
    md->groups.resize(model.nGroups);
    std::size_t tris = 0;
    for (uint32_t gi = 0; gi < model.nGroups; ++gi) {
        // One group at a time: parse it, keep only its collision form, let the parsed copy go.
        for (const std::string& gp : pipeline::wmoGroupCandidates(path, gi)) {
            const std::vector<uint8_t> gd = assets->readFile(gp);
            if (gd.empty()) continue;
            pipeline::WMOLoader::loadGroup(gd, model, gi);
            break;
        }
        if (gi < model.groups.size()) {
            pipeline::WMOGroup& g = model.groups[gi];
            md->groupPortalRefs[gi] = {g.portalStart, g.portalCount};
            fillCollisionGroup(g, md->groups[gi]);
            tris += md->groups[gi].collisionIndices.size() / 3;
            std::vector<pipeline::WMOVertex>().swap(g.vertices);
            std::vector<uint16_t>().swap(g.indices);
            std::vector<pipeline::WMOBatch>().swap(g.batches);
            std::vector<uint8_t>().swap(g.triFlags);
        }
        if (tris > kMaxTriangles) {
            LOG_WARNING("WMO collision for ", path, " skipped: more than ", kMaxTriangles, " triangles (", model.nGroups, " groups)");
            return std::make_unique<WMORenderer::ModelData>();
        }
        if (freeHeapBytes() < kStopBelow) {
            LOG_WARNING("WMO collision for ", path, " stopped at group ", gi, " of ", model.nGroups, ": ", freeHeapBytes() / (1024 * 1024),
                        " MB of heap free (will try again)");
            g_collisionFailedForMemory.store(true);
            return std::make_unique<WMORenderer::ModelData>();
        }
    }
    for (const pipeline::WMOPortalRef& ref : model.portalRefs) {
        WMORenderer::PortalRef pr;
        pr.portalIndex = ref.portalIndex;
        pr.groupIndex = ref.groupIndex;
        pr.side = ref.side;
        md->portalRefs.push_back(pr);
    }
    md->setupDone = !md->groups.empty();
    LOG_WARNING("WMO collision built: ", path, " ", md->groups.size(), " groups, ", tris, " triangles, ", freeHeapBytes() / (1024 * 1024),
                " MB heap free after");
    return md;
}

static float boxDistance(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 d = glm::max(glm::max(lo - p, p - hi), glm::vec3(0.0f));
    return glm::length(d);
}

void WMORenderer::glUpdateCollision(const glm::vec3& focus) {
    if (!glReady()) return;
    constexpr float kBuildRadius = 120.0f;   // yards from the player's position to a building's edge
    constexpr float kDropRadius = 320.0f;

    // A finished build: hand it to the collision tables and give every instance of the model its group bounds.
    if (gl_->job.valid() && gl_->job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::unique_ptr<ModelData> md;
        try {
            md = gl_->job.get();
        } catch (const std::exception& e) {
            LOG_WARNING("WMO collision job failed: ", e.what());
        }
        const uint32_t id = gl_->jobModel;
        auto mit = gl_->models.find(id);
        if (mit != gl_->models.end()) {
            if (md && md->setupDone && !md->groups.empty()) {
                std::size_t tris = 0;
                for (const GroupResources& gr : md->groups) tris += gr.collisionIndices.size() / 3;
                loadedModels[id] = std::move(*md);
                mit->second.collision = 2;
                for (WMOInstance& wi : instances) {
                    if (wi.modelId != id) continue;
                    wi.worldGroupBounds.clear();
                    for (const GroupResources& gr : loadedModels[id].groups) {
                        glm::vec3 gLo, gHi;
                        worldBox(wi.modelMatrix, gr.boundingBoxMin, gr.boundingBoxMax, gLo, gHi);
                        wi.worldGroupBounds.emplace_back(gLo - glm::vec3(0.5f), gHi + glm::vec3(0.5f));
                    }
                }
                LOG_WARNING("WMO collision built for model ", id, ": ", loadedModels[id].groups.size(), " groups, ", tris, " triangles");
            } else {
                mit->second.collision = 0;
                if (g_collisionFailedForMemory.exchange(false)) {
                    mit->second.retryTick = gl_->collisionTick + 600;  // out of heap right now: the next try is in about 20 seconds
                } else {
                    mit->second.sourcePath.clear();  // could not be built: do not retry every time
                }
            }
        }
    }

    if (++gl_->collisionTick % 10 != 0) return;

    // Models none of whose instances are near any more give their memory back.
    for (auto& kv : gl_->models) {
        Gl::Model& m = kv.second;
        if (m.collision != 2) continue;
        bool near = false;
        for (const WMOInstance& wi : instances) {
            if (wi.modelId == kv.first && boxDistance(focus, wi.worldBoundsMin, wi.worldBoundsMax) < kDropRadius) { near = true; break; }
        }
        if (near) continue;
        loadedModels.erase(kv.first);
        for (WMOInstance& wi : instances) if (wi.modelId == kv.first) wi.worldGroupBounds.clear();
        m.collision = 0;
        LOG_WARNING("WMO collision dropped for model ", kv.first);
    }

    // The nearest building without collision, one at a time.
    if (gl_->job.valid()) return;
    float best = kBuildRadius;
    uint32_t bestModel = 0;
    for (const WMOInstance& wi : instances) {
        auto mit = gl_->models.find(wi.modelId);
        if (mit == gl_->models.end() || mit->second.collision != 0 || mit->second.sourcePath.empty()) continue;
        if (gl_->collisionTick < mit->second.retryTick) continue;
        const float d = boxDistance(focus, wi.worldBoundsMin, wi.worldBoundsMax);
        if (d < best) { best = d; bestModel = wi.modelId; }
    }
    if (bestModel == 0) return;
    Gl::Model& m = gl_->models[bestModel];
    m.collision = 1;
    gl_->jobModel = bestModel;
    gl_->job = std::async(std::launch::async, &Gl::buildCollisionModel, gl_->assets, m.sourcePath, bestModel);
}

uint32_t WMORenderer::createInstance(uint32_t modelId, const glm::vec3& position, const glm::vec3& rotation, float scale) {
    if (!glReady() || !isModelLoaded(modelId)) return 0;
    auto it = gl_->models.find(modelId);
    if (it == gl_->models.end()) return 0;
    Gl::Instance inst;
    inst.model = modelId;
    inst.matrix = placementModelMatrix(position, rotation, scale);
    glm::vec3 lo, hi;
    worldBox(inst.matrix, it->second.bmin, it->second.bmax, lo, hi);
    inst.center = (lo + hi) * 0.5f;
    inst.radius = glm::length(hi - lo) * 0.5f;
    const uint32_t id = gl_->nextInstance++;
    gl_->instances.emplace(id, inst);
    // The same instance for the upstream collision code: matrices, world and per-group bounds, spatial index.
    WMOInstance wi;
    wi.id = id;
    wi.modelId = modelId;
    wi.position = position;
    wi.rotation = rotation;
    wi.scale = scale;
    wi.modelMatrix = inst.matrix;
    wi.invModelMatrix = glm::inverse(inst.matrix);
    wi.worldBoundsMin = lo;
    wi.worldBoundsMax = hi;
    auto lmIt = loadedModels.find(modelId);
    if (lmIt != loadedModels.end()) {
        wi.worldGroupBounds.reserve(lmIt->second.groups.size());
        for (const GroupResources& gr : lmIt->second.groups) {
            glm::vec3 gLo, gHi;
            worldBox(inst.matrix, gr.boundingBoxMin, gr.boundingBoxMax, gLo, gHi);
            wi.worldGroupBounds.emplace_back(gLo - glm::vec3(0.5f), gHi + glm::vec3(0.5f));
        }
    }
    instances.push_back(wi);
    instanceIndexById[id] = instances.size() - 1;
    insertBounds(spatialGrid, wi.worldBoundsMin, wi.worldBoundsMax, id);
    return id;
}

bool WMORenderer::hasInstance(uint32_t instanceId) const { return gl_ && gl_->instances.count(instanceId) != 0; }

void WMORenderer::removeInstance(uint32_t instanceId) {
    removeInstances(std::vector<uint32_t>{instanceId});
}

void WMORenderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (!gl_ || instanceIds.empty()) return;
    for (uint32_t id : instanceIds) gl_->instances.erase(id);
    // The upstream copies: drop them and rebuild the index once for the whole batch.
    std::unordered_set<uint32_t> gone(instanceIds.begin(), instanceIds.end());
    instances.erase(std::remove_if(instances.begin(), instances.end(), [&](const WMOInstance& w) { return gone.count(w.id) != 0; }),
                    instances.end());
    rebuildSpatialIndex();
}

void WMORenderer::setInstanceHidden(uint32_t instanceId, bool hidden) {
    if (!gl_) return;
    auto it = gl_->instances.find(instanceId);
    if (it != gl_->instances.end()) it->second.hidden = hidden;
    auto idx = instanceIndexById.find(instanceId);
    if (idx != instanceIndexById.end() && idx->second < instances.size()) instances[idx->second].hidden = hidden;
}

void WMORenderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    if (!gl_) return;
    auto it = gl_->instances.find(instanceId);
    if (it == gl_->instances.end()) return;
    auto mit = gl_->models.find(it->second.model);
    if (mit == gl_->models.end()) return;
    it->second.matrix = transform;
    glm::vec3 lo, hi;
    worldBox(transform, mit->second.bmin, mit->second.bmax, lo, hi);
    it->second.center = (lo + hi) * 0.5f;
    it->second.radius = glm::length(hi - lo) * 0.5f;
    auto idx = instanceIndexById.find(instanceId);
    if (idx != instanceIndexById.end() && idx->second < instances.size()) {
        WMOInstance& wi = instances[idx->second];
        const glm::vec3 oldMin = wi.worldBoundsMin, oldMax = wi.worldBoundsMax;
        wi.modelMatrix = transform;
        wi.invModelMatrix = glm::inverse(transform);
        wi.worldBoundsMin = lo;
        wi.worldBoundsMax = hi;
        auto lm = loadedModels.find(wi.modelId);
        if (lm != loadedModels.end()) {
            wi.worldGroupBounds.clear();
            for (const GroupResources& gr : lm->second.groups) {
                glm::vec3 gLo, gHi;
                worldBox(transform, gr.boundingBoxMin, gr.boundingBoxMax, gLo, gHi);
                wi.worldGroupBounds.emplace_back(gLo - glm::vec3(0.5f), gHi + glm::vec3(0.5f));
            }
        }
        // Only this instance moves in the grid, and not at all while it stays in its cells. Rebuilding the whole index here
        // took ~100 ms a frame for a moving transport (the Stormwind box alone covers thousands of cells).
        refileBounds(spatialGrid, oldMin, oldMax, lo, hi, instanceId);
    }
}

void WMORenderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
}

uint32_t WMORenderer::glInstanceCount() const { return gl_ ? static_cast<uint32_t>(gl_->instances.size()) : 0; }

void WMORenderer::glRender(const gl::SceneParams& scene) {
    if (gl_) gl_->textures.pump(2.0);
    if (!glReady() || gl_->instances.empty()) return;
    static gl::FrameStats stats("WMO");
    stats.begin();
    long drawCalls = 0;
    Frustum frustum;
    frustum.extractFromMatrix(scene.cullViewProj);
    gl_->drawnGroups = gl_->culledGroups = 0;

    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    const glm::mat4 viewProj = scene.projection * scene.view;
    // Per-frame uniforms once per program per frame, per-batch ones only when they change (see the M2 renderer).
    int current = -1;
    bool frameSet[3] = {false, false, false};
    struct Last { glm::vec4 mode{-1.0f}; float fogToColour = -1.0f; const glm::mat4* model = nullptr; const void* modelData = nullptr; } last[3];
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
        glUniform3f(u.tint, 1.0f, 1.0f, 1.0f);
        glUniform1i(u.texture, 0);
        last[k] = Last{};
        glActiveTexture(GL_TEXTURE0);
    };
    for (int pass = 0; pass < 2; ++pass) {  // 0: opaque and alpha-tested, 1: blended
        if (pass == 0) { glDepthMask(GL_TRUE); glDisable(GL_BLEND); }
        else { glDepthMask(GL_FALSE); glEnable(GL_BLEND); }
        GLuint boundTexture = 0;
        for (const auto& kv : gl_->instances) {
            const Gl::Instance& inst = kv.second;
            if (inst.hidden) continue;
            if (glm::length(inst.center - scene.eye) - inst.radius > scene.fogEnd) continue;
            auto mit = gl_->models.find(inst.model);
            if (mit == gl_->models.end()) continue;
            const Gl::Model& model = mit->second;
            for (const Gl::Group& g : model.groups) {
                if (g.batches.empty()) continue;
                glm::vec3 lo, hi;
                worldBox(inst.matrix, g.bmin, g.bmax, lo, hi);
                const glm::vec3 c = (lo + hi) * 0.5f;
                const float r = glm::length(hi - lo) * 0.5f;
                if (glm::length(c - scene.eye) - r > scene.fogEnd || !frustum.intersectsSphere(c, r)) {
                    if (pass == 0) ++gl_->culledGroups;
                    continue;
                }
                bool bound = false;
                for (const Gl::Batch& b : g.batches) {
                    if ((b.kind == gl::M2Kind::Blend) != (pass == 1)) continue;
                    if (!bound) {
                        bound = true;
                        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
                        glEnableVertexAttribArray(0);
                        glEnableVertexAttribArray(1);
                        glEnableVertexAttribArray(2);
                        glEnableVertexAttribArray(3);
                        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, pos)));
                        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, normal)));
                        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, uv)));
                        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, color)));
                        if (pass == 0) ++gl_->drawnGroups;
                    }
                    const int k = static_cast<int>(b.kind);
                    useProgram(k);
                    const Gl::Uniforms& u = gl_->u[k];
                    if (b.kind == gl::M2Kind::Blend) {
                        switch (b.blendMode) {
                            case 3: case 6: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
                            case 4: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
                            case 5: glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;
                            default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
                        }
                    }
                    Last& l = last[k];
                    if (l.model != &inst.matrix) {
                        l.model = &inst.matrix;
                        glUniformMatrix4fv(u.model, 1, GL_FALSE, &inst.matrix[0][0]);
                    }
                    const float ext = (!g.interior && !b.unlit) ? 1.0f : 0.0f;
                    const float inner = (g.interior && !b.unlit) ? 1.0f : 0.0f;
                    const glm::vec4 mode(ext, inner, b.unlit ? 1.0f : 0.0f, g.hasColors ? 1.0f : 0.0f);
                    if (l.mode != mode) {
                        l.mode = mode;
                        glUniform4f(u.mode, mode.x, mode.y, mode.z, mode.w);
                    }
                    if (l.modelData != &model) {
                        l.modelData = &model;
                        glUniform3f(u.wmoAmbient, model.ambient.x, model.ambient.y, model.ambient.z);
                    }
                    const float fogToColour = (b.blendMode == 3 || b.blendMode == 4 || b.blendMode == 6) ? 0.0f : 1.0f;
                    if (l.fogToColour != fogToColour) {
                        l.fogToColour = fogToColour;
                        glUniform4f(u.params, 0.5f, 0.0f, 1.0f, fogToColour);
                    }
                    if (boundTexture != b.texture) {
                        boundTexture = b.texture;
                        glBindTexture(GL_TEXTURE_2D, b.texture);
                    }
                    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(b.indexCount), GL_UNSIGNED_SHORT,
                                   reinterpret_cast<const void*>(static_cast<uintptr_t>(b.firstIndexBytes)));
                    ++drawCalls;
                }
            }
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    stats.end(drawCalls, gl_->drawnGroups);
    if (stats.frames == 0) {
        LOG_WARNING("GL memory WMO: textures ", gl_->textures.bytes() / 1024, " KB in ", gl_->textures.count(), " (RGBA ", gl_->textures.rgbaBytes() / 1024, " KB in ", gl_->textures.rgbaCount(), ")", ", buffers ", gl_->gpuBytes / 1024,
                    " KB, models ", gl_->models.size(), ", instances ", gl_->instances.size());
    }
}

void WMORenderer::addDoodadToInstance([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t m2InstanceId, [[maybe_unused]] const glm::mat4& localTransform) { }




void WMORenderer::debugDumpGroupsAtPosition([[maybe_unused]] float glX, [[maybe_unused]] float glY, [[maybe_unused]] float glZ) const { }

const std::vector<WMORenderer::DoodadTemplate>* WMORenderer::getDoodadTemplates([[maybe_unused]] uint32_t modelId) const { return nullptr; }


bool WMORenderer::instanceHasCollisionGeometry(uint32_t instanceId) const {
    auto idx = instanceIndexById.find(instanceId);
    if (idx == instanceIndexById.end() || idx->second >= instances.size()) return false;
    auto model = loadedModels.find(instances[idx->second].modelId);
    return model != loadedModels.end() && model->second.setupDone && !model->second.groups.empty();
}





size_t WMORenderer::setInstanceDoodadAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool loop) { return 0; }


void WMORenderer::setInstanceIsTransport([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool isTransport) { }


pipeline::BLPImage WMORenderer::generateNormalHeightMapPixels([[maybe_unused]] const uint8_t* pixels, [[maybe_unused]] uint32_t width, [[maybe_unused]] uint32_t height, [[maybe_unused]] float& outVariance) { return {}; }


}  // namespace wowee::rendering

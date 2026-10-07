// The Vita's terrain renderer (VITA-18, ADR-001): the GL half of rendering/terrain_renderer.hpp. TerrainManager (upstream's,
// compiled as it is) streams the ADT tiles, builds the meshes and hands them to loadTerrain*; this uploads them and draws.
//
// One vertex buffer and one index buffer per tile (256 chunks of 145 vertices: 37,120 vertices, so 16-bit indices fit once the
// chunk's vertices are offset into the tile's), one packed RGBA alpha texture per chunk (layers 1-3 in r, g, b: the
// terrain_renderer shader's uAlpha), and the base and layer textures from the shared TextureCache.
#include "rendering/terrain_renderer.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/frustum.hpp"
#include "rendering/gl/gl_stats.hpp"
#include "rendering/gl/gl_program.hpp"
#include "rendering/gl/gl_texture.hpp"
#include "rendering/gl/shader_sources.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <vitaGL.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <malloc.h>

namespace wowee::rendering {

namespace {

struct GlVertex {  // 40 bytes; what the vertex shader reads: aPosition, aNormal, aTexCoord, aLayerUV
    float pos[3];
    float normal[3];
    float uv[2];
    float layerUv[2];
};
static_assert(sizeof(GlVertex) == 40);

constexpr int kChunksPerTile = 256;

}  // namespace

struct TerrainRenderer::Gl {
    struct Uniforms {
        GLint viewProj = -1, model = -1, lightDir = -1, lightColor = -1, ambient = -1, eye = -1, fog = -1, fogColor = -1;
        GLint base = -1, alpha = -1, layer[3] = {-1, -1, -1};
    };
    struct ChunkRecord {
        uint32_t firstIndexBytes = 0;
        uint32_t indexCount = 0;
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        int layers = 0;  // 0..3 above the base
        GLuint base = 0, layer[3] = {0, 0, 0};
        GLuint alpha = 0;  // packed RGBA, 0 when there are no layers
    };
    struct Tile {
        int x = -1, y = -1;
        GLuint vbo = 0, ibo = 0;
        std::vector<ChunkRecord> chunks;
        std::size_t bytes = 0;
    };
    struct Building {  // a tile whose chunks are still arriving (loadTerrainIncremental)
        int x = -1, y = -1;
        std::vector<GlVertex> vertices;
        std::vector<uint16_t> indices;
        std::vector<ChunkRecord> chunks;
        std::vector<GLuint> alphaTextures;
    };

    pipeline::AssetManager* assets = nullptr;
    GLuint program[4] = {0, 0, 0, 0};
    Uniforms u[4];
    gl::TextureCache textures;
    std::vector<Tile> tiles;
    std::vector<Building> building;
    bool ready = false;
    std::size_t gpuBytes = 0;
};

TerrainRenderer::TerrainRenderer() = default;

TerrainRenderer::~TerrainRenderer() {
    if (gl_ && gl_->ready) shutdown();
}

bool TerrainRenderer::glReady() const { return gl_ && gl_->ready; }

bool TerrainRenderer::glInitialize(pipeline::AssetManager* assetManager) {
    if (glReady()) return true;
    gl_ = std::make_unique<Gl>();
    gl_->assets = assetManager;
    this->assetManager = assetManager;
    gl_->textures.setAssetManager(assetManager);
    gl_->textures.setAsync(true);  // BLPs are read on a worker; get() answers with a placeholder name (gl_texture.hpp)
    // Compressed textures stay compressed (the BLP loader decodes them to RGBA unless told the GPU takes the blocks).
    pipeline::setBlockCompressionSupported(true);
    for (int n = 0; n < 4; ++n) {
        gl_->program[n] = gl::linkProgram(gl::terrainProgram(n));
        if (gl_->program[n] == 0) {
            LOG_ERROR("Terrain: the ", n, "-layer program did not link");
            return false;
        }
        Gl::Uniforms& u = gl_->u[n];
        const GLuint p = gl_->program[n];
        u.viewProj = glGetUniformLocation(p, "uViewProj");
        u.model = glGetUniformLocation(p, "uModel");
        u.lightDir = glGetUniformLocation(p, "uLightDir");
        u.lightColor = glGetUniformLocation(p, "uLightColor");
        u.ambient = glGetUniformLocation(p, "uAmbient");
        u.eye = glGetUniformLocation(p, "uEye");
        u.fog = glGetUniformLocation(p, "uFog");
        u.fogColor = glGetUniformLocation(p, "uFogColor");
        u.base = glGetUniformLocation(p, "uBase");
        u.alpha = glGetUniformLocation(p, "uAlpha");
        u.layer[0] = glGetUniformLocation(p, "uLayer1");
        u.layer[1] = glGetUniformLocation(p, "uLayer2");
        u.layer[2] = glGetUniformLocation(p, "uLayer3");
    }
    gl_->ready = true;
    LOG_WARNING("Terrain renderer (GL) ready");
    return true;
}

void TerrainRenderer::shutdown() {
    clear();
    if (gl_) {
        for (GLuint& p : gl_->program) {
            if (p) glDeleteProgram(p);
            p = 0;
        }
        gl_->textures.clear();
        gl_->ready = false;
    }
}

void TerrainRenderer::clear() {
    if (!gl_) return;
    for (Gl::Tile& t : gl_->tiles) {
        if (t.vbo) glDeleteBuffers(1, &t.vbo);
        if (t.ibo) glDeleteBuffers(1, &t.ibo);
        for (Gl::ChunkRecord& c : t.chunks) if (c.alpha) glDeleteTextures(1, &c.alpha);
    }
    gl_->tiles.clear();
    for (Gl::Building& b : gl_->building) for (GLuint a : b.alphaTextures) glDeleteTextures(1, &a);
    gl_->building.clear();
    gl_->gpuBytes = 0;
    chunks.clear();
}

void TerrainRenderer::removeTile(int tileX, int tileY) {
    if (!gl_) return;
    auto it = std::find_if(gl_->tiles.begin(), gl_->tiles.end(),
                           [&](const Gl::Tile& t) { return t.x == tileX && t.y == tileY; });
    if (it != gl_->tiles.end()) {
        if (it->vbo) glDeleteBuffers(1, &it->vbo);
        if (it->ibo) glDeleteBuffers(1, &it->ibo);
        for (Gl::ChunkRecord& c : it->chunks) if (c.alpha) glDeleteTextures(1, &c.alpha);
        gl_->gpuBytes -= std::min(gl_->gpuBytes, it->bytes);
        gl_->tiles.erase(it);
    }
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(),
                                [&](const TerrainChunkGPU& c) { return c.tileX == tileX && c.tileY == tileY; }),
                 chunks.end());
}

void TerrainRenderer::recreatePipelines() {}

void TerrainRenderer::uploadPreloadedTextures(const std::unordered_map<std::string, pipeline::BLPImage>& textures) {
    if (!gl_) return;
    for (const auto& [path, image] : textures) gl_->textures.adopt(path, image);
}

int TerrainRenderer::getTriangleCount() const {
    std::size_t total = 0;
    for (const TerrainChunkGPU& c : chunks) total += c.indexCount / 3;
    return static_cast<int>(total);
}

// ---- loading ------------------------------------------------------------------------------------------------------------

namespace {

/// Layers 1-3 of a chunk in one RGBA texture: the alpha map of layer n in channel n-1 (255 where there is none). 64x64.
GLuint packAlpha(const pipeline::ChunkMesh& chunk, std::size_t layers) {
    std::vector<uint8_t> rgba(64 * 64 * 4, 255);
    for (std::size_t li = 1; li < chunk.layers.size() && li <= layers && li < 4; ++li) {
        const std::vector<uint8_t>& a = chunk.layers[li].alphaData;
        for (std::size_t i = 0; i < 64 * 64; ++i) rgba[i * 4 + (li - 1)] = i < a.size() ? a[i] : 255;
    }
    // Four bits a channel: sixteen steps of blend between two ground textures, at half the memory (WotLK stores eight, but the
    // GPU pools were full around Goldshire; WOWEE_ALPHA8=1 in env.txt keeps eight).
    static const bool alpha8 = std::getenv("WOWEE_ALPHA8") != nullptr;
    gl::GlTexture tex = alpha8 ? gl::uploadRgba(rgba.data(), 64, 64, false) : gl::uploadRgba4444(rgba.data(), 64, 64, false);
    return tex.id;
}

}  // namespace

bool TerrainRenderer::loadTerrainIncremental(const pipeline::TerrainMesh& mesh, const std::vector<std::string>& texturePaths,
                                             int tileX, int tileY, int& chunkIndex, int maxChunksPerCall) {
    if (!glReady()) return true;  // nothing to upload to: report done so the tile does not stall the streamer
    if (mesh.validChunkCount == 0) return true;

    auto it = std::find_if(gl_->building.begin(), gl_->building.end(),
                           [&](const Gl::Building& b) { return b.x == tileX && b.y == tileY; });
    if (chunkIndex == 0 || it == gl_->building.end()) {
        if (it != gl_->building.end()) gl_->building.erase(it);
        gl_->building.emplace_back();
        gl_->building.back().x = tileX;
        gl_->building.back().y = tileY;
        it = gl_->building.end() - 1;
    }
    Gl::Building& b = *it;

    int done = 0;
    while (chunkIndex < kChunksPerTile && done < maxChunksPerCall) {
        const pipeline::ChunkMesh& chunk = mesh.chunks[static_cast<std::size_t>(chunkIndex)];
        ++chunkIndex;
        if (!chunk.isValid()) continue;
        ++done;
        if (b.vertices.size() + chunk.vertices.size() > 65535) continue;  // 16-bit indices; a tile never reaches this

        Gl::ChunkRecord rec;
        const uint32_t base = static_cast<uint32_t>(b.vertices.size());
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const pipeline::TerrainVertex& v : chunk.vertices) {
            GlVertex g;
            std::memcpy(g.pos, v.position, sizeof g.pos);
            std::memcpy(g.normal, v.normal, sizeof g.normal);
            std::memcpy(g.uv, v.texCoord, sizeof g.uv);
            std::memcpy(g.layerUv, v.layerUV, sizeof g.layerUv);
            b.vertices.push_back(g);
            lo = glm::min(lo, glm::vec3(v.position[0], v.position[1], v.position[2]));
            hi = glm::max(hi, glm::vec3(v.position[0], v.position[1], v.position[2]));
        }
        rec.firstIndexBytes = static_cast<uint32_t>(b.indices.size() * sizeof(uint16_t));
        rec.indexCount = static_cast<uint32_t>(chunk.indices.size());
        for (pipeline::TerrainIndex idx : chunk.indices) b.indices.push_back(static_cast<uint16_t>(base + idx));
        rec.center = (lo + hi) * 0.5f;
        rec.radius = glm::length(hi - lo) * 0.5f;

        // Textures: layer 0 is the base; up to three more blend over it with their alpha maps.
        auto textureFor = [&](uint32_t id) {
            return id < texturePaths.size() ? gl_->textures.get(texturePaths[id]) : gl_->textures.white();
        };
        rec.base = chunk.layers.empty() ? gl_->textures.white() : textureFor(chunk.layers[0].textureId);
        rec.layers = static_cast<int>(std::min<std::size_t>(chunk.layers.empty() ? 0 : chunk.layers.size() - 1, 3));
        for (int li = 0; li < rec.layers; ++li) rec.layer[li] = textureFor(chunk.layers[static_cast<std::size_t>(li) + 1].textureId);
        if (rec.layers > 0) {
            rec.alpha = packAlpha(chunk, static_cast<std::size_t>(rec.layers));
            b.alphaTextures.push_back(rec.alpha);
        }
        b.chunks.push_back(rec);

        TerrainChunkGPU stat;
        stat.indexCount = rec.indexCount;
        stat.tileX = tileX;
        stat.tileY = tileY;
        stat.layerCount = rec.layers;
        chunks.push_back(stat);
    }
    if (chunkIndex < kChunksPerTile) return false;

    // Every chunk is in: one vertex buffer and one index buffer for the tile.
    Gl::Tile tile;
    tile.x = tileX;
    tile.y = tileY;
    tile.chunks = std::move(b.chunks);
    glGenBuffers(1, &tile.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, tile.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(b.vertices.size() * sizeof(GlVertex)), b.vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &tile.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tile.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(b.indices.size() * sizeof(uint16_t)), b.indices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    std::size_t alphaTextures = 0;
    for (const Gl::ChunkRecord& c : tile.chunks) if (c.alpha) ++alphaTextures;
    static const bool alpha8Bytes = std::getenv("WOWEE_ALPHA8") != nullptr;
    tile.bytes = b.vertices.size() * sizeof(GlVertex) + b.indices.size() * sizeof(uint16_t) + alphaTextures * 64 * 64 * (alpha8Bytes ? 4 : 2);
    gl_->gpuBytes += tile.bytes;
    const struct mallinfo heap = mallinfo();
    LOG_WARNING("Terrain tile [", tileX, ",", tileY, "] uploaded (heap in use ", static_cast<unsigned>(heap.uordblks) / (1024 * 1024),
                " MB): ", tile.chunks.size(), " chunks, ", b.vertices.size(),
                " vertices, ", tile.bytes / 1024, " KB buffers+alpha, textures cached ", gl_->textures.count(), " (",
                gl_->textures.bytes() / 1024, " KB)");
    gl_->tiles.push_back(std::move(tile));
    gl_->building.erase(it);
    return true;
}

bool TerrainRenderer::loadTerrain(const pipeline::TerrainMesh& mesh, const std::vector<std::string>& texturePaths,
                                  int tileX, int tileY) {
    if (mesh.validChunkCount == 0) return false;
    int next = 0;
    while (!loadTerrainIncremental(mesh, texturePaths, tileX, tileY, next, kChunksPerTile)) {}
    return !chunks.empty();
}

// ---- drawing ------------------------------------------------------------------------------------------------------------

void TerrainRenderer::glRender(const gl::SceneParams& scene) {
    if (gl_) gl_->textures.pump(2.0);
    if (!glReady() || gl_->tiles.empty()) return;
    static gl::FrameStats stats("terrain");
    stats.begin();

    Frustum frustum;
    frustum.extractFromMatrix(scene.cullViewProj);
    const float maxDistSq = std::min(scene.viewDistance, maxViewDistance_) * std::min(scene.viewDistance, maxViewDistance_);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);  // the terrain winding is upstream's Vulkan one; culling comes back with the renderer's own state

    const glm::mat4 identity(1.0f);
    bool programSet[4] = {false, false, false, false};
    int rendered = 0, culled = 0;
    float furthest = 0.0f;

    for (const Gl::Tile& tile : gl_->tiles) {
        glBindBuffer(GL_ARRAY_BUFFER, tile.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tile.ibo);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<const void*>(offsetof(GlVertex, pos)));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<const void*>(offsetof(GlVertex, normal)));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<const void*>(offsetof(GlVertex, uv)));
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<const void*>(offsetof(GlVertex, layerUv)));

        int current = -1;
        for (const Gl::ChunkRecord& c : tile.chunks) {
            const glm::vec3 d = c.center - scene.eye;
            const float distSq = glm::dot(d, d);
            const float reach = std::sqrt(maxDistSq) + c.radius;
            if (distSq > reach * reach || (frustumCullingEnabled && !frustum.intersectsSphere(c.center, c.radius))) {
                ++culled;
                continue;
            }
            if (c.layers != current) {
                current = c.layers;
                const Gl::Uniforms& u = gl_->u[current];
                glUseProgram(gl_->program[current]);
                if (!programSet[current]) {
                    programSet[current] = true;
                    glUniformMatrix4fv(u.viewProj, 1, GL_FALSE, glm::value_ptr(scene.projection * scene.view));
                    glUniformMatrix4fv(u.model, 1, GL_FALSE, glm::value_ptr(identity));
                    glUniform4f(u.lightDir, scene.lightDir.x, scene.lightDir.y, scene.lightDir.z, 0.0f);
                    glUniform3f(u.lightColor, scene.lightColor.x, scene.lightColor.y, scene.lightColor.z);
                    glUniform3f(u.ambient, scene.ambient.x, scene.ambient.y, scene.ambient.z);
                    glUniform4f(u.eye, scene.eye.x, scene.eye.y, scene.eye.z, 1.0f);
                    glUniform4f(u.fog, fogEnabled ? scene.fogStart : 1.0e8f, fogEnabled ? scene.fogEnd : 2.0e8f, 0.0f, 0.0f);
                    glUniform3f(u.fogColor, scene.fogColor.x, scene.fogColor.y, scene.fogColor.z);
                    glUniform1i(u.base, 0);
                    if (current >= 1) glUniform1i(u.alpha, 1);
                    for (int li = 0; li < current; ++li) glUniform1i(u.layer[li], 2 + li);
                }
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, c.base);
            if (c.layers >= 1) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, c.alpha);
                for (int li = 0; li < c.layers; ++li) {
                    glActiveTexture(GL_TEXTURE2 + li);
                    glBindTexture(GL_TEXTURE_2D, c.layer[li]);
                }
            }
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(c.indexCount), GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(static_cast<std::uintptr_t>(c.firstIndexBytes)));
            ++rendered;
            furthest = std::max(furthest, distSq);
        }
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glDisableVertexAttribArray(3);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    renderedChunks = rendered;
    culledChunks = culled;
    stats.end(rendered, rendered);
    if (stats.frames == 0) {
        LOG_WARNING("GL memory terrain: textures ", gl_->textures.bytes() / 1024, " KB in ", gl_->textures.count(), ", tiles ", gl_->tiles.size(),
                    " buffers+alpha ", gl_->gpuBytes / 1024, " KB");
    }
    furthestDrawnSq_ = furthest;
}

}  // namespace wowee::rendering

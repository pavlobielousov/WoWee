// Water on vitaGL (VITA-21, ADR-001): the surfaces are kept as upstream keeps them (the queries for swimming and the
// liquid type read them), and drawn as one blended mesh per terrain tile or building: a flat colour per liquid type with a
// slow shimmer (0.88 to 1.0 of the colour, in the fragment shader) and fog, no reflection, refraction or geometry waves.
#include "rendering/water_renderer.hpp"

#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/gl/gl_program.hpp"
#include "rendering/gl/scene_params.hpp"
#include "rendering/gl/shader_sources.hpp"
#include "rendering/water_mask.hpp"
#include "rendering/water_surface_grid.hpp"

#include <vitaGL.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace wowee::rendering {

struct WaterRenderer::Gl {
    struct Vertex {
        float pos[3];
        float uv[2];
        uint8_t color[4];
    };
    struct Mesh {
        GLuint vbo = 0, ibo = 0;
        int indexCount = 0;
    };
    GLuint program = 0;
    GLint viewProj = -1, eye = -1, fog = -1, fogColor = -1, time = -1;
    std::unordered_map<int64_t, Mesh> meshes;  // terrain tile (x << 8 | y) or building (1 << 40 | wmoId)
    bool ready = false;
};

namespace {
int64_t tileKey(int x, int y) { return (static_cast<int64_t>(x & 0xFF) << 8) | static_cast<int64_t>(y & 0xFF); }
int64_t wmoKey(uint32_t id) { return (int64_t{1} << 40) | id; }
}  // namespace

WaterRenderer::WaterRenderer() = default;

WaterRenderer::~WaterRenderer() {
    if (gl_ && gl_->ready) shutdown();
}

bool WaterRenderer::glReady() const { return gl_ && gl_->ready; }

bool WaterRenderer::glInitialize() {
    if (glReady()) return true;
    gl_ = std::make_unique<Gl>();
    gl_->program = gl::linkProgram(gl::waterProgram());
    if (gl_->program == 0) {
        LOG_ERROR("Water (GL): the program did not link");
        return false;
    }
    gl_->viewProj = glGetUniformLocation(gl_->program, "uViewProj");
    gl_->eye = glGetUniformLocation(gl_->program, "uEye");
    gl_->fog = glGetUniformLocation(gl_->program, "uFog");
    gl_->fogColor = glGetUniformLocation(gl_->program, "uFogColor");
    gl_->time = glGetUniformLocation(gl_->program, "uTime");
    gl_->ready = true;
    LOG_WARNING("Water renderer (GL) ready");
    return true;
}

void WaterRenderer::shutdown() {
    clear();
    if (gl_) {
        if (gl_->program) glDeleteProgram(gl_->program);
        gl_->program = 0;
        gl_->ready = false;
    }
}

void WaterRenderer::clear() {
    surfaces.clear();
    if (!gl_) return;
    for (auto& kv : gl_->meshes) {
        if (kv.second.vbo) glDeleteBuffers(1, &kv.second.vbo);
        if (kv.second.ibo) glDeleteBuffers(1, &kv.second.ibo);
    }
    gl_->meshes.clear();
}

glm::vec4 WaterRenderer::getLiquidColor(uint16_t liquidType) const {
    const uint8_t basicType = (liquidType == 0) ? 0 : ((liquidType - 1) % 4);
    switch (basicType) {
        case 0: return glm::vec4(0.10f, 0.28f, 0.55f, 1.0f);  // inland
        case 1: return glm::vec4(0.04f, 0.16f, 0.38f, 1.0f);  // ocean
        case 2: return glm::vec4(0.90f, 0.30f, 0.05f, 1.0f);  // magma
        case 3: return glm::vec4(0.20f, 0.60f, 0.10f, 1.0f);  // slime
        default: return glm::vec4(0.10f, 0.28f, 0.55f, 1.0f);
    }
}

float WaterRenderer::getLiquidAlpha(uint16_t liquidType) const {
    const uint8_t basicType = (liquidType == 0) ? 0 : ((liquidType - 1) % 4);
    switch (basicType) {
        case 1: return 0.72f;
        case 2: return 0.75f;
        case 3: return 0.65f;
        default: return 0.48f;
    }
}

// Appends one surface to a mesh under construction (positions as the upstream mesh builds them).
static void appendSurface(const WaterSurface& surface, const glm::vec4& colour, float alpha, std::vector<WaterRenderer::Gl::Vertex>& vertices,
                          std::vector<uint16_t>& indices) {
    const int gridWidth = surface.width + 1;
    const int gridHeight = surface.height + 1;
    constexpr float kZBias = 0.02f;
    if (vertices.size() + static_cast<std::size_t>(gridWidth) * static_cast<std::size_t>(gridHeight) > 65535) return;
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    const uint8_t c[4] = {static_cast<uint8_t>(colour.r * 255.0f), static_cast<uint8_t>(colour.g * 255.0f),
                          static_cast<uint8_t>(colour.b * 255.0f), static_cast<uint8_t>(alpha * 255.0f)};
    for (int y = 0; y < gridHeight; ++y) {
        for (int x = 0; x < gridWidth; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(gridWidth) + static_cast<std::size_t>(x);
            const float height = index < surface.heights.size() ? surface.heights[index] : surface.minHeight;
            const glm::vec3 pos = surface.origin + surface.stepX * static_cast<float>(x) + surface.stepY * static_cast<float>(y);
            WaterRenderer::Gl::Vertex v{};
            v.pos[0] = pos.x;
            v.pos[1] = pos.y;
            v.pos[2] = height + kZBias;
            v.uv[0] = pos.x * 0.35f;
            v.uv[1] = pos.y * 0.35f;
            v.color[0] = c[0]; v.color[1] = c[1]; v.color[2] = c[2]; v.color[3] = c[3];
            vertices.push_back(v);
        }
    }
    for (int y = 0; y < gridHeight - 1; ++y) {
        for (int x = 0; x < gridWidth - 1; ++x) {
            if (!waterCellRendered(surface.mask, surface.wmoId, surface.width, surface.xOffset, surface.yOffset, x, y)) continue;
            const uint16_t tl = static_cast<uint16_t>(base + y * gridWidth + x), tr = static_cast<uint16_t>(tl + 1);
            const uint16_t bl = static_cast<uint16_t>(base + (y + 1) * gridWidth + x), br = static_cast<uint16_t>(bl + 1);
            indices.insert(indices.end(), {tl, bl, tr, tr, bl, br});
        }
    }
}

// Rebuilds the mesh of one tile or building from the surfaces that belong to it.
void WaterRenderer::rebuildGlMesh(int64_t key, int tileX, int tileY, uint32_t wmoId) {
    if (!glReady()) return;
    auto old = gl_->meshes.find(key);
    if (old != gl_->meshes.end()) {
        if (old->second.vbo) glDeleteBuffers(1, &old->second.vbo);
        if (old->second.ibo) glDeleteBuffers(1, &old->second.ibo);
        gl_->meshes.erase(old);
    }
    std::vector<Gl::Vertex> vertices;
    std::vector<uint16_t> indices;
    for (const WaterSurface& s : surfaces) {
        const bool mine = wmoId != 0 ? s.wmoId == wmoId : (s.wmoId == 0 && s.tileX == tileX && s.tileY == tileY);
        if (!mine) continue;
        appendSurface(s, getLiquidColor(s.liquidType), getLiquidAlpha(s.liquidType), vertices, indices);
    }
    if (indices.empty()) return;
    Gl::Mesh mesh;
    mesh.indexCount = static_cast<int>(indices.size());
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Gl::Vertex)), vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &mesh.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint16_t)), indices.data(), GL_STATIC_DRAW);
    gl_->meshes[key] = mesh;
}

void WaterRenderer::loadFromTerrain(const pipeline::ADTTerrain& terrain, bool append, int tileX, int tileY) {
    constexpr float TILE_SIZE = 33.33333f / 8.0f;
    if (!append) clear();
    // This tile's old surfaces go first (a tile can be loaded again).
    surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(),
                                  [&](const WaterSurface& s) { return s.wmoId == 0 && s.tileX == tileX && s.tileY == tileY; }),
                   surfaces.end());
    for (int chunkIdx = 0; chunkIdx < 256; ++chunkIdx) {
        const auto& chunkWater = terrain.waterData[chunkIdx];
        if (!chunkWater.hasWater()) continue;
        const auto& terrainChunk = terrain.getChunk(chunkIdx % 16, chunkIdx / 16);
        for (const auto& layer : chunkWater.layers) {
            WaterSurface surface;
            surface.position = glm::vec3(terrainChunk.position[0], terrainChunk.position[1], layer.minHeight);
            surface.origin = glm::vec3(surface.position.x - static_cast<float>(layer.y) * TILE_SIZE,
                                       surface.position.y - static_cast<float>(layer.x) * TILE_SIZE, layer.minHeight);
            surface.stepX = glm::vec3(0.0f, -TILE_SIZE, 0.0f);
            surface.stepY = glm::vec3(-TILE_SIZE, 0.0f, 0.0f);
            surface.minHeight = layer.minHeight;
            surface.maxHeight = layer.maxHeight;
            surface.liquidType = layer.liquidType;
            surface.xOffset = layer.x;
            surface.yOffset = layer.y;
            surface.width = layer.width;
            surface.height = layer.height;
            const std::size_t numVertices = static_cast<std::size_t>(layer.width + 1) * static_cast<std::size_t>(layer.height + 1);
            bool useFlat = true;
            if (layer.heights.size() == numVertices) {
                bool sane = true;
                for (float h : layer.heights) {
                    if (!std::isfinite(h) || std::abs(h) > 50000.0f || h < layer.minHeight - 8.0f || h > layer.maxHeight + 8.0f) {
                        sane = false;
                        break;
                    }
                }
                if (sane) {
                    useFlat = false;
                    surface.heights = layer.heights;
                }
            }
            if (useFlat) surface.heights.assign(numVertices, layer.minHeight);
            surface.mask = layer.mask;
            surface.tileX = tileX;
            surface.tileY = tileY;
            surfaces.push_back(std::move(surface));
        }
    }
    rebuildGlMesh(tileKey(tileX, tileY), tileX, tileY, 0);
}

void WaterRenderer::loadFromWMO(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix, uint32_t wmoId) {
    if (!liquid.hasLiquid() || liquid.xTiles == 0 || liquid.yTiles == 0 || liquid.xVerts < 2 || liquid.yVerts < 2) return;
    if (liquid.xTiles != liquid.xVerts - 1 || liquid.yTiles != liquid.yVerts - 1 || liquid.xTiles > 64 || liquid.yTiles > 64) return;
    WaterSurface surface;
    surface.tileX = -1;
    surface.tileY = -1;
    surface.wmoId = wmoId;
    surface.liquidType = liquid.materialId;
    surface.width = static_cast<uint8_t>(std::min<uint32_t>(255, liquid.xTiles));
    surface.height = static_cast<uint8_t>(std::min<uint32_t>(255, liquid.yTiles));
    constexpr float WMO_LIQUID_TILE_SIZE = 4.1666625f;
    surface.origin = glm::vec3(modelMatrix * glm::vec4(liquid.basePosition, 1.0f));
    surface.stepX = glm::vec3(modelMatrix * glm::vec4(WMO_LIQUID_TILE_SIZE, 0.0f, 0.0f, 0.0f));
    surface.stepY = glm::vec3(modelMatrix * glm::vec4(0.0f, WMO_LIQUID_TILE_SIZE, 0.0f, 0.0f));
    const float stepXLen = glm::length(surface.stepX), stepYLen = glm::length(surface.stepY);
    const glm::vec3 planeN = glm::cross(surface.stepX, surface.stepY);
    const float nLenSq = glm::dot(planeN, planeN);
    const float nz = nLenSq > 1e-8f ? std::abs(planeN.z * glm::inversesqrt(nLenSq)) : 0.0f;
    if (stepXLen < 0.2f || stepXLen > 12.0f || stepYLen < 0.2f || stepYLen > 12.0f || nz < 0.60f ||
        stepXLen * surface.width > 450.0f || stepYLen * surface.height > 450.0f) {
        return;
    }
    constexpr float WMO_WATER_Z_OFFSET = -1.0f;
    const float z = surface.origin.z + WMO_WATER_Z_OFFSET;
    if (z > 2000.0f || z < -500.0f) return;
    surface.origin.z = z;
    surface.position = surface.origin;
    surface.minHeight = surface.maxHeight = z;
    const std::size_t vertexCount = static_cast<std::size_t>(surface.width + 1) * static_cast<std::size_t>(surface.height + 1);
    surface.heights.assign(vertexCount, z);
    const std::size_t tileCount = static_cast<std::size_t>(surface.width) * static_cast<std::size_t>(surface.height);
    surface.mask.assign((tileCount + 7) / 8, 0x00);
    for (std::size_t t = 0; t < tileCount; ++t) {
        if (t < liquid.flags.size() && (liquid.flags[t] & 0x0F) == 0x0F) continue;  // no liquid in this tile
        surface.mask[t / 8] |= static_cast<uint8_t>(1u << (t % 8));
    }
    surfaces.push_back(std::move(surface));
    rebuildGlMesh(wmoKey(wmoId), -1, -1, wmoId);
}

void WaterRenderer::removeTile(int tileX, int tileY) {
    const std::size_t before = surfaces.size();
    surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(),
                                  [&](const WaterSurface& s) { return s.wmoId == 0 && s.tileX == tileX && s.tileY == tileY; }),
                   surfaces.end());
    if (surfaces.size() != before) rebuildGlMesh(tileKey(tileX, tileY), tileX, tileY, 0);
}

void WaterRenderer::removeWMO(uint32_t wmoId) {
    const std::size_t before = surfaces.size();
    surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [&](const WaterSurface& s) { return s.wmoId == wmoId; }), surfaces.end());
    if (surfaces.size() != before) rebuildGlMesh(wmoKey(wmoId), -1, -1, wmoId);
}

void WaterRenderer::glRender(const gl::SceneParams& scene, float timeSeconds) {
    if (!glReady() || gl_->meshes.empty()) return;
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(gl_->program);
    const glm::mat4 viewProj = scene.projection * scene.view;
    glUniformMatrix4fv(gl_->viewProj, 1, GL_FALSE, &viewProj[0][0]);
    glUniform4f(gl_->eye, scene.eye.x, scene.eye.y, scene.eye.z, 0.0f);
    glUniform4f(gl_->fog, scene.fogStart, scene.fogEnd, 0.0f, 0.0f);
    glUniform3f(gl_->fogColor, scene.fogColor.x, scene.fogColor.y, scene.fogColor.z);
    glUniform1f(gl_->time, timeSeconds);
    for (const auto& kv : gl_->meshes) {
        const Gl::Mesh& m = kv.second;
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, pos)));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, uv)));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, color)));
        glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_SHORT, nullptr);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// ---- queries (swimming, liquid type): upstream's, over the surfaces kept above --------------------------------------------

std::optional<glm::vec2> WaterRenderer::wateredGridPosition(const WaterSurface& surface, float glX, float glY) const {
    const auto grid = surfaceGridPosition(surface.origin, surface.stepX, surface.stepY, surface.width, surface.height, glX, glY);
    if (!grid) return std::nullopt;
    const int ix = std::min(static_cast<int>(grid->x), static_cast<int>(surface.width) - 1);
    const int iy = std::min(static_cast<int>(grid->y), static_cast<int>(surface.height) - 1);
    if (!waterCellRendered(surface.mask, surface.wmoId, surface.width, surface.xOffset, surface.yOffset, ix, iy)) return std::nullopt;
    return grid;
}

std::optional<float> WaterRenderer::getWaterHeightAt(float glX, float glY) const {
    std::optional<float> best;
    for (const auto& surface : surfaces) {
        const auto grid = wateredGridPosition(surface, glX, glY);
        if (!grid) continue;
        const auto sampled = sampleGridHeight(surface.heights, surface.width, surface.height, grid->x, grid->y);
        if (!sampled) continue;
        if (!best || *sampled > *best) best = *sampled;
    }
    return best;
}

std::optional<float> WaterRenderer::getNearestWaterHeightAt(float glX, float glY, float queryZ, float maxAbove) const {
    std::optional<float> best;
    float bestDist = 1e9f;
    for (const auto& surface : surfaces) {
        const auto grid = wateredGridPosition(surface, glX, glY);
        if (!grid) continue;
        const auto sampled = sampleGridHeight(surface.heights, surface.width, surface.height, grid->x, grid->y);
        if (!sampled) continue;
        const float h = *sampled;
        if (h < queryZ - 2.0f || h > queryZ + maxAbove) continue;
        const float dist = std::abs(h - queryZ);
        if (!best || dist < bestDist) {
            best = h;
            bestDist = dist;
        }
    }
    return best;
}

std::optional<uint16_t> WaterRenderer::getWaterTypeAt(float glX, float glY) const {
    std::optional<float> bestHeight;
    std::optional<uint16_t> bestType;
    for (const auto& surface : surfaces) {
        if (!wateredGridPosition(surface, glX, glY)) continue;
        const float h = surface.minHeight;
        if (!bestHeight || h > *bestHeight) {
            bestHeight = h;
            bestType = surface.liquidType;
        }
    }
    return bestType;
}

bool WaterRenderer::isWmoWaterAt(float glX, float glY) const {
    for (const auto& surface : surfaces) {
        if (surface.wmoId == 0) continue;
        if (surfaceGridPosition(surface.origin, surface.stepX, surface.stepY, surface.width, surface.height, glX, glY)) return true;
    }
    return false;
}

}  // namespace wowee::rendering

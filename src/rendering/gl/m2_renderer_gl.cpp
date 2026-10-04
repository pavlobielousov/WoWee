// Vita skeleton of M2Renderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/m2_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/m2_renderer.hpp"

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
#include <cmath>
#include <unordered_map>

namespace wowee::rendering {

// Static doodads (VITA-19): trees, rocks and props as they stand, without bones, particles, ribbons or texture animation.
struct M2Renderer::Gl {
    struct Uniforms {
        GLint viewProj = -1, model = -1, uvOffset = -1, lightDir = -1, lightColor = -1, ambient = -1, eye = -1, fog = -1,
              lit = -1, texture = -1, fogColor = -1, tint = -1, params = -1;
    };
    struct Vertex {
        float pos[3];
        float normal[3];
        float uv[2];
    };
    struct Batch {
        uint32_t firstIndexBytes = 0;
        uint32_t indexCount = 0;
        GLuint texture = 0;
        gl::M2Kind kind = gl::M2Kind::Opaque;
        uint16_t blendMode = 0;
        bool unlit = false;
        bool twoSided = false;
        glm::vec3 tint{1.0f};
    };
    struct Model {
        GLuint vbo = 0, ibo = 0;
        std::vector<Batch> batches;
        glm::vec3 center{0.0f};
        float radius = 1.0f;
        std::size_t bytes = 0;
    };
    struct Instance {
        uint32_t model = 0;
        glm::mat4 matrix{1.0f};
        glm::vec3 center{0.0f};
        float radius = 1.0f;
    };

    pipeline::AssetManager* assets = nullptr;
    GLuint program[3] = {0, 0, 0};
    Uniforms u[3];
    gl::TextureCache textures;
    std::unordered_map<uint32_t, Model> models;
    std::unordered_map<uint32_t, Instance> instances;
    uint32_t nextInstance = 1;
    bool ready = false;
    std::size_t gpuBytes = 0;
    int drawn = 0, culled = 0;
};

M2Renderer::M2Renderer() = default;

M2Renderer::~M2Renderer() {
    if (gl_ && gl_->ready) shutdown();
}

bool M2Renderer::glReady() const { return gl_ && gl_->ready; }

bool M2Renderer::glInitialize(pipeline::AssetManager* assets) {
    if (glReady()) return true;
    gl_ = std::make_unique<Gl>();
    gl_->assets = assets;
    assetManager = assets;
    gl_->textures.setAssetManager(assets);
    for (int k = 0; k < 3; ++k) {
        gl_->program[k] = gl::linkProgram(gl::m2Program(static_cast<gl::M2Kind>(k)));
        if (gl_->program[k] == 0) {
            LOG_ERROR("M2 (GL): program ", k, " did not link");
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
    gl_->ready = true;
    initialized_ = true;
    LOG_WARNING("M2 renderer (GL) ready");
    return true;
}

void M2Renderer::shutdown() {
    clear();
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

void M2Renderer::clear() {
    if (!gl_) return;
    for (auto& m : gl_->models) {
        if (m.second.vbo) glDeleteBuffers(1, &m.second.vbo);
        if (m.second.ibo) glDeleteBuffers(1, &m.second.ibo);
    }
    gl_->models.clear();
    gl_->instances.clear();
    gl_->gpuBytes = 0;
}

bool M2Renderer::hasModel(uint32_t modelId) const { return gl_ && gl_->models.count(modelId) != 0; }

bool M2Renderer::loadModel(const pipeline::M2Model& model, uint32_t modelId) {
    if (!glReady()) return false;
    if (gl_->models.count(modelId)) return true;
    if (model.vertices.empty() || model.indices.empty()) return false;

    Gl::Model out;
    std::vector<Gl::Vertex> verts(model.vertices.size());
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const pipeline::M2Vertex& v = model.vertices[i];
        verts[i] = Gl::Vertex{{v.position.x, v.position.y, v.position.z}, {v.normal.x, v.normal.y, v.normal.z},
                              {v.texCoords[0].x, v.texCoords[0].y}};
    }
    glGenBuffers(1, &out.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Gl::Vertex)), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &out.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(model.indices.size() * sizeof(uint16_t)), model.indices.data(),
                 GL_STATIC_DRAW);
    out.bytes = verts.size() * sizeof(Gl::Vertex) + model.indices.size() * sizeof(uint16_t);

    out.center = (model.boundMin + model.boundMax) * 0.5f;
    out.radius = std::max(0.1f, glm::length(model.boundMax - model.boundMin) * 0.5f);

    for (const pipeline::M2Batch& b : model.batches) {
        if (b.submeshLevel != 0 || b.indexCount == 0) continue;  // the base mesh only
        if (static_cast<std::size_t>(b.indexStart) + b.indexCount > model.indices.size()) continue;
        Gl::Batch batch;
        batch.firstIndexBytes = b.indexStart * static_cast<uint32_t>(sizeof(uint16_t));
        batch.indexCount = b.indexCount;
        if (b.materialIndex < model.materials.size()) {
            const pipeline::M2Material& m = model.materials[b.materialIndex];
            batch.blendMode = m.blendMode;
            batch.unlit = (m.flags & 0x01) != 0;
            batch.twoSided = (m.flags & 0x04) != 0;
        }
        batch.kind = batch.blendMode == 0 ? gl::M2Kind::Opaque : (batch.blendMode == 1 ? gl::M2Kind::AlphaTest : gl::M2Kind::Blend);
        // Texture: lookup -> texture; a type other than 0 (skin, hair, ...) names no file here.
        GLuint tex = 0;
        if (b.textureIndex < model.textureLookup.size()) {
            const uint16_t t = model.textureLookup[b.textureIndex];
            if (t < model.textures.size() && !model.textures[t].filename.empty()) {
                const std::string& path = model.textures[t].filename;
                if (predecodedBLPCache_) {
                    auto it = predecodedBLPCache_->find(path);
                    if (it != predecodedBLPCache_->end()) gl_->textures.adopt(path, it->second);
                }
                tex = gl_->textures.get(path);
            }
        }
        batch.texture = tex != 0 ? tex : gl_->textures.white();
        out.batches.push_back(batch);
    }
    gl_->gpuBytes += out.bytes;
    gl_->models.emplace(modelId, std::move(out));
    return true;
}

void M2Renderer::unloadModel(uint32_t modelId) {
    if (!gl_) return;
    auto it = gl_->models.find(modelId);
    if (it == gl_->models.end()) return;
    if (it->second.vbo) glDeleteBuffers(1, &it->second.vbo);
    if (it->second.ibo) glDeleteBuffers(1, &it->second.ibo);
    gl_->gpuBytes -= std::min(gl_->gpuBytes, it->second.bytes);
    gl_->models.erase(it);
}

static void boundsOf(const glm::mat4& m, const glm::vec3& localCenter, float localRadius, glm::vec3& center, float& radius) {
    center = glm::vec3(m * glm::vec4(localCenter, 1.0f));
    const float sx = glm::length(glm::vec3(m[0])), sy = glm::length(glm::vec3(m[1])), sz = glm::length(glm::vec3(m[2]));
    radius = localRadius * std::max(sx, std::max(sy, sz));
}

uint32_t M2Renderer::createInstance(uint32_t modelId, const glm::vec3& position, const glm::vec3& rotation, float scale,
                                    [[maybe_unused]] bool allowPositionDedup) {
    if (!glReady() || !std::isfinite(position.x + position.y + position.z + rotation.x + rotation.y + rotation.z + scale) ||
        scale <= 0.0f) {
        return 0;
    }
    return createInstanceWithMatrix(modelId, placementModelMatrix(position, rotation, scale), position);
}

uint32_t M2Renderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix, [[maybe_unused]] const glm::vec3& position) {
    if (!glReady()) return 0;
    auto it = gl_->models.find(modelId);
    if (it == gl_->models.end()) return 0;
    Gl::Instance inst;
    inst.model = modelId;
    inst.matrix = modelMatrix;
    boundsOf(modelMatrix, it->second.center, it->second.radius, inst.center, inst.radius);
    const uint32_t id = gl_->nextInstance++;
    gl_->instances.emplace(id, inst);
    return id;
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    if (gl_) gl_->instances.erase(instanceId);
}

void M2Renderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (!gl_) return;
    for (uint32_t id : instanceIds) gl_->instances.erase(id);
}

void M2Renderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    if (!gl_) return;
    auto it = gl_->instances.find(instanceId);
    if (it == gl_->instances.end()) return;
    auto mit = gl_->models.find(it->second.model);
    if (mit == gl_->models.end()) return;
    it->second.matrix = transform;
    boundsOf(transform, mit->second.center, mit->second.radius, it->second.center, it->second.radius);
}

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    if (!gl_) return;
    auto it = gl_->instances.find(instanceId);
    if (it == gl_->instances.end()) return;
    glm::mat4 m = it->second.matrix;
    m[3] = glm::vec4(position, 1.0f);
    setInstanceTransform(instanceId, m);
}

bool M2Renderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    if (!gl_) return false;
    auto it = gl_->instances.find(instanceId);
    if (it == gl_->instances.end()) return false;
    outCenter = it->second.center;
    outRadius = it->second.radius;
    return true;
}

uint32_t M2Renderer::glInstanceCount() const { return gl_ ? static_cast<uint32_t>(gl_->instances.size()) : 0; }

void M2Renderer::glRender(const gl::SceneParams& scene) {
    if (!glReady() || gl_->instances.empty()) return;
    static gl::FrameStats stats("M2");
    stats.begin();
    Frustum frustum;
    frustum.extractFromMatrix(scene.cullViewProj);
    gl_->drawn = gl_->culled = 0;

    // Visible instances, then opaque ones first (grouped by model), then the blended ones.
    struct Item { const Gl::Instance* inst; const Gl::Model* model; float dist; };
    std::vector<Item> items;
    items.reserve(gl_->instances.size());
    for (const auto& kv : gl_->instances) {
        const Gl::Instance& i = kv.second;
        const float dist = glm::length(i.center - scene.eye);
        // Small things vanish first: 60 yards plus 40 for every yard of radius, never past the fog.
        const float reach = std::min(scene.fogEnd, 60.0f + i.radius * 40.0f);
        if (dist - i.radius > reach || !frustum.intersectsSphere(i.center, i.radius)) {
            ++gl_->culled;
            continue;
        }
        auto m = gl_->models.find(i.model);
        if (m == gl_->models.end()) continue;
        items.push_back({&i, &m->second, dist});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.model < b.model; });

    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    // GL keeps uniforms per program, so the per-frame ones are set once per program per frame, however often the draws
    // switch between programs (a tree alternates opaque trunk and alpha-tested leaves); the per-batch ones only when they
    // change. Measured before: 24 ms of CPU for 360 draws, almost all of it uniform calls.
    int current = -1;
    bool frameSet[3] = {false, false, false};
    struct Last { float lit = -1, tint0 = -1, p0 = -1, p3 = -1; const glm::mat4* model = nullptr; } last[3];
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
        last[k] = Last{};
        glActiveTexture(GL_TEXTURE0);
    };

    for (int pass = 0; pass < 2; ++pass) {  // 0: opaque and alpha-tested, 1: blended
        if (pass == 0) { glDepthMask(GL_TRUE); glDisable(GL_BLEND); }
        else { glDepthMask(GL_FALSE); glEnable(GL_BLEND); }
        const Gl::Model* bound = nullptr;
        GLuint boundTexture = 0;
        for (const Item& it : items) {
            for (const Gl::Batch& b : it.model->batches) {
                const bool blended = b.kind == gl::M2Kind::Blend;
                if (blended != (pass == 1)) continue;
                if (bound != it.model) {
                    bound = it.model;
                    glBindBuffer(GL_ARRAY_BUFFER, it.model->vbo);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it.model->ibo);
                    glEnableVertexAttribArray(0);
                    glEnableVertexAttribArray(1);
                    glEnableVertexAttribArray(2);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, pos)));
                    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, normal)));
                    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Gl::Vertex), reinterpret_cast<const void*>(offsetof(Gl::Vertex, uv)));
                }
                const int k = static_cast<int>(b.kind);
                useProgram(k);
                const Gl::Uniforms& u = gl_->u[k];
                if (blended) {
                    switch (b.blendMode) {
                        case 3: case 6: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
                        case 4: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
                        case 5: glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;
                        default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
                    }
                }
                Last& l = last[k];
                if (l.model != &it.inst->matrix) {
                    l.model = &it.inst->matrix;
                    glUniformMatrix4fv(u.model, 1, GL_FALSE, &it.inst->matrix[0][0]);
                }
                const float lit = b.unlit ? 0.0f : 1.0f;
                if (l.lit != lit) {
                    l.lit = lit;
                    glUniform2f(u.lit, lit, 1.0f);
                }
                // x alpha cutoff, y colour key, z fade, w fog to colour (additive and multiply fade to black)
                const float fogToColour = (b.blendMode == 3 || b.blendMode == 4 || b.blendMode == 6) ? 0.0f : 1.0f;
                if (l.p3 != fogToColour) {
                    l.p3 = fogToColour;
                    glUniform4f(u.params, 0.5f, 0.0f, 1.0f, fogToColour);
                }
                if (boundTexture != b.texture) {
                    boundTexture = b.texture;
                    glBindTexture(GL_TEXTURE_2D, b.texture);
                }
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(b.indexCount), GL_UNSIGNED_SHORT,
                               reinterpret_cast<const void*>(static_cast<uintptr_t>(b.firstIndexBytes)));
                ++gl_->drawn;
            }
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    stats.end(gl_->drawn, static_cast<long>(items.size()));
    if (stats.frames == 0) {
        LOG_WARNING("GL memory M2: textures ", gl_->textures.bytes() / 1024, " KB in ", gl_->textures.count(), ", buffers ", gl_->gpuBytes / 1024,
                    " KB, models ", gl_->models.size(), ", instances ", gl_->instances.size());
    }
}


void M2Renderer::clearInstanceHighlights() { }



float M2Renderer::getInstanceAnimDuration([[maybe_unused]] uint32_t instanceId) const { return 0; }


bool M2Renderer::hasAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId) const { return false; }



void M2Renderer::markModelAsSpellEffect([[maybe_unused]] uint32_t modelId) { }


void M2Renderer::restartInstanceAnimation([[maybe_unused]] uint32_t instanceId) { }

void M2Renderer::setInstanceAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool loop) { }

void M2Renderer::setInstanceAnimationFrozen([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool frozen) { }

void M2Renderer::setInstanceAnimationHeld([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool skipToEnd) { }

void M2Renderer::setInstanceHighlight([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] float amount) { }

void M2Renderer::setInstanceIsGameObject([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool isGameObject) { }



void M2Renderer::setModelPinned([[maybe_unused]] uint32_t modelId, [[maybe_unused]] bool pinned) { }

void M2Renderer::setSkipCollision([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool skip) { }

void M2Renderer::setSkipWallCollision([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool skip) { }

std::optional<uint32_t> M2Renderer::soleSequenceId([[maybe_unused]] uint32_t instanceId) const { return {}; }

std::vector<uint32_t> M2Renderer::drainReapedModelIds() { return {}; }


}  // namespace wowee::rendering

// See shader_selftest.hpp (VITA-14).
#include "rendering/gl/shader_selftest.hpp"

#include "core/logger.hpp"
#include "rendering/gl/gl_program.hpp"
#include "rendering/gl/shader_sources.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vitaGL.h>

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace wowee::rendering::gl {

namespace {

constexpr int kW = 960;
constexpr int kH = 544;

int g_failures = 0;
int g_checks = 0;

void check(const char* name, bool ok, const std::string& detail = "") {
    ++g_checks;
    if (!ok) ++g_failures;
    LOG_WARNING("GLTEST ", ok ? "PASS " : "FAIL ", name, detail.empty() ? "" : " : ", detail);
}

double nowMs() { return static_cast<double>(sceKernelGetProcessTimeWide()) / 1000.0; }

struct Rgb { int r, g, b; };

Rgb readPixel(int x, int yFromTop, int height = kH) {
    unsigned char p[4] = {};
    glReadPixels(x, height - 1 - yFromTop, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return {p[0], p[1], p[2]};
}

bool near(Rgb got, Rgb want, int tol = 12) {
    return std::abs(got.r - want.r) <= tol && std::abs(got.g - want.g) <= tol && std::abs(got.b - want.b) <= tol;
}

std::string fmt(Rgb got, Rgb want) {
    char b[96];
    std::snprintf(b, sizeof b, "got (%d,%d,%d) want about (%d,%d,%d)", got.r, got.g, got.b, want.r, want.g, want.b);
    return b;
}

// ---- textures -----------------------------------------------------------------------------------------------------

uint16_t rgb565(int r, int g, int b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// One 4x4 block of a single colour: DXT1 (8 bytes) or DXT5 (alpha block 255 + colour block).
void dxtBlock(uint8_t* out, bool dxt5, Rgb c) {
    if (dxt5) {
        out[0] = 255; out[1] = 255;
        std::memset(out + 2, 0, 6);
        out += 8;
    }
    const uint16_t c0 = rgb565(c.r, c.g, c.b);
    out[0] = c0 & 0xFF; out[1] = c0 >> 8;
    out[2] = 0; out[3] = 0;  // colour1 = 0 (black): c0 > c1, so four-colour mode, index 0 = c0
    std::memset(out + 4, 0, 4);
}

/// A solid-colour compressed texture with a full mip chain (64x64 down to 1x1), the pattern terrain really uses.
GLuint solidDxt(bool dxt5, Rgb c, int smallest = 1, GLint minFilter = GL_LINEAR_MIPMAP_LINEAR, bool finishEach = false,
                bool reverse = false, bool paramsAfter = true) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    auto setParams = [&] {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    };
    // vitaGL applies the mip count only when MIN_FILTER is set on a texture that already has its data, so the order
    // matters (found by the probe below): parameters AFTER every level is uploaded.
    if (!paramsAfter) setParams();
    const int blockBytes = dxt5 ? 16 : 8;
    std::vector<int> sizes;
    for (int size = 64; size >= smallest; size /= 2) sizes.push_back(size);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::size_t level = reverse ? sizes.size() - 1 - i : i;
        const int size = sizes[level];
        const int blocks = ((size + 3) / 4) * ((size + 3) / 4);
        std::vector<uint8_t> data(static_cast<std::size_t>(blocks * blockBytes));
        for (int k = 0; k < blocks; ++k) dxtBlock(data.data() + k * blockBytes, dxt5, c);
        glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                               dxt5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, size, size, 0,
                               blocks * blockBytes, data.data());
        if (const GLenum e = glGetError(); e != GL_NO_ERROR) LOG_WARNING("GLTEST glGetError 0x", std::hex, e, std::dec, " uploading ", size, "x", size, dxt5 ? " DXT5" : " DXT1");
        if (finishEach) sceGxmTransferFinish();  // vitaGL copies DXT3/DXT5 levels with an asynchronous hardware transfer
    }
    if (paramsAfter) setParams();
    return tex;
}

GLuint rgba2x2(const uint8_t* px, GLint filter, GLint wrap) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return tex;
}

GLuint solidRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t px[16] = {r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a};
    return rgba2x2(px, GL_NEAREST, GL_CLAMP_TO_EDGE);
}

void bindUnit(int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

// ---- a program in use ---------------------------------------------------------------------------------------------

struct Prog {
    GLuint id = 0;
    GLint loc(const char* name) const { return glGetUniformLocation(id, name); }
    void use() const { glUseProgram(id); }
    void mat(const char* n, const glm::mat4& m) const { glUniformMatrix4fv(loc(n), 1, GL_FALSE, glm::value_ptr(m)); }
    void v4(const char* n, float a, float b, float c, float d) const { glUniform4f(loc(n), a, b, c, d); }
    void v3(const char* n, float a, float b, float c) const { glUniform3f(loc(n), a, b, c); }
    void v2(const char* n, float a, float b) const { glUniform2f(loc(n), a, b); }
    void i1(const char* n, int a) const { glUniform1i(loc(n), a); }
};

struct Light {
    glm::vec3 ambient{1.0f};
    glm::vec3 color{0.0f};
    glm::vec3 fogColor{0.1f, 0.8f, 0.2f};
    float fogStart = 1.0e8f, fogEnd = 2.0e8f;  // fog off unless a test turns it on
};

void setCommon(const Prog& p, const glm::mat4& viewProj, const glm::vec3& eye, const Light& l) {
    p.mat("uViewProj", viewProj);
    p.v4("uLightDir", 0.0f, 0.0f, -1.0f, 0.0f);
    p.v3("uLightColor", l.color.x, l.color.y, l.color.z);
    p.v3("uAmbient", l.ambient.x, l.ambient.y, l.ambient.z);
    p.v4("uEye", eye.x, eye.y, eye.z, 1.0f);
    p.v4("uFog", l.fogStart, l.fogEnd, 0.0f, 0.0f);
    p.v3("uFogColor", l.fogColor.x, l.fogColor.y, l.fogColor.z);
}

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float layerUv[2];
};

/// Draw a quad given in clip/world space with the attribute layout both programs share (terrain adds LayerUV).
void drawQuad(bool terrain, const glm::vec3& a, const glm::vec3& b, float uvScale) {
    const Vertex v[4] = {
        {{a.x, a.y, a.z}, {0, 0, 1}, {0, 0}, {0, 0}},
        {{b.x, a.y, a.z}, {0, 0, 1}, {uvScale, 0}, {1, 0}},
        {{b.x, b.y, b.z}, {0, 0, 1}, {uvScale, uvScale}, {1, 1}},
        {{a.x, b.y, b.z}, {0, 0, 1}, {0, uvScale}, {0, 1}},
    };
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), v[0].pos);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), v[0].normal);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), v[0].uv);
    if (terrain) {
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), v[0].layerUv);
    }
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    if (terrain) glDisableVertexAttribArray(3);
}

const glm::mat4 kIdentity(1.0f);

void clearTo(float r, float g, float b) {
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

struct Textures {
    GLuint base, layer1, layer2, layer3, alpha;
    GLuint orange, cutout, halfAlpha;
};

Textures makeTextures() {
    Textures t{};
    // Base DXT1 (mipped), layer 1 DXT5, layer 2 DXT1, layer 3 DXT5: the real pattern, and the one that tripped
    // vitaGL's DXT5-after-mipped-DXT1 quirk in GlProbe.
    t.base = solidDxt(false, {255, 0, 0});
    t.layer1 = solidDxt(true, {0, 0, 255});
    t.layer2 = solidDxt(false, {0, 255, 0});
    t.layer3 = solidDxt(true, {255, 255, 0});
    const uint8_t a[16] = {128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255};
    t.alpha = rgba2x2(a, GL_LINEAR, GL_CLAMP_TO_EDGE);
    t.orange = solidRgba(255, 128, 0, 255);
    // 2x2: left column transparent, right column opaque (nearest, so the halves stay sharp)
    const uint8_t cut[16] = {255, 128, 0, 0, 255, 128, 0, 255, 255, 128, 0, 0, 255, 128, 0, 255};
    t.cutout = rgba2x2(cut, GL_NEAREST, GL_CLAMP_TO_EDGE);
    t.halfAlpha = solidRgba(255, 128, 0, 128);
    return t;
}

void bindTerrainTextures(const Prog& p, const Textures& t, int layers) {
    bindUnit(0, t.base);
    p.i1("uBase", 0);
    if (layers >= 1) {
        bindUnit(1, t.alpha); p.i1("uAlpha", 1);
        bindUnit(2, t.layer1); p.i1("uLayer1", 2);
    }
    if (layers >= 2) { bindUnit(3, t.layer2); p.i1("uLayer2", 3); }
    if (layers >= 3) { bindUnit(4, t.layer3); p.i1("uLayer3", 4); }
}

void setM2(const Prog& p, float cutoff, float colorKey, float fade, float fogToColor, bool lit = false) {
    p.mat("uModel", kIdentity);
    p.v2("uUVOffset", 0.0f, 0.0f);
    p.v2("uLit", lit ? 1.0f : 0.0f, 1.0f);
    p.v3("uTint", 1.0f, 1.0f, 1.0f);
    p.v4("uParams", cutoff, colorKey, fade, fogToColor);
    p.i1("uTexture", 0);
}

// ---- the checks ---------------------------------------------------------------------------------------------------

void logLimits() {
    GLint units = 0, vtx = 0, frag = 0, vary = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vtx);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &frag);
    glGetIntegerv(GL_MAX_VARYING_VECTORS, &vary);
    LOG_WARNING("GLTEST limits: texture units ", units, ", vertex uniform vectors ", vtx, ", fragment uniform vectors ",
                frag, ", varying vectors ", vary);
}

/// Which DXT uploads sample correctly through the terrain program: full mip chain, chain down to 4x4, no mips, DXT1 and DXT5.
void dxtProbe() {
    Prog p;
    p.id = linkProgram(terrainProgram(0));
    if (!p.id) return;
    const glm::vec3 lo(-1.0f, -1.0f, 0.0f), hi(1.0f, 1.0f, 0.0f);
    struct Variant { const char* name; bool dxt5; int smallest; GLint filter; bool finishEach; bool reverse; bool paramsAfter; };
    const Variant variants[] = {
        {"DXT1 full chain, params BEFORE upload", false, 1, GL_LINEAR_MIPMAP_LINEAR, false, false, false},
        {"DXT1 full chain, params AFTER upload", false, 1, GL_LINEAR_MIPMAP_LINEAR, false, false, true},
        {"DXT5 full chain, params BEFORE upload", true, 1, GL_LINEAR_MIPMAP_LINEAR, false, false, false},
        {"DXT5 full chain, params AFTER upload", true, 1, GL_LINEAR_MIPMAP_LINEAR, false, false, true},
        {"DXT5 full chain, params AFTER upload (again)", true, 1, GL_LINEAR_MIPMAP_LINEAR, false, false, true},
        {"DXT5 level 0 only, params AFTER upload", true, 64, GL_LINEAR, false, false, true},
        {"DXT5 chain 64..8, params AFTER upload", true, 8, GL_LINEAR_MIPMAP_LINEAR, false, false, true},
        {"DXT5 full chain, params AFTER upload, nearest mip", true, 1, GL_NEAREST_MIPMAP_NEAREST, false, false, true},
        {"DXT5 full chain, params BEFORE upload (again)", true, 1, GL_LINEAR_MIPMAP_LINEAR, false, false, false},
    };
    Light light;
    for (const Variant& v : variants) {
        const GLuint tex = solidDxt(v.dxt5, {255, 0, 0}, v.smallest, v.filter, v.finishEach, v.reverse, v.paramsAfter);
        std::string seq;
        for (int draw = 0; draw < 3; ++draw) {
            clearTo(0, 0, 0);
            glDisable(GL_BLEND);
            p.use();
            setCommon(p, kIdentity, glm::vec3(0.0f), light);
            p.mat("uModel", kIdentity);
            bindUnit(0, tex);
            p.i1("uBase", 0);
            drawQuad(true, lo, hi, 8.0f);
            const Rgb got = readPixel(480, 272);
            seq += near(got, {255, 0, 0}) ? 'P' : 'F';
        }
        // a P after an F means the same texture, drawn again, sampled correctly: state around the draw, not the upload
        check(v.name, seq == "PPP", "texture " + std::to_string(tex) + ", three draws: " + seq);
        glDeleteTextures(1, &tex);
    }
    glDeleteProgram(p.id);
}

void pixelChecks(const Textures& t) {
    glViewport(0, 0, kW, kH);
    const glm::vec3 lo(-1.0f, -1.0f, 0.0f), hi(1.0f, 1.0f, 0.0f);
    const glm::vec3 eye(0.0f);

    // terrain: base and each layer count (ambient 1, light 0, fog off: the texture colours come out as they are)
    Prog terrain[4];
    for (int n = 0; n < 4; ++n) terrain[n].id = linkProgram(terrainProgram(n));
    const Rgb want[4] = {{255, 0, 0}, {127, 0, 128}, {63, 128, 64}, {159, 191, 32}};
    const char* names[4] = {"terrain base only", "terrain 1 layer at 50%", "terrain 2 layers", "terrain 3 layers"};
    Light light;
    for (int n = 0; n < 4; ++n) {
        if (terrain[n].id == 0) { check(names[n], false, "program did not link"); continue; }
        clearTo(0, 0, 0);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        terrain[n].use();
        setCommon(terrain[n], kIdentity, eye, light);
        terrain[n].mat("uModel", kIdentity);
        bindTerrainTextures(terrain[n], t, n);
        drawQuad(true, lo, hi, 8.0f);
        const Rgb got = readPixel(480, 272);
        check(names[n], near(got, want[n]), fmt(got, want[n]));
    }

    // terrain lighting: ambient 0.2 + Lambert 1.0 * 0.3 = 0.5 of the base colour
    if (terrain[0].id) {
        Light lit;
        lit.ambient = glm::vec3(0.2f);
        lit.color = glm::vec3(0.3f);
        clearTo(0, 0, 0);
        terrain[0].use();
        setCommon(terrain[0], kIdentity, eye, lit);
        terrain[0].mat("uModel", kIdentity);
        bindTerrainTextures(terrain[0], t, 0);
        drawQuad(true, lo, hi, 8.0f);
        const Rgb got = readPixel(480, 272);
        check("terrain per-vertex lighting (ambient 0.2 + 1.0 x 0.3)", near(got, {127, 0, 0}), fmt(got, {127, 0, 0}));

        // fog fully on: the fog colour
        Light fog;
        fog.fogStart = -100.0f;
        fog.fogEnd = -50.0f;
        clearTo(0, 0, 0);
        setCommon(terrain[0], kIdentity, eye, fog);
        drawQuad(true, lo, hi, 8.0f);
        const Rgb f = readPixel(480, 272);
        check("terrain fog fully on gives the fog colour", near(f, {26, 204, 51}), fmt(f, {26, 204, 51}));
    }

    // M2 programs
    Prog m2[3];
    m2[0].id = linkProgram(m2Program(M2Kind::Opaque));
    m2[1].id = linkProgram(m2Program(M2Kind::AlphaTest));
    m2[2].id = linkProgram(m2Program(M2Kind::Blend));
    if (!m2[0].id || !m2[1].id || !m2[2].id) {
        check("M2 programs link", false, "a program did not link");
        return;
    }
    // opaque, unlit: the texture as it is
    clearTo(0, 0, 1);
    m2[0].use();
    setCommon(m2[0], kIdentity, eye, light);
    setM2(m2[0], 0.5f, 0.0f, 1.0f, 1.0f);
    bindUnit(0, t.orange);
    drawQuad(false, lo, hi, 1.0f);
    Rgb got = readPixel(480, 272);
    check("M2 opaque", near(got, {255, 128, 0}), fmt(got, {255, 128, 0}));

    // depth test: the near red quad drawn first must survive the far blue one drawn after it
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    clearTo(0, 0, 0);
    bindUnit(0, t.orange);
    drawQuad(false, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, 1.0f);  // near, orange
    GLuint blue = solidRgba(0, 0, 255, 255);
    bindUnit(0, blue);
    drawQuad(false, {-1.0f, -1.0f, 0.5f}, {1.0f, 1.0f, 0.5f}, 1.0f);    // far, blue, drawn later
    got = readPixel(480, 272);
    const Rgb outside = readPixel(100, 100);
    check("depth test: a near quad hides a far one drawn later", near(got, {255, 128, 0}) && near(outside, {0, 0, 255}),
          fmt(got, {255, 128, 0}) + "; outside " + fmt(outside, {0, 0, 255}));
    glDisable(GL_DEPTH_TEST);

    // alpha test: the transparent half shows the background, the opaque half the texture
    clearTo(0, 0, 1);
    m2[1].use();
    setCommon(m2[1], kIdentity, eye, light);
    setM2(m2[1], 0.5f, 0.0f, 1.0f, 1.0f);
    bindUnit(0, t.cutout);
    drawQuad(false, lo, hi, 1.0f);
    const Rgb left = readPixel(240, 272), right = readPixel(720, 272);
    check("M2 alpha test: transparent half discarded, opaque half drawn",
          near(left, {0, 0, 255}) && near(right, {255, 128, 0}), fmt(left, {0, 0, 255}) + "; right " + fmt(right, {255, 128, 0}));

    // blend over a known background: alpha 128/255
    clearTo(0, 0, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m2[2].use();
    setCommon(m2[2], kIdentity, eye, light);
    setM2(m2[2], 0.5f, 0.0f, 1.0f, 1.0f);
    bindUnit(0, t.halfAlpha);
    drawQuad(false, lo, hi, 1.0f);
    got = readPixel(480, 272);
    check("M2 blend over a blue background", near(got, {128, 64, 127}), fmt(got, {128, 64, 127}));

    // additive with fog to black: fog fully on makes it invisible
    Light fog;
    fog.fogStart = -100.0f;
    fog.fogEnd = -50.0f;
    clearTo(0, 0, 1);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    setCommon(m2[2], kIdentity, eye, fog);
    setM2(m2[2], 0.5f, 0.0f, 1.0f, 0.0f);
    drawQuad(false, lo, hi, 1.0f);
    got = readPixel(480, 272);
    check("M2 additive with fog to black leaves the background", near(got, {0, 0, 255}), fmt(got, {0, 0, 255}));
    glDisable(GL_BLEND);

    for (Prog& p : terrain) glDeleteProgram(p.id);
    for (Prog& p : m2) glDeleteProgram(p.id);
}

// ---- measurements -------------------------------------------------------------------------------------------------

/// Swap-paced fill rate: frames of K full-screen passes presented with vglSwapBuffers. glFinish does not wait for the GPU
/// in vitaGL (a full-screen pass "took" 5 microseconds), so the time is the frame time: under one vsync (16.7 ms) the GPU
/// cost per pass is only bounded, so the pass count is raised (64, then 512) until the frame is past the vsync.
void fillRate(const char* label, const Prog& p, bool terrain, const Textures& t, int layers, int w, int h, GLuint fbo) {
    // Additive blending: the GPU is tile-based and removes hidden opaque surfaces, so stacked opaque passes would cost
    // almost nothing; a blended pass has to be shaded, which is what is being measured (blend cost included).
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDisable(GL_DEPTH_TEST);
    p.use();
    Light light;
    setCommon(p, kIdentity, glm::vec3(0.0f), light);
    if (terrain) {
        p.mat("uModel", kIdentity);
        bindTerrainTextures(p, t, layers);
    } else {
        setM2(p, 0.5f, 0.0f, 1.0f, 1.0f);
        bindUnit(0, t.orange);
    }
    auto run = [&](int passes, int frames) {
        double started = 0;
        for (int frame = 0; frame < frames + 2; ++frame) {
            if (frame == 2) started = nowMs();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, w, h);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            for (int k = 0; k < passes; ++k) drawQuad(terrain, {-1, -1, 0}, {1, 1, 0}, 8.0f);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, kW, kH);
            vglSwapBuffers(GL_FALSE);
        }
        return (nowMs() - started) / frames;
    };
    int passes = 64;
    double frameMs = run(passes, 10);
    if (frameMs < 17.5) {
        passes = 512;
        frameMs = run(passes, 6);
    }
    glDisable(GL_BLEND);
    if (frameMs < 17.5) {
        LOG_WARNING("GLTEST fill ", w, "x", h, " ", label, ": ", frameMs, " ms per frame of ", passes,
                    " passes: still vsync-bound, under ", frameMs / passes, " ms per pass");
    } else {
        LOG_WARNING("GLTEST fill ", w, "x", h, " ", label, ": ", frameMs, " ms per frame of ", passes, " passes = ",
                    frameMs / passes, " ms per pass (", static_cast<double>(w) * h / 1.0e3 / (frameMs / passes),
                    " Mpix/s)");
    }
}

void measure(const Textures& t) {
    Prog terrain[4];
    for (int n = 0; n < 4; ++n) terrain[n].id = linkProgram(terrainProgram(n));
    Prog m2[3];
    m2[0].id = linkProgram(m2Program(M2Kind::Opaque));
    m2[1].id = linkProgram(m2Program(M2Kind::AlphaTest));
    m2[2].id = linkProgram(m2Program(M2Kind::Blend));

    // 640x368 scene target (ADR-001 decision 6)
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 640, 368, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool fboOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    static const char* terrainNames[4] = {"terrain base", "terrain 1 layer", "terrain 2 layers", "terrain 3 layers"};
    for (int pass = 0; pass < 2; ++pass) {
        const int w = pass == 0 ? kW : 640, h = pass == 0 ? kH : 368;
        if (pass == 1 && !fboOk) break;
        const GLuint target = pass == 0 ? 0 : fbo;
        for (int n = 0; n < 4; ++n) if (terrain[n].id) fillRate(terrainNames[n], terrain[n], true, t, n, w, h, target);
        static const char* m2Names[3] = {"M2 opaque", "M2 alpha test", "M2 blend (opaque pixels)"};
        for (int n = 0; n < 3; ++n) if (m2[n].id) fillRate(m2Names[n], m2[n], false, t, 0, w, h, target);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kW, kH);

    // CPU cost of a draw on the programmable path with a uModel change every time (ADR-001 open question 2)
    if (terrain[2].id) {
        terrain[2].use();
        Light light;
        setCommon(terrain[2], kIdentity, glm::vec3(0.0f), light);
        bindTerrainTextures(terrain[2], t, 2);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 1, 1);  // one pixel: only submission is measured
        const int draws = 1000;
        glFinish();
        const double t0 = nowMs();
        for (int i = 0; i < draws; ++i) {
            terrain[2].mat("uModel", glm::translate(kIdentity, glm::vec3(0.0001f * static_cast<float>(i), 0.0f, 0.0f)));
            drawQuad(true, {-1, -1, 0}, {1, 1, 0}, 8.0f);
        }
        const double submit = nowMs() - t0;
        glFinish();
        glDisable(GL_SCISSOR_TEST);
        LOG_WARNING("GLTEST draw cost: ", draws, " terrain draws with a new uModel each: ", submit, " ms to submit (",
                    submit * 1000.0 / draws, " us per draw), ", nowMs() - t0, " ms with the GPU");
    }
    for (Prog& p : terrain) glDeleteProgram(p.id);
    for (Prog& p : m2) glDeleteProgram(p.id);
}

int mode() {
    const char* v = std::getenv("WOWEE_GL_SELFTEST");
    return (v && *v == '1') ? 1 : 0;
}

}  // namespace

void runShaderSelfTest() {
    if (mode() == 0) return;
    LOG_WARNING("GLTEST start (WOWEE_GL_SELFTEST=", mode(), ")");
    const double started = nowMs();
    logLimits();
    // WOWEE_GL_TEST_ORDER=1 creates the textures after the probe instead of before it. Both orders must pass: the case
    // that caught the compressed-upload bug in vitaGL (DEV_SETUP section 22).
    const bool lateTextures = std::getenv("WOWEE_GL_TEST_ORDER") && *std::getenv("WOWEE_GL_TEST_ORDER") == '1';
    Textures t{};
    if (!lateTextures) t = makeTextures();
    LOG_WARNING("GLTEST stage: textures created (", lateTextures ? "late" : "early", ")");
    dxtProbe();
    LOG_WARNING("GLTEST stage: probe done");
    if (lateTextures) t = makeTextures();
    pixelChecks(t);
    LOG_WARNING("GLTEST stage: checks done");
    measure(t);
    LOG_WARNING("GLTEST done: ", g_checks, " checks, ", g_failures, " failed, ", nowMs() - started, " ms");
}

}  // namespace wowee::rendering::gl

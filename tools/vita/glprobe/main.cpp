// GlProbe (VITA-13): answers, on Vita3K now and on a real Vita later, the vitaGL questions ADR-001
// leaves open, with PASS/FAIL lines in ux0:data/wowee/glprobe.log. Uses only the fixed-function
// pipeline for the first tests. NOTE (corrected 2026-10-03): vitaGL's fixed-function path still compiles a generated
// shader at run time for every new combination of GL state (and caches it in ux0:data/shader_cache), so it needs
// the shader compiler (libshacccg.suprx) as much as GLSL does, unless the cache already holds the combination.
//
//   1. context and memory pools (vglInitExtended, vglMemFree/Total)
//   2. clear colour read back with glReadPixels
//   3. DXT1 and DXT5 textures uploaded compressed (glCompressedTexImage2D), sampled, read back;
//      DXT1 with a second mip level, to check that mips are uploaded and selected
//   4. the scene at 640x368 in an FBO, scaled to 960x544 by a textured quad (ADR-001 resolution plan)
//   5. ImGui 1.92 (the vendored copy) drawn by a minimal fixed-function renderer written here, because the
//      stock imgui_impl_opengl2 needs desktop GL calls vitaGL lacks (glOrtho, glPushAttrib, glGetTexEnviv,
//      GL_TEXTURE_BINDING_2D); a window drawn and read back
//   6. a frame loop with a frame-time number (an emulator number means nothing; a device number does)
//
// A FAIL is a finding, not necessarily a bug (see the DepCheck convention).
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <vitaGL.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

// EXT_texture_compression_s3tc, spelled out: vitaGL's headers do not pull in GLES2/gl2ext.h.
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR_PATH "ux0:data/wowee"
#define LOG_PATH DIR_PATH "/glprobe.log"

static int g_fd = -1;
static int g_fail = 0;

static void log_line(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf) - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_fd >= 0) sceIoWrite(g_fd, buf, n);
    sceClibPrintf("%s", buf);
}

static void check(const char* what, bool ok, const char* detail) {
    log_line("%s %s %s", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

struct Rgba { int r, g, b, a; };

// glReadPixels takes the origin at the bottom left; the probe thinks top left like the UI does.
static Rgba read_px(int x, int y_from_top, int height) {
    unsigned char p[4] = {0, 0, 0, 0};
    glReadPixels(x, height - 1 - y_from_top, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return {p[0], p[1], p[2], p[3]};
}

static bool near_rgb(Rgba got, int r, int g, int b, int tol) {
    return abs(got.r - r) <= tol && abs(got.g - g) <= tol && abs(got.b - b) <= tol;
}

static void fmt_rgba(char* out, size_t n, Rgba got, int r, int g, int b) {
    snprintf(out, n, "got (%d,%d,%d,%d) want about (%d,%d,%d)", got.r, got.g, got.b, got.a, r, g, b);
}

static void set_ortho(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, static_cast<float>(w), static_cast<float>(h), 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// A textured quad, x/y/w/h in pixels of the current ortho, the whole texture.
static void draw_quad(GLuint tex, float x, float y, float w, float h) {
    const GLfloat v[] = {x, y, x + w, y, x, y + h, x + w, y + h};
    const GLfloat t[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1, 1, 1, 1);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static void fill_rect(float x, float y, float w, float h, float r, float g, float b) {
    const GLfloat v[] = {x, y, x + w, y, x, y + h, x + w, y + h};
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, 1);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void put16(uint8_t* p, uint16_t v) { p[0] = v & 255; p[1] = v >> 8; }

// One DXT colour block: colour0 > colour1 (four-colour mode), every texel index 0 -> colour0.
static void dxt_color_block(uint8_t* p, uint16_t c0) {
    put16(p, c0);
    put16(p + 2, 0x0000);
    memset(p + 4, 0, 4);
}

enum : uint16_t { RED565 = 0xF800, GREEN565 = 0x07E0, BLUE565 = 0x001F, WHITE565 = 0xFFFF, YELLOW565 = 0xFFE0 };

static void test_dxt() {
    char d[160];
    // 8x8 = 2x2 blocks: red, green / blue, white. Level 1 (4x4) = one yellow block.
    uint8_t dxt1[4 * 8 + 8];
    dxt_color_block(dxt1 + 0, RED565);
    dxt_color_block(dxt1 + 8, GREEN565);
    dxt_color_block(dxt1 + 16, BLUE565);
    dxt_color_block(dxt1 + 24, WHITE565);
    dxt_color_block(dxt1 + 32, YELLOW565);

    GLuint t1 = 0;
    glGenTextures(1, &t1);
    glBindTexture(GL_TEXTURE_2D, t1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8, 8, 0, 32, dxt1);
    GLenum e1 = glGetError();
    snprintf(d, sizeof d, "glGetError=0x%x", e1);
    check("DXT1 8x8 uploads compressed", e1 == GL_NO_ERROR, d);

    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    set_ortho(960, 544);
    draw_quad(t1, 100, 100, 64, 64);  // 8x magnification: each block is 32x32 on screen
    struct { int x, y; int r, g, b; const char* name; } probes[] = {
        {116, 116, 255, 0, 0, "DXT1 block red"},
        {148, 116, 0, 255, 0, "DXT1 block green"},
        {116, 148, 0, 0, 255, "DXT1 block blue"},
        {148, 148, 255, 255, 255, "DXT1 block white"},
    };
    for (auto& p : probes) {
        Rgba got = read_px(p.x, p.y, 544);
        fmt_rgba(d, sizeof d, got, p.r, p.g, p.b);
        check(p.name, near_rgb(got, p.r, p.g, p.b, 12), d);
    }

    // mip level 1 of the same texture, selected by drawing it at half size: LOD 1
    glBindTexture(GL_TEXTURE_2D, t1);
    glCompressedTexImage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 4, 4, 0, 8, dxt1 + 32);
    GLenum em = glGetError();
    snprintf(d, sizeof d, "glGetError=0x%x", em);
    check("DXT1 mip level 1 uploads", em == GL_NO_ERROR, d);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_quad(t1, 300, 100, 4, 4);  // 8 texels onto 4 pixels: LOD 1, the yellow mip
    Rgba mip = read_px(301, 101, 544);
    fmt_rgba(d, sizeof d, mip, 255, 255, 0);
    check("DXT1 mip is selected when minified", near_rgb(mip, 255, 255, 0, 20), d);

    // DXT5: 16-byte blocks (8 bytes alpha, 8 bytes colour). Alpha 255 (a0 = a1 = 255, indices 0).
    uint8_t dxt5[16 * 4];
    for (int b = 0; b < 4; b++) {
        uint8_t* p = dxt5 + b * 16;
        p[0] = 255; p[1] = 255; memset(p + 2, 0, 6);
        dxt_color_block(p + 8, b == 0 ? RED565 : b == 1 ? GREEN565 : b == 2 ? BLUE565 : WHITE565);
    }
    GLuint t5 = 0;
    glGenTextures(1, &t5);
    glBindTexture(GL_TEXTURE_2D, t5);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 8, 8, 0, 64, dxt5);
    GLenum e5 = glGetError();
    snprintf(d, sizeof d, "glGetError=0x%x", e5);
    check("DXT5 8x8 uploads compressed", e5 == GL_NO_ERROR, d);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_quad(t5, 100, 100, 64, 64);
    Rgba g5 = read_px(148, 116, 544);
    fmt_rgba(d, sizeof d, g5, 0, 255, 0);
    // Deterministic on the device: this exact order (a DXT5 texture created right after a mipmapped DXT1 texture was
    // drawn) samples as (0,0,0,0), while every DXT5 case in isolation passes (mask 32). A vitaGL state quirk; see
    // docs/vita/DEV_SETUP.md section 19.
    check("DXT5 block green (right after a mipmapped DXT1 draw; see mask 32 for DXT5 alone)", near_rgb(g5, 0, 255, 0, 12), d);

    glDeleteTextures(1, &t1);
    glDeleteTextures(1, &t5);
}

// DXT3/DXT5 variants (diagnostic, mask 32): DXT5 at 4x4, 8x8 and 16x16, DXT3 at 8x8, every block read back.
// Alpha blocks are written as fully opaque; the colour blocks are red, green, blue, white in block order.
static void upload_and_probe_dxt(const char* name, GLenum fmt, int size, bool dxt5, GLenum wrap = GL_CLAMP_TO_EDGE) {
    char d[200];
    const int blocks = (size / 4) * (size / 4);
    const bool dxt1 = (fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
    const int blockBytes = dxt1 ? 8 : 16;  // DXT1 has no alpha block
    uint8_t* data = static_cast<uint8_t*>(calloc(blocks, blockBytes));
    static const uint16_t cols[4] = {RED565, GREEN565, BLUE565, WHITE565};
    for (int b = 0; b < blocks; b++) {
        uint8_t* p = data + b * blockBytes;
        if (dxt1) {
            dxt_color_block(p, cols[b % 4]);
            continue;
        }
        if (dxt5) {
            p[0] = 255; p[1] = 255;                       // alpha0 = alpha1 = 255, every index 0
        } else {
            memset(p, 0xFF, 8);                           // DXT3: 4 bit alpha 15 for all 16 texels
        }
        dxt_color_block(p + 8, cols[b % 4]);
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, size, size, 0, blocks * blockBytes, data);
    const GLenum err = glGetError();
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    set_ortho(960, 544);
    const float px = 8.0f * 4;  // each 4x4 block is 32x32 pixels on screen... for size 8; scaled below
    (void)px;
    const float quad = 64.0f;
    draw_quad(t, 100, 100, quad, quad);
    // sample the centre of the first four blocks in row-major order (a 4x4 texture has just one block)
    const int perRow = size / 4;
    char colours[160] = {0};
    int ok = 0;
    for (int b = 0; b < 4 && b < blocks; b++) {
        const int bx = b % perRow, by = b / perRow;
        const float cx = 100 + (bx + 0.5f) * (quad / perRow);
        const float cy = 100 + (by + 0.5f) * (quad / perRow);
        Rgba got = read_px(static_cast<int>(cx), static_cast<int>(cy), 544);
        char one[40];
        snprintf(one, sizeof one, "(%d,%d,%d,%d) ", got.r, got.g, got.b, got.a);
        strncat(colours, one, sizeof colours - strlen(colours) - 1);
        static const int want[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
        if (near_rgb(got, want[b % 4][0], want[b % 4][1], want[b % 4][2], 12)) ok++;
    }
    snprintf(d, sizeof d, "err=0x%x blocks read %s", err, colours);
    check(name, err == GL_NO_ERROR && ok == (blocks < 4 ? blocks : 4), d);
    glDeleteTextures(1, &t);
    free(data);
}

static void test_dxt_variants() {
    upload_and_probe_dxt("DXT5 4x4 (one block)", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 4, true);
    upload_and_probe_dxt("DXT5 8x8", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 8, true);
    upload_and_probe_dxt("DXT5 16x16", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16, true);
    upload_and_probe_dxt("DXT5 64x64", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 64, true);
    upload_and_probe_dxt("DXT3 8x8", 0x83F2 /* GL_COMPRESSED_RGBA_S3TC_DXT3_EXT */, 8, false);
    upload_and_probe_dxt("DXT3 16x16", 0x83F2, 16, false);
    upload_and_probe_dxt("DXT1 16x16 (control)", GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 16, false);
    // wrap modes: WoW's terrain and most models repeat; the first DXT5 test left the default (GL_REPEAT)
    upload_and_probe_dxt("DXT5 8x8 GL_REPEAT", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 8, true, GL_REPEAT);
    upload_and_probe_dxt("DXT5 64x64 GL_REPEAT", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 64, true, GL_REPEAT);
    upload_and_probe_dxt("DXT3 8x8 GL_REPEAT", 0x83F2, 8, false, GL_REPEAT);
    upload_and_probe_dxt("DXT1 8x8 GL_REPEAT", GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8, false, GL_REPEAT);
    upload_and_probe_dxt("DXT5 8x8 GL_MIRRORED_REPEAT", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 8, true, GL_MIRRORED_REPEAT);
}

// ---- draw-call cost (mask 64, ADR-001 open question b) ------------------------------------------------------
// N small textured quads per frame, five ways. "submit" is the CPU time spent issuing the draws (what a renderer
// has to budget per frame at the CPU clock set in main()); "frame" includes the swap and the vsync wait, so it
// saturates at 16.7 ms and grows only when the CPU or GPU cannot keep up.
//   A: N glDrawArrays from one big vertex array, no state change   B: + glBindTexture between two textures
//   C: + glColor4f each draw (FFP state)                          D: ONE glDrawArrays for all N quads (batched)
//   E: N draws, each re-pointing glVertexPointer/glTexCoordPointer (a different vertex buffer per object)
static void bench_variant(const char* label, int n, char mode, GLuint texA, GLuint texB, const GLfloat* verts,
                          const GLfloat* uvs, const GLfloat* batchedVerts, const GLfloat* batchedUvs) {
    const int frames = 40;
    uint64_t submitUs = 0, totalUs = 0;
    for (int f = 0; f < frames; f++) {
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        set_ortho(960, 544);
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        const uint64_t s0 = sceKernelGetProcessTimeWide();
        if (mode == 'D') {
            glBindTexture(GL_TEXTURE_2D, texA);
            glVertexPointer(2, GL_FLOAT, 0, batchedVerts);
            glTexCoordPointer(2, GL_FLOAT, 0, batchedUvs);
            glDrawArrays(GL_TRIANGLES, 0, 6 * n);
        } else {
            glVertexPointer(2, GL_FLOAT, 0, verts);
            glTexCoordPointer(2, GL_FLOAT, 0, uvs);
            glBindTexture(GL_TEXTURE_2D, texA);
            for (int i = 0; i < n; i++) {
                if (mode == 'B' || mode == 'C') glBindTexture(GL_TEXTURE_2D, (i & 1) ? texB : texA);
                if (mode == 'C') glColor4f((i & 2) ? 1.0f : 0.8f, 1, 1, 1);
                if (mode == 'E') {
                    glVertexPointer(2, GL_FLOAT, 0, verts + i * 8);
                    glTexCoordPointer(2, GL_FLOAT, 0, uvs + i * 8);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                } else {
                    glDrawArrays(GL_TRIANGLE_STRIP, i * 4, 4);
                }
            }
        }
        const uint64_t s1 = sceKernelGetProcessTimeWide();
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_TEXTURE_2D);
        vglSwapBuffers(GL_FALSE);
        const uint64_t t1 = sceKernelGetProcessTimeWide();
        if (f >= 5) {  // the first frames include one-off setup
            submitUs += s1 - s0;
            totalUs += t1 - t0;
        }
    }
    const double n2 = frames - 5;
    log_line("INFO bench %-30s n=%4d submit %6.2f ms (%5.1f us/draw) frame %6.2f ms", label, n,
             submitUs / 1000.0 / n2, submitUs / n2 / n, totalUs / 1000.0 / n2);
}

static void test_draw_cost() {
    static const int kMax = 2000;
    GLfloat* verts = static_cast<GLfloat*>(malloc(sizeof(GLfloat) * 8 * kMax));
    GLfloat* uvs = static_cast<GLfloat*>(malloc(sizeof(GLfloat) * 8 * kMax));
    GLfloat* bverts = static_cast<GLfloat*>(malloc(sizeof(GLfloat) * 12 * kMax));
    GLfloat* buvs = static_cast<GLfloat*>(malloc(sizeof(GLfloat) * 12 * kMax));
    for (int i = 0; i < kMax; i++) {
        const float x = static_cast<float>((i % 150) * 6), y = static_cast<float>(((i / 150) * 6) % 520);
        const GLfloat q[8] = {x, y, x + 5, y, x, y + 5, x + 5, y + 5};
        const GLfloat t[8] = {0, 0, 1, 0, 0, 1, 1, 1};
        memcpy(verts + i * 8, q, sizeof q);
        memcpy(uvs + i * 8, t, sizeof t);
        const int tri[6] = {0, 1, 2, 2, 1, 3};  // the strip as two triangles
        for (int k = 0; k < 6; k++) {
            bverts[i * 12 + k * 2] = q[tri[k] * 2];
            bverts[i * 12 + k * 2 + 1] = q[tri[k] * 2 + 1];
            buvs[i * 12 + k * 2] = t[tri[k] * 2];
            buvs[i * 12 + k * 2 + 1] = t[tri[k] * 2 + 1];
        }
    }
    GLuint tex[2] = {0, 0};
    glGenTextures(2, tex);
    for (int k = 0; k < 2; k++) {
        uint8_t px[8 * 8 * 4];
        for (int i = 0; i < 64; i++) {
            px[i * 4] = k ? 255 : 80;
            px[i * 4 + 1] = 160;
            px[i * 4 + 2] = k ? 80 : 255;
            px[i * 4 + 3] = 255;
        }
        glBindTexture(GL_TEXTURE_2D, tex[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    log_line("INFO bench ARM clock %d MHz, GPU %d MHz, bus %d MHz", scePowerGetArmClockFrequency(),
             scePowerGetGpuClockFrequency(), scePowerGetBusClockFrequency());
    static const int counts[] = {100, 500, 1000, 2000};
    for (int n : counts) {
        bench_variant("A draws only", n, 'A', tex[0], tex[1], verts, uvs, bverts, buvs);
        bench_variant("B + texture switch", n, 'B', tex[0], tex[1], verts, uvs, bverts, buvs);
        bench_variant("C + texture switch + colour", n, 'C', tex[0], tex[1], verts, uvs, bverts, buvs);
        bench_variant("E + vertex pointers each draw", n, 'E', tex[0], tex[1], verts, uvs, bverts, buvs);
        bench_variant("D one batched draw", n, 'D', tex[0], tex[1], verts, uvs, bverts, buvs);
    }
    glDeleteTextures(2, tex);
    free(verts);
    free(uvs);
    free(bverts);
    free(buvs);
}

// vitaGL has no glDetachShader, and the stock imgui_impl_opengl3 calls it (twice, after linking its program). Detaching
// only matters for freeing the shader objects early; the backend deletes them right after, so a no-op is enough. A Vita
// ImGui backend must carry this shim (VITA-13 finding).
extern "C" void glDetachShader(GLuint, GLuint) {}

// ---- the shader path (mask 128, ADR-001 open question 1, VITA-13 task e) ------------------------------------------
// GLSL ES 1.00 compiled at run time by vitaGL's translator (vitashark + libshacccg.suprx). Times the compile and link
// of a representative terrain shader (a base texture, a second layer blended by an alpha map, directional light, fog),
// draws with it and reads a pixel back, then runs the STOCK imgui_impl_opengl3 (GLES2 profile) backend.
static const char* kTerrainVS =
    "precision highp float;\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "uniform vec3 uLightDir;\n"
    "varying vec2 vUV;\n"
    "varying float vLight;\n"
    "varying float vFog;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vUV = aUV;\n"
    "    vLight = max(dot(vec3(0.0, 0.0, 1.0), uLightDir), 0.2);\n"
    "    vFog = clamp(gl_Position.w / 500.0, 0.0, 1.0);\n"
    "}\n";
static const char* kTerrainFS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "varying float vLight;\n"
    "varying float vFog;\n"
    "uniform sampler2D uBase;\n"
    "uniform sampler2D uLayer1;\n"
    "uniform sampler2D uAlpha1;\n"
    "uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 base = texture2D(uBase, vUV * 8.0);\n"
    "    vec4 l1 = texture2D(uLayer1, vUV * 8.0);\n"
    "    float a = texture2D(uAlpha1, vUV).r;\n"
    "    vec3 c = mix(base.rgb, l1.rgb, a) * vLight;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";
static const char* kM2VS =
    "precision highp float;\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "varying vec2 vUV;\n"
    "void main() { gl_Position = uMVP * vec4(aPos, 1.0); vUV = aUV; }\n";
static const char* kM2FS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uColor;\n"
    "void main() { vec4 t = texture2D(uTex, vUV); if (t.a < 0.5) discard; gl_FragColor = t * uColor; }\n";

static GLuint compile_shader(GLenum type, const char* src, double* ms, const char* label) {
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    *ms = (sceKernelGetProcessTimeWide() - t0) / 1000.0;
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[200] = {0};
        glGetShaderInfoLog(sh, sizeof info - 1, nullptr, info);
        log_line("INFO %s compile log: %s", label, info);
    }
    return ok ? sh : 0;
}

static GLuint build_program(const char* name, const char* vs, const char* fs, double* total) {
    double msV = 0, msF = 0;
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs, &msV, name);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, &msF, name);
    if (!v || !f) { *total = msV + msF; return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    glLinkProgram(prog);
    const double msL = (sceKernelGetProcessTimeWide() - t0) / 1000.0;
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    *total = msV + msF + msL;
    log_line("INFO shader %-18s vertex %7.1f ms, fragment %7.1f ms, link %7.1f ms (total %7.1f ms)", name, msV, msF,
             msL, *total);
    return ok ? prog : 0;
}

static GLuint solid_texture(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t px[2 * 2 * 4];
    for (int i = 0; i < 4; i++) { px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255; }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

// Draws a full-screen quad with the terrain program (base red, layer blue, alpha map 0.5, light 1, fog about 0.002)
// and checks the pixel: about (128, 0, 128).
static bool draw_terrain_and_check(GLuint terrain, char* detail, size_t detailSize) {
    GLuint base = solid_texture(255, 0, 0), l1 = solid_texture(0, 0, 255), alpha = solid_texture(128, 128, 128);
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, 960, 544);
    glUseProgram(terrain);
    const GLfloat id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(terrain, "uMVP"), 1, GL_FALSE, id);
    glUniform3f(glGetUniformLocation(terrain, "uLightDir"), 0, 0, 1);
    glUniform3f(glGetUniformLocation(terrain, "uFogColor"), 0.5f, 0.5f, 0.5f);
    const GLuint tex[3] = {base, l1, alpha};
    static const char* names[3] = {"uBase", "uLayer1", "uAlpha1"};
    for (int i = 0; i < 3; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glUniform1i(glGetUniformLocation(terrain, names[i]), i);
    }
    const GLfloat pos[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    const GLfloat uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glActiveTexture(GL_TEXTURE0);
    Rgba got = read_px(480, 272, 544);
    fmt_rgba(detail, detailSize, got, 128, 0, 128);
    glUseProgram(0);
    glDeleteTextures(1, &base);
    glDeleteTextures(1, &l1);
    glDeleteTextures(1, &alpha);
    return near_rgb(got, 128, 0, 128, 24);
}

static void test_shaders() {
    char d[200];
    double msTerrain = 0, msM2 = 0, msAgain = 0;
    GLuint terrain = build_program("terrain (2 layers)", kTerrainVS, kTerrainFS, &msTerrain);
    check("terrain shader compiles and links (GLSL ES 1.00, run-time translator)", terrain != 0, nullptr);
    GLuint m2 = build_program("m2 (alpha test)", kM2VS, kM2FS, &msM2);
    check("M2-style shader (discard, uniforms) compiles and links", m2 != 0, nullptr);
    GLuint again = build_program("terrain (2nd time)", kTerrainVS, kTerrainFS, &msAgain);
    snprintf(d, sizeof d, "first %.1f ms, second %.1f ms", msTerrain, msAgain);
    check("same source compiled again", again != 0, d);

    if (terrain) {
        const bool ok = draw_terrain_and_check(terrain, d, sizeof d);
        check("terrain shader draws: mix(red, blue, 0.5 alpha map) with light and fog", ok, d);
    }

    // The stock GLES2 ImGui backend: it builds its own shaders at the first frame.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(960, 544);
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    const bool ok = ImGui_ImplOpenGL3_Init("#version 100");
    check("stock imgui_impl_opengl3 (GLES2) initialises", ok, nullptr);
    if (ok) {
        glClearColor(0.2f, 0.4f, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(300, 200));
        ImGui::Begin("GlProbe ES2", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("stock GLES2 backend");
        ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        log_line("INFO stock ImGui GLES2 backend: init + first frame %.1f ms (shader compile included)",
                 (sceKernelGetProcessTimeWide() - t0) / 1000.0);
        Rgba inside = read_px(110, 280, 544);
        snprintf(d, sizeof d, "window body (%d,%d,%d)", inside.r, inside.g, inside.b);
        check("stock ImGui GLES2 backend draws a window", inside.g < 60 && inside.r < 60, d);
        ImGui_ImplOpenGL3_Shutdown();
    }
    ImGui::DestroyContext();
}

// ---- precompiled shaders: glShaderBinary (mask 256) ----------------------------------------------------------------
// First run (no files): compile the terrain program from source, dump both shaders with vglGetShaderBinary into
// ux0:data/wowee/gxp/, then load them back from memory and time it. Later runs (files present): load them straight from
// the files in a fresh process, before any GLSL is compiled, and time read, glShaderBinary and link separately.
static bool read_whole_file(const char* path, uint8_t** out, int* size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    const int sz = static_cast<int>(sceIoLseek(fd, 0, SCE_SEEK_END));
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    uint8_t* buf = static_cast<uint8_t*>(malloc(sz));
    const int got = sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    if (got != sz) { free(buf); return false; }
    *out = buf;
    *size = sz;
    return true;
}

static void write_whole_file(const char* path, const void* data, int size) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, data, size);
    sceIoClose(fd);
}

static GLuint link_from_binaries(const uint8_t* vb, int vlen, const uint8_t* fb, int flen, double* msBinary,
                                 double* msLink) {
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderBinary(1, &v, 0, vb, vlen);
    glShaderBinary(1, &f, 0, fb, flen);
    const uint64_t t1 = sceKernelGetProcessTimeWide();
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    const uint64_t t2 = sceKernelGetProcessTimeWide();
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    *msBinary = (t1 - t0) / 1000.0;
    *msLink = (t2 - t1) / 1000.0;
    return ok ? prog : 0;
}

static void test_binary_shaders() {
    char d[200];
    sceIoMkdir(DIR_PATH "/gxp", 0777);
    const char* kV = DIR_PATH "/gxp/terrain.vert.bin";
    const char* kF = DIR_PATH "/gxp/terrain.frag.bin";
    uint8_t *vb = nullptr, *fb = nullptr;
    int vlen = 0, flen = 0;
    const uint64_t r0 = sceKernelGetProcessTimeWide();
    const bool haveFiles = read_whole_file(kV, &vb, &vlen) && read_whole_file(kF, &fb, &flen);
    const double msRead = (sceKernelGetProcessTimeWide() - r0) / 1000.0;
    if (!haveFiles) {
        log_line("INFO no binaries yet: compiling from source, dumping with vglGetShaderBinary");
        double ms = 0;
        GLuint v = compile_shader(GL_VERTEX_SHADER, kTerrainVS, &ms, "terrain vs");
        GLuint f = compile_shader(GL_FRAGMENT_SHADER, kTerrainFS, &ms, "terrain fs");
        GLuint prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glBindAttribLocation(prog, 0, "aPos");
        glBindAttribLocation(prog, 1, "aUV");
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        glLinkProgram(prog);
        log_line("INFO source path: link (compiles) %.1f ms", (sceKernelGetProcessTimeWide() - t0) / 1000.0);
        vb = static_cast<uint8_t*>(malloc(65536));
        fb = static_cast<uint8_t*>(malloc(65536));
        GLsizei lv = 0, lf = 0;
        vglGetShaderBinary(v, 65536, &lv, vb);
        vglGetShaderBinary(f, 65536, &lf, fb);
        vlen = lv;
        flen = lf;
        snprintf(d, sizeof d, "vertex %d bytes, fragment %d bytes", vlen, flen);
        check("shaders dumped with vglGetShaderBinary", vlen > 0 && flen > 0, d);
        write_whole_file(kV, vb, vlen);
        write_whole_file(kF, fb, flen);
    } else {
        snprintf(d, sizeof d, "vertex %d bytes, fragment %d bytes, read in %.1f ms", vlen, flen, msRead);
        log_line("INFO binaries found: %s", d);
    }
    double msBin = 0, msLink = 0;
    GLuint prog = link_from_binaries(vb, vlen, fb, flen, &msBin, &msLink);
    snprintf(d, sizeof d, "glShaderBinary x2 %.2f ms, link %.2f ms%s", msBin, msLink,
             haveFiles ? " (fresh process, no GLSL compiled yet)" : " (same process)");
    check("terrain program links from binaries", prog != 0, d);
    if (prog) {
        const bool ok = draw_terrain_and_check(prog, d, sizeof d);
        check("terrain program from binaries draws the right colour", ok, d);
    }
    free(vb);
    free(fb);
}

// ---- first use of fixed-function state combinations (mask 512) ------------------------------------------------------
// vitaGL's fixed-function path generates a shader for every new combination of GL state, compiles it at run time and
// caches the result in ux0:data/shader_cache/v*/ by a hash of the state. This times the FIRST draw of combinations the
// device probably has not seen, then the second draw. Run it twice: the first run pays the compile, the second reads the
// cache.
static void ffp_first_use(const char* name, void (*setup)(), void (*teardown)()) {
    GLuint t = solid_texture(200, 100, 50);
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    set_ortho(960, 544);
    uint64_t first = 0, second = 0;
    for (int pass = 0; pass < 2; pass++) {
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        setup();
        draw_quad(t, 100, 100, 64, 64);
        glFinish();
        (pass == 0 ? first : second) = sceKernelGetProcessTimeWide() - t0;
        teardown();
    }
    log_line("INFO ffp %-34s first draw %7.1f ms, second %6.2f ms", name, first / 1000.0, second / 1000.0);
    glDeleteTextures(1, &t);
}

static void test_ffp_first_use() {
    ffp_first_use("texture, nothing else", [] {}, [] {});
    ffp_first_use("texture + alpha blend", [] { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); },
                  [] { glDisable(GL_BLEND); });
    ffp_first_use("texture + alpha test", [] { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); },
                  [] { glDisable(GL_ALPHA_TEST); });
    ffp_first_use("texture + linear fog", [] { glEnable(GL_FOG); glFogi(GL_FOG_MODE, GL_LINEAR); glFogf(GL_FOG_START, 0.0f); glFogf(GL_FOG_END, 10.0f); },
                  [] { glDisable(GL_FOG); });
    ffp_first_use("texture + lighting (1 light)", [] { glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); },
                  [] { glDisable(GL_LIGHT0); glDisable(GL_LIGHTING); });
    ffp_first_use("texture + modulate env + fog + blend", [] { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_FOG); glFogi(GL_FOG_MODE, GL_EXP2); glFogf(GL_FOG_DENSITY, 0.01f); },
                  [] { glDisable(GL_FOG); glDisable(GL_BLEND); });
}

// ---- heavier shaders: how compile time grows with shader size (mask 1024) ---------------------------------------------
// Representative of what the Vita renderer will need, in GLSL ES 1.00: a skinned character (four bone influences from a
// uniform array, four point lights, specular, fog, alpha test), water (waves in the vertex shader, fresnel, two layers),
// a sky dome (gradient and sun glow), a WMO group (baked vertex colour, lightmap, four lights, fog) and a deliberately
// heavy stress shader (eight lights with specular and six texture samples) as an upper bound. Compile only (link time).
static const char* kCharVS =
    "precision highp float;\n"
    "attribute vec3 aPos; attribute vec3 aNormal; attribute vec2 aUV; attribute vec4 aBones; attribute vec4 aWeights;\n"
    "uniform mat4 uViewProj; uniform mat4 uModel; uniform vec4 uBones[96];\n"
    "varying vec2 vUV; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "mat4 bone(float i) {\n"
    "    int k = int(i) * 3;\n"
    "    return mat4(vec4(uBones[k].x, uBones[k+1].x, uBones[k+2].x, 0.0), vec4(uBones[k].y, uBones[k+1].y, uBones[k+2].y, 0.0),\n"
    "                vec4(uBones[k].z, uBones[k+1].z, uBones[k+2].z, 0.0), vec4(uBones[k].w, uBones[k+1].w, uBones[k+2].w, 1.0));\n"
    "}\n"
    "void main() {\n"
    "    mat4 skin = bone(aBones.x) * aWeights.x + bone(aBones.y) * aWeights.y + bone(aBones.z) * aWeights.z + bone(aBones.w) * aWeights.w;\n"
    "    vec4 p = uModel * (skin * vec4(aPos, 1.0));\n"
    "    vNormal = normalize(mat3(uModel) * (mat3(skin) * aNormal));\n"
    "    vWorld = p.xyz; vUV = aUV;\n"
    "    gl_Position = uViewProj * p;\n"
    "    vFog = clamp(gl_Position.w / 400.0, 0.0, 1.0);\n"
    "}\n";
static const char* kCharFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "uniform sampler2D uTex; uniform sampler2D uEnv;\n"
    "uniform vec4 uLightPos[4]; uniform vec4 uLightColor[4]; uniform vec3 uEye; uniform vec3 uAmbient; uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 base = texture2D(uTex, vUV);\n"
    "    if (base.a < 0.5) discard;\n"
    "    vec3 n = normalize(vNormal); vec3 v = normalize(uEye - vWorld);\n"
    "    vec3 lit = uAmbient;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        vec3 l = uLightPos[i].xyz - vWorld; float d = length(l); l /= d;\n"
    "        float att = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);\n"
    "        float diff = max(dot(n, l), 0.0);\n"
    "        vec3 h = normalize(l + v);\n"
    "        float spec = pow(max(dot(n, h), 0.0), 24.0);\n"
    "        lit += uLightColor[i].rgb * (diff + 0.3 * spec) * att;\n"
    "    }\n"
    "    vec3 env = texture2D(uEnv, n.xy * 0.5 + 0.5).rgb;\n"
    "    vec3 c = base.rgb * lit + env * 0.15;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, base.a);\n"
    "}\n";
static const char* kWaterVS =
    "precision highp float;\n"
    "attribute vec3 aPos; attribute vec2 aUV;\n"
    "uniform mat4 uViewProj; uniform float uTime; uniform vec3 uEye;\n"
    "varying vec2 vUV; varying vec2 vUV2; varying vec3 vView; varying float vFog;\n"
    "void main() {\n"
    "    vec3 p = aPos;\n"
    "    p.z += 0.15 * sin(p.x * 0.7 + uTime) + 0.1 * sin(p.y * 1.1 + uTime * 1.3) + 0.05 * sin((p.x + p.y) * 2.3 + uTime * 2.1);\n"
    "    vUV = aUV * 6.0 + vec2(uTime * 0.03, 0.0); vUV2 = aUV * 11.0 - vec2(0.0, uTime * 0.05);\n"
    "    vView = uEye - p;\n"
    "    gl_Position = uViewProj * vec4(p, 1.0);\n"
    "    vFog = clamp(gl_Position.w / 600.0, 0.0, 1.0);\n"
    "}\n";
static const char* kWaterFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec2 vUV2; varying vec3 vView; varying float vFog;\n"
    "uniform sampler2D uWater; uniform sampler2D uNormal; uniform vec3 uDeep; uniform vec3 uShallow; uniform vec3 uSun; uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec3 nm = texture2D(uNormal, vUV).rgb * 2.0 - 1.0;\n"
    "    vec3 nm2 = texture2D(uNormal, vUV2).rgb * 2.0 - 1.0;\n"
    "    vec3 n = normalize(vec3((nm.xy + nm2.xy) * 0.6, 1.0));\n"
    "    vec3 v = normalize(vView);\n"
    "    float fres = pow(1.0 - max(dot(n, v), 0.0), 4.0);\n"
    "    vec3 tex = texture2D(uWater, vUV + n.xy * 0.05).rgb;\n"
    "    vec3 body = mix(uShallow, uDeep, fres) * tex;\n"
    "    vec3 h = normalize(uSun + v);\n"
    "    float spec = pow(max(dot(n, h), 0.0), 64.0);\n"
    "    vec3 c = body + uSun * spec * 0.8 + fres * 0.25;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, 0.55 + fres * 0.35);\n"
    "}\n";
static const char* kSkyVS =
    "precision highp float;\n"
    "attribute vec3 aPos; uniform mat4 uViewProj; varying vec3 vDir;\n"
    "void main() { vDir = aPos; gl_Position = (uViewProj * vec4(aPos, 1.0)).xyww; }\n";
static const char* kSkyFS =
    "precision mediump float;\n"
    "varying vec3 vDir; uniform vec3 uZenith; uniform vec3 uHorizon; uniform vec3 uSunDir; uniform vec3 uSunColor;\n"
    "void main() {\n"
    "    vec3 d = normalize(vDir);\n"
    "    float t = clamp(d.z, 0.0, 1.0);\n"
    "    vec3 sky = mix(uHorizon, uZenith, pow(t, 0.6));\n"
    "    float sun = max(dot(d, normalize(uSunDir)), 0.0);\n"
    "    sky += uSunColor * (pow(sun, 256.0) * 2.0 + pow(sun, 8.0) * 0.25);\n"
    "    gl_FragColor = vec4(sky, 1.0);\n"
    "}\n";
static const char* kWmoVS =
    "precision highp float;\n"
    "attribute vec3 aPos; attribute vec3 aNormal; attribute vec2 aUV; attribute vec4 aColor;\n"
    "uniform mat4 uViewProj; uniform mat4 uModel;\n"
    "varying vec2 vUV; varying vec4 vColor; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "void main() {\n"
    "    vec4 p = uModel * vec4(aPos, 1.0);\n"
    "    vWorld = p.xyz; vNormal = normalize(mat3(uModel) * aNormal); vUV = aUV; vColor = aColor;\n"
    "    gl_Position = uViewProj * p;\n"
    "    vFog = clamp(gl_Position.w / 500.0, 0.0, 1.0);\n"
    "}\n";
static const char* kWmoFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec4 vColor; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "uniform sampler2D uTex; uniform sampler2D uLightmap; uniform vec4 uLightPos[4]; uniform vec4 uLightColor[4];\n"
    "uniform vec3 uSunDir; uniform vec3 uSunColor; uniform vec3 uAmbient; uniform vec3 uFogColor; uniform float uAlphaRef;\n"
    "void main() {\n"
    "    vec4 base = texture2D(uTex, vUV);\n"
    "    if (base.a < uAlphaRef) discard;\n"
    "    vec3 n = normalize(vNormal);\n"
    "    vec3 lit = uAmbient * vColor.rgb + uSunColor * max(dot(n, normalize(uSunDir)), 0.0);\n"
    "    lit += texture2D(uLightmap, vUV).rgb * 0.5;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        vec3 l = uLightPos[i].xyz - vWorld; float d = length(l);\n"
    "        float att = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);\n"
    "        lit += uLightColor[i].rgb * max(dot(n, l / d), 0.0) * att * att;\n"
    "    }\n"
    "    vec3 c = base.rgb * lit;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, base.a * vColor.a);\n"
    "}\n";
static const char* kStressFS =
    "precision mediump float;\n"
    "varying vec2 vUV; varying vec3 vNormal; varying vec3 vWorld; varying float vFog;\n"
    "uniform sampler2D uT0; uniform sampler2D uT1; uniform sampler2D uT2; uniform sampler2D uT3; uniform sampler2D uT4; uniform sampler2D uT5;\n"
    "uniform vec4 uLightPos[8]; uniform vec4 uLightColor[8]; uniform vec3 uEye; uniform vec3 uFogColor;\n"
    "void main() {\n"
    "    vec4 a = texture2D(uT0, vUV) * texture2D(uT1, vUV * 2.0);\n"
    "    vec4 b = mix(texture2D(uT2, vUV * 4.0), texture2D(uT3, vUV * 4.0), texture2D(uT4, vUV).r);\n"
    "    vec3 n = normalize(vNormal + (texture2D(uT5, vUV * 3.0).rgb - 0.5) * 0.6);\n"
    "    vec3 v = normalize(uEye - vWorld);\n"
    "    vec3 lit = vec3(0.1);\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        vec3 l = uLightPos[i].xyz - vWorld; float d = length(l); l /= d;\n"
    "        float att = clamp(1.0 - d / uLightPos[i].w, 0.0, 1.0);\n"
    "        vec3 h = normalize(l + v);\n"
    "        lit += uLightColor[i].rgb * (max(dot(n, l), 0.0) + 0.5 * pow(max(dot(n, h), 0.0), 32.0)) * att;\n"
    "    }\n"
    "    vec3 c = mix(a.rgb, b.rgb, 0.5) * lit;\n"
    "    c = mix(c, uFogColor, vFog);\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

static void heavy_one(const char* name, const char* vs, const char* fs) {
    char d[200];
    double ms = 0;
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    GLuint prog = build_program(name, vs, fs, &ms);
    (void)t0;
    snprintf(d, sizeof d, "source %d + %d chars, %.1f ms", static_cast<int>(strlen(vs)), static_cast<int>(strlen(fs)), ms);
    check(name, prog != 0, d);
}

static void test_heavy_shaders() {
    log_line("INFO heavy shaders: compile cost against source size (ARM %d MHz)", scePowerGetArmClockFrequency());
    heavy_one("terrain 2 layers (reference)", kTerrainVS, kTerrainFS);
    heavy_one("skinned character (4 bones, 4 lights)", kCharVS, kCharFS);
    heavy_one("water (waves, fresnel, 2 layers)", kWaterVS, kWaterFS);
    heavy_one("sky dome", kSkyVS, kSkyFS);
    heavy_one("WMO group (lightmap, 4 lights)", kWmoVS, kWmoFS);
    heavy_one("stress (8 lights, 6 samples)", kWmoVS, kStressFS);
    // the same character shader twice more, for the spread
    heavy_one("skinned character again", kCharVS, kCharFS);
    heavy_one("water again", kWaterVS, kWaterFS);
}

static void test_fbo_scale() {
    char d[160];
    const int fw = 640, fh = 368;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    snprintf(d, sizeof d, "status=0x%x", st);
    check("FBO 640x368 is complete", st == GL_FRAMEBUFFER_COMPLETE, d);

    // scene: blue background, a red square in the middle, in FBO pixels
    glViewport(0, 0, fw, fh);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    set_ortho(fw, fh);
    fill_rect(280, 144, 80, 80, 1, 0, 0);
    Rgba inFbo = read_px(320, 184, fh);
    fmt_rgba(d, sizeof d, inFbo, 255, 0, 0);
    check("FBO scene: red square drawn in the FBO", near_rgb(inFbo, 255, 0, 0, 12), d);

    // back to the screen: scale the FBO to 960x544 (1.5x), UI would go on top at native size
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    set_ortho(960, 544);
    draw_quad(tex, 0, 0, 960, 544);
    Rgba centre = read_px(480, 272, 544);
    Rgba corner = read_px(10, 10, 544);
    fmt_rgba(d, sizeof d, centre, 255, 0, 0);
    check("FBO scaled to 960x544: red square at the centre", near_rgb(centre, 255, 0, 0, 20), d);
    fmt_rgba(d, sizeof d, corner, 0, 0, 255);
    check("FBO scaled to 960x544: blue background at the corner", near_rgb(corner, 0, 0, 255, 20), d);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

// ImGui draw data through vitaGL's fixed-function client arrays: what a Vita ImGui backend is, in short.
static GLuint g_fontTex = 0;

static bool imgui_gl_init() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    glGenTextures(1, &g_fontTex);
    glBindTexture(GL_TEXTURE_2D, g_fontTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID(static_cast<ImTextureID>(g_fontTex));
    return glGetError() == GL_NO_ERROR && px != nullptr && w > 0;
}

static void imgui_gl_render(ImDrawData* dd) {
    const int fbW = static_cast<int>(dd->DisplaySize.x * dd->FramebufferScale.x);
    const int fbH = static_cast<int>(dd->DisplaySize.y * dd->FramebufferScale.y);
    if (fbW <= 0 || fbH <= 0) return;
    glViewport(0, 0, fbW, fbH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(dd->DisplayPos.x, dd->DisplayPos.x + dd->DisplaySize.x, dd->DisplayPos.y + dd->DisplaySize.y,
             dd->DisplayPos.y, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        const char* vtx = reinterpret_cast<const char*>(cl->VtxBuffer.Data);
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), vtx + offsetof(ImDrawVert, pos));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), vtx + offsetof(ImDrawVert, uv));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), vtx + offsetof(ImDrawVert, col));
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const ImVec2 clipMin((cmd.ClipRect.x - dd->DisplayPos.x) * dd->FramebufferScale.x,
                                 (cmd.ClipRect.y - dd->DisplayPos.y) * dd->FramebufferScale.y);
            const ImVec2 clipMax((cmd.ClipRect.z - dd->DisplayPos.x) * dd->FramebufferScale.x,
                                 (cmd.ClipRect.w - dd->DisplayPos.y) * dd->FramebufferScale.y);
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) continue;
            glScissor(static_cast<int>(clipMin.x), static_cast<int>(fbH - clipMax.y),
                      static_cast<int>(clipMax.x - clipMin.x), static_cast<int>(clipMax.y - clipMin.y));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cmd.GetTexID()));
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount),
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           cl->IdxBuffer.Data + cmd.IdxOffset);
        }
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

static void test_imgui() {
    char d[160];
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(960, 544);
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    bool ok = imgui_gl_init();
    snprintf(d, sizeof d, "ImGui %s", IMGUI_VERSION);
    check("ImGui font atlas builds and uploads to a texture", ok, d);
    if (!ok) { ImGui::DestroyContext(); return; }

    glClearColor(0.2f, 0.4f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    set_ortho(960, 544);
    Rgba before = read_px(110, 280, 544);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(100, 100));
    ImGui::SetNextWindowSize(ImVec2(300, 200));
    ImGui::Begin("GlProbe", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("ImGui on vitaGL");
    ImGui::End();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    check("ImGui produced draw data", dd && dd->TotalVtxCount > 0, nullptr);
    imgui_gl_render(dd);
    Rgba inside = read_px(110, 280, 544);  // inside the window body, below the text
    // the dark window background (about 0.06 grey at 0.94 alpha) must replace the green clear colour
    snprintf(d, sizeof d, "clear (%d,%d,%d), window body (%d,%d,%d)", before.r, before.g, before.b,
             inside.r, inside.g, inside.b);
    check("ImGui window body drawn over the clear colour", inside.g < before.g - 40 && inside.r < 60, d);
    // the title text must have put lighter pixels somewhere in the title bar
    int light = 0;
    for (int x = 104; x < 396 && light == 0; x += 2) {
        for (int y = 104; y < 124; y += 2) {
            Rgba p = read_px(x, y, 544);
            if (p.r > 150 && p.g > 150 && p.b > 150) { light++; break; }
        }
    }
    check("ImGui text pixels (light on dark) found in the title bar", light > 0, nullptr);
    glDeleteTextures(1, &g_fontTex);
    ImGui::DestroyContext();
}

// ux0:data/wowee/glprobe.mask (hex) selects what runs, to bisect a crash: 1 clear read-back, 2 DXT, 4 FBO,
// 8 ImGui, 16 frame loop, 32 DXT3/DXT5 variants (diagnostic), 64 draw-call cost, 128 shader path (run-time compile, stock GLES2 ImGui), 256 precompiled shaders (glShaderBinary), 512 first use of fixed-function state combinations, 1024 heavier shaders (compile time against size). Missing or unreadable: 31.
static unsigned read_mask() {
    unsigned mask = 31;
    SceUID fd = sceIoOpen(DIR_PATH "/glprobe.mask", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char t[16] = {0};
        sceIoRead(fd, t, sizeof t - 1);
        sceIoClose(fd);
        mask = static_cast<unsigned>(strtoul(t, nullptr, 16));
    }
    return mask;
}

// ux0:data/wowee/glprobe.init (a number) picks the vitaGL init call, to find what an environment accepts:
// 0 vglInitExtended(0, 960x544, ram_threshold 24 MB)   1 vglInit(8 MB legacy pool)
// 2 vglInitExtended(8 MB legacy pool, 960x544, ram_threshold 24 MB)
// 3 vglInitWithCustomSizes(0, 960x544, ram 32 MB, cdram 24 MB, phycont 1 MB, cdlg 0)
static unsigned read_init_mode() {
    unsigned mode = 0;
    SceUID fd = sceIoOpen(DIR_PATH "/glprobe.init", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char t[16] = {0};
        sceIoRead(fd, t, sizeof t - 1);
        sceIoClose(fd);
        mode = static_cast<unsigned>(strtoul(t, nullptr, 10));
    }
    return mode;
}

static GLboolean init_vgl(unsigned mode) {
    switch (mode) {
        case 1: return vglInit(0x800000);
        case 2: return vglInitExtended(0x800000, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
        case 3: return vglInitWithCustomSizes(0, 960, 544, 32 * 1024 * 1024, 24 * 1024 * 1024, 1024 * 1024, 0,
                                              SCE_GXM_MULTISAMPLE_NONE);
        default: return vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
    }
}

int main() {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DIR_PATH, 0777);
    g_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    log_line("INFO glprobe start");
    // The clocks the client sets in platform::vita::initProcess (VITA-6); an app starts at 333/222/222.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    char d[160];
    vglSetupRuntimeShaderCompiler(SHARK_OPT_DEFAULT, 0, 0, 0);
#ifdef GLPROBE_CUSTOM_VGL
    // Only in a vitaGL built with HAVE_SHADER_CACHE=1: keep the cache inside our own folder, easy to delete.
    vglSetShaderCachePath("ux0:data/wowee/shader_cache");
#endif
    const unsigned initMode = read_init_mode();
    // vglInit* returns GL_TRUE ONLY WHEN THE REQUESTED RESOLUTION HAD TO BE LOWERED ("res_fallback" in vitaGL's
    // vgl.c) and GL_FALSE on a normal success, so the return value says nothing about whether init worked.
    // The first version of this probe read it as success and aborted (VITA-13); state is checked instead.
    const GLboolean resFallback = init_vgl(initMode);
    snprintf(d, sizeof d, "init mode %u returned %d (GL_TRUE means the resolution fell back)", initMode,
             static_cast<int>(resFallback));
    log_line("INFO %s", d);
    const bool inited = vglMemTotal(VGL_MEM_VRAM) > 0 && glGetString(GL_VERSION) != nullptr;
    snprintf(d, sizeof d, "CDRAM total %llu KB", static_cast<unsigned long long>(vglMemTotal(VGL_MEM_VRAM) / 1024));
    check("vitaGL is initialised (pools exist, GL_VERSION answers)", inited, d);
    check("960x544 was available (no resolution fallback)", resFallback == GL_FALSE, nullptr);
    if (!inited) { log_line("INFO glprobe aborted"); return 1; }

    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    log_line("INFO GL vendor='%s' renderer='%s' version='%s'", vendor ? vendor : "?", renderer ? renderer : "?",
             version ? version : "?");
    log_line("INFO GL extensions: %s", ext ? ext : "?");
    check("GL_EXT_texture_compression_s3tc is advertised",
          ext && strstr(ext, "texture_compression_s3tc") != nullptr, nullptr);
    static const char* kMemNames[] = {"CDRAM", "RAM", "PHYCONT", "BUDGET(CDLG)", "EXTERNAL(newlib)"};
    for (int t = 0; t < 5; t++) {
        log_line("INFO vitaGL memory %-16s free %llu KB of %llu KB", kMemNames[t],
                 static_cast<unsigned long long>(vglMemFree(static_cast<vglMemType>(t)) / 1024),
                 static_cast<unsigned long long>(vglMemTotal(static_cast<vglMemType>(t)) / 1024));
    }

    const unsigned mask = read_mask();
    log_line("INFO test mask 0x%x", mask);

    // 2. clear colour
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (mask & 1) {
        Rgba c = read_px(480, 272, 544);
        fmt_rgba(d, sizeof d, c, 64, 128, 191);
        check("clear colour reads back", near_rgb(c, 64, 128, 191, 4), d);
    }

    if (mask & 2) test_dxt();
    if (mask & 32) test_dxt_variants();
    if (mask & 64) test_draw_cost();
    if (mask & 128) test_shaders();
    if (mask & 256) test_binary_shaders();
    if (mask & 512) test_ffp_first_use();
    if (mask & 1024) test_heavy_shaders();
    if (mask & 4) test_fbo_scale();
    if (mask & 8) test_imgui();

    // 6. frame loop: clear, scaled FBO-sized quad of work, swap
    const int frames = (mask & 16) ? 120 : 0;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (int i = 0; i < frames; i++) {
        glClearColor(0.1f, 0.1f * (i % 10), 0.3f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        set_ortho(960, 544);
        fill_rect(10.0f + i, 10, 100, 100, 1, 1, 0);
        vglSwapBuffers(GL_FALSE);
    }
    uint64_t t1 = sceKernelGetProcessTimeWide();
    if (frames > 0)
        log_line("INFO %d frames in %llu us: %.2f ms per frame (emulator numbers are not device numbers)", frames,
                 static_cast<unsigned long long>(t1 - t0), static_cast<double>(t1 - t0) / 1000.0 / frames);

    for (int t = 0; t < 5; t++) {
        log_line("INFO vitaGL memory after %-16s free %llu KB of %llu KB", kMemNames[t],
                 static_cast<unsigned long long>(vglMemFree(static_cast<vglMemType>(t)) / 1024),
                 static_cast<unsigned long long>(vglMemTotal(static_cast<vglMemType>(t)) / 1024));
    }
    log_line("INFO glprobe done: %d failure(s)", g_fail);
    if (g_fd >= 0) sceIoClose(g_fd);
    sceKernelExitProcess(0);
    return 0;
}

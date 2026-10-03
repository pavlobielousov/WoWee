// GlProbe (VITA-13): answers, on Vita3K now and on a real Vita later, the vitaGL questions ADR-001
// leaves open, with PASS/FAIL lines in ux0:data/wowee/glprobe.log. Uses only the fixed-function
// pipeline (vitaGL has built-in shaders for it, so no libshacccg.suprx is needed); the GLES2 shader
// path of the ImGui backend is a separate probe because it needs the run-time shader compiler.
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
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <vitaGL.h>

#include "imgui.h"

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
    check("DXT5 block green (colour part)", near_rgb(g5, 0, 255, 0, 12), d);

    glDeleteTextures(1, &t1);
    glDeleteTextures(1, &t5);
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
// 8 ImGui, 16 frame loop. Missing or unreadable: everything.
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

int main() {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DIR_PATH, 0777);
    g_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    log_line("INFO glprobe start");

    char d[160];
    vglSetupRuntimeShaderCompiler(SHARK_OPT_DEFAULT, 0, 0, 0);
    GLboolean ok = vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
    check("vglInitExtended(960x544)", ok == GL_TRUE, nullptr);
    if (ok != GL_TRUE) { log_line("INFO glprobe aborted"); return 1; }

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
        log_line("INFO vitaGL memory %-16s free %zu KB of %zu KB", kMemNames[t],
                 vglMemFree(static_cast<vglMemType>(t)) / 1024, vglMemTotal(static_cast<vglMemType>(t)) / 1024);
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
        log_line("INFO vitaGL memory after %-16s free %zu KB of %zu KB", kMemNames[t],
                 vglMemFree(static_cast<vglMemType>(t)) / 1024, vglMemTotal(static_cast<vglMemType>(t)) / 1024);
    }
    log_line("INFO glprobe done: %d failure(s)", g_fail);
    if (g_fd >= 0) sceIoClose(g_fd);
    sceKernelExitProcess(0);
    return 0;
}

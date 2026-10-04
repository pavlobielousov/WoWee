// See shader_build.hpp (VITA-53).
#include "rendering/gl/shader_build.hpp"

#include "core/logger.hpp"
#include "platform/vita/shader_cache_logic.hpp"
#include "rendering/gl/shader_cache.hpp"
#include "rendering/gl/shader_manifest.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <vitaGL.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace sc = wowee::platform::vita::shadercache;

namespace wowee::rendering::gl {

namespace {

constexpr int kW = 960;
constexpr int kH = 544;

double nowMs() { return static_cast<double>(sceKernelGetProcessTimeWide()) / 1000.0; }

/// Every iteration: keep the console awake (a long compile reads as idle to the system) and let SDL see its events.
void keepAlive() {
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    SDL_PumpEvents();
}

// ---- the screen ---------------------------------------------------------------------------------------------------

struct Screen {
    bool imguiUp = false;

    // Before the interface's own program exists nothing can draw text, and the fixed-function path would compile a
    // shader of its own (150 to 650 ms, DEV_SETUP section 19), so the first frame is a bar made of scissored clears.
    void barOnly(float fraction) {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(120, 250, static_cast<GLsizei>(720.0f * std::clamp(fraction, 0.0f, 1.0f)), 20);
        glClearColor(0.78f, 0.61f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        vglSwapBuffers(GL_FALSE);
    }

    void startImgui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::GetStyle().FontScaleMain = 2.0f;  // the built-in face is tiny at 960x544
        // The stock backend builds its program here; through the cache it is loaded, or compiled and stored.
        ImGui_ImplOpenGL3_Init("#version 100");
        // Init() only prepares: the backend creates its program at the first frame. Doing it now makes the cache see
        // (and keep) its shaders even on a run that draws no frame (the sweep deletes entries nobody asked for).
        ImGui_ImplOpenGL3_CreateDeviceObjects();
        imguiUp = true;
    }

    void stopImgui() {
        if (!imguiUp) return;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        imguiUp = false;
    }

    void frame(float fraction, const std::string& headline, const std::string& detail, const std::string& eta) {
        if (!imguiUp) {
            barOnly(fraction);
            return;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText(ImVec2(120, 150), IM_COL32(230, 220, 190, 255), "Preparing graphics");
        dl->AddText(ImVec2(120, 190), IM_COL32(170, 170, 170, 255), "This happens once, and again after an update.");
        dl->AddText(ImVec2(120, 300), IM_COL32(230, 230, 230, 255), headline.c_str());
        if (!detail.empty()) dl->AddText(ImVec2(120, 340), IM_COL32(170, 170, 170, 255), detail.c_str());
        if (!eta.empty()) dl->AddText(ImVec2(120, 380), IM_COL32(170, 170, 170, 255), eta.c_str());
        const float w = 720.0f * std::clamp(fraction, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(120, 250), ImVec2(840, 270), IM_COL32(40, 42, 50, 255));
        if (w > 0.0f) dl->AddRectFilled(ImVec2(120, 250), ImVec2(120 + w, 270), IM_COL32(199, 156, 33, 255));
        ImGui::Render();
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        vglSwapBuffers(GL_FALSE);
    }

    /// A message the player has to see: shown until a button is pressed or the screen is touched.
    void message(const std::string& line1, const std::string& line2) {
        SceCtrlData pad{};
        for (;;) {
            if (imguiUp) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                dl->AddText(ImVec2(80, 150), IM_COL32(230, 150, 120, 255), "Graphics setup problem");
                dl->AddText(ImVec2(80, 230), IM_COL32(230, 230, 230, 255), line1.c_str());
                dl->AddText(ImVec2(80, 280), IM_COL32(170, 170, 170, 255), line2.c_str());
                dl->AddText(ImVec2(80, 420), IM_COL32(170, 170, 170, 255), "Press any button to continue.");
                ImGui::Render();
                glClearColor(0.12f, 0.04f, 0.04f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            } else {
                glClearColor(0.5f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            vglSwapBuffers(GL_FALSE);
            keepAlive();
            sceCtrlPeekBufferPositive(0, &pad, 1);
            SDL_Event e;
            bool touched = false;
            while (SDL_PollEvent(&e)) touched |= (e.type == SDL_EVENT_FINGER_DOWN);
            if (pad.buttons != 0 || touched) return;
        }
    }
};

bool compilerModulePresent() {
    std::ifstream in("ur0:data/libshacccg.suprx", std::ios::binary);
    return static_cast<bool>(in);
}

/// Compile and link one program, then throw it away: what is wanted is the cache entry it leaves.
bool buildProgram(const ProgramDef& def) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &def.vertex, nullptr);
    glShaderSource(f, 1, &def.fragment, nullptr);
    glCompileShader(v);
    glCompileShader(f);
    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {};
        glGetProgramInfoLog(program, sizeof log - 1, nullptr, log);
        LOG_ERROR("Shader program '", def.name, "' failed to link: ", log);
    }
    glDeleteProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    return linked != 0;
}

std::string etaText(double ms) {
    if (ms < 1500.0) return "";
    char buf[48];
    std::snprintf(buf, sizeof buf, "About %d seconds left", static_cast<int>(ms / 1000.0 + 0.5));
    return buf;
}

}  // namespace

ShaderBuildResult buildShaders() {
    ShaderBuildResult result;
    const auto& manifest = shaderManifest();
    const double started = nowMs();

    sc::Progress progress;
    progress.add(ProgramClass::Ui);  // the interface's own program, built first (below)
    for (const ProgramDef& def : manifest) progress.add(def.cls);
    result.programs = static_cast<unsigned>(progress.total());

    Screen screen;
    const bool firstRun = shaderCacheEmpty();
    if (firstRun) screen.barOnly(0.0f);

    // 1. The interface's program: the progress screen's own text needs it, so it comes first. A temporary ImGui
    //    context is enough; UIManager makes the real one later and finds the program in the cache.
    {
        const unsigned compiledBefore = shaderCacheStats().compiled;
        const double t0 = nowMs();
        screen.startImgui();
        const double ms = nowMs() - t0;
        const bool compiled = shaderCacheStats().compiled > compiledBefore;
        compiled ? ++result.compiled : ++result.cached;
        result.longestStallMs = std::max(result.longestStallMs, compiled ? ms : 0.0);
        progress.finished(ms, compiled);
    }

    // 2. The manifest, one program at a time: say what is next, compile it, repeat. A cached program takes no time and
    //    draws nothing; only work that takes time earns a frame.
    for (std::size_t i = 0; i < manifest.size(); ++i) {
        const ProgramDef& def = manifest[i];
        keepAlive();
        const bool cached = shaderCacheHas(def.vertex, def.fragment);
        if (!cached) {
            char head[96];
            std::snprintf(head, sizeof head, "Compiling %s", def.name);
            char count[48];
            std::snprintf(count, sizeof count, "%u of %u", static_cast<unsigned>(progress.done() + 1),
                          static_cast<unsigned>(progress.total()));
            screen.frame(static_cast<float>(progress.fraction()), head, count, etaText(progress.remainingMs()));
        }
        const unsigned compiledBefore = shaderCacheStats().compiled;
        const double t0 = nowMs();
        const bool ok = buildProgram(def);
        const double ms = nowMs() - t0;
        const bool compiled = shaderCacheStats().compiled > compiledBefore;
        if (!ok) {
            ++result.failed;
            if (!compilerModulePresent()) {
                screen.message("The shader compiler (libshacccg.suprx) is missing from ur0:data.",
                               "Install it and restart WoWee. See the release notes.");
                break;  // every further program would fail the same way
            }
            screen.message(std::string("The graphics program '") + def.name + "' failed to compile.",
                           "Details are in ux0:data/wowee/wowee.log. Continuing without it.");
        }
        compiled ? ++result.compiled : ++result.cached;
        if (compiled) result.longestStallMs = std::max(result.longestStallMs, ms);
        progress.finished(ms, compiled);
    }

    if (firstRun || result.compiled > 1) {
        screen.frame(1.0f, "Done", "", "");
    }
    screen.stopImgui();
    if (result.failed == 0) sweepShaderCache();

    result.totalMs = nowMs() - started;
    LOG_WARNING("Shader build: ", result.programs, " programs, ", result.compiled, " compiled, ", result.cached,
                " cached, ", result.failed, " failed, longest stall ", result.longestStallMs, " ms, total ",
                result.totalMs, " ms");
    return result;
}

}  // namespace wowee::rendering::gl

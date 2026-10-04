// The Vita's core::Window (VITA-52, ADR-001, docs/vita/DEV_SETUP.md section 20).
//
// Display sharing: vitaGL owns the display (it brings up sceGxm and the framebuffers in vglInit*), and SDL3
// is initialised for events, gamepad and touch only. The SDL window is created WITHOUT a graphics flag
// (SDL_WINDOW_OPENGL / _VULKAN), so SDL's Vita video driver only stands behind the touch screen, keyboard and
// the window handle the interface code asks for; it never creates a GXM renderer or a GLES context.
//
// include/core/window.hpp is shared and unedited, and it names three rendering types by forward declaration
// (VkContext, VkUiTextureService, VkImGuiBackend) and owns them through unique_ptr. This file gives those names
// Vita definitions, so the out-of-line destructor compiles: "Vk" is the name the shared header fixed, not a
// Vulkan type. VkUiTextureService and VkImGuiBackend derive from the same interfaces the desktop ones do
// (include/rendering/ui_texture.hpp, imgui_backend.hpp), which is what the interface code talks to.
#include "core/window.hpp"

#include "core/logger.hpp"
#include "platform/vita/ime_dialog.hpp"
#include "rendering/gl/shader_build.hpp"
#include "rendering/gl/shader_cache.hpp"
#include "rendering/imgui_backend.hpp"
#include "rendering/ui_texture.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <vitaGL.h>
#include <vitashark.h>

#include <atomic>
#include <cstdlib>
#include <cstdint>

// vitaGL has no glDetachShader and the stock imgui_impl_opengl3 calls it after linking its program (measured,
// DEV_SETUP section 19). Weak, so a vitaGL that gains the function wins.
extern "C" __attribute__((weak)) void glDetachShader(GLuint, GLuint) {}

namespace wowee {
namespace rendering {

/// What the shared Window header calls the "rendering context": here, the vitaGL context. It is up from a
/// successful Window::initialize() until shutdown().
class VkContext {
public:
    bool up = false;
    /// Bumped whenever texture ids an earlier upload returned stop being valid (never on the Vita yet: a GL texture
    /// name lives until it is deleted). The widget renderer re-uploads when it changes.
    std::atomic<uint32_t> uiTextureGeneration{1};
};

class VkUiTextureService final : public IUiTextureService {
public:
    explicit VkUiTextureService(VkContext& ctx) : ctx_(ctx) {}

    UiTexture upload(const uint8_t* rgba, int width, int height) override {
        if (!ctx_.up || !rgba || width <= 0 || height <= 0) return kNoUiTexture;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (tex == 0) return kNoUiTexture;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        // ImGui's GL backend reads the id as a GL texture name.
        return UiTexture{static_cast<uint64_t>(tex)};
    }

    [[nodiscard]] uint32_t generation() const override { return ctx_.uiTextureGeneration.load(); }

    // Uploads are synchronous GL calls on the main thread (ADR-001, single GL thread): no batching to do.
    void beginUploadBatch() override {}
    void endUploadBatchSync() override {}

private:
    VkContext& ctx_;
};

class VkImGuiBackend final : public IImGuiBackend {
public:
    explicit VkImGuiBackend(VkContext* ctx) : ctx_(ctx) {}

    bool init(SDL_Window* window) override {
        if (!ctx_ || !ctx_->up) return false;
        // SDL for input only (events, touch, gamepad); the stock GLES2 backend draws, shaders compiled by vitaGL's
        // run-time translator at the first frame (VITA-14 moves them to precompiled binaries).
        if (!ImGui_ImplSDL3_InitForOther(window)) return false;
        if (!ImGui_ImplOpenGL3_Init("#version 100")) {
            ImGui_ImplSDL3_Shutdown();
            return false;
        }
        live_ = true;
        return true;
    }

    void newFrame() override {
        if (!live_) return;
        driveKeyboard();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
    }

    void shutdown() override {
        if (!live_) return;
        live_ = false;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }

    /// The context is going away: after this nothing may call GL.
    void contextGone() { ctx_ = nullptr; }

private:
    // The system keyboard (VITA-55). SDL's own Vita keyboard is switched off (Window::initialize) because its dialog
    // always opens empty. Instead, when a text field is tapped, the dialog opens with the field's current text; on
    // Enter the field is emptied (Ctrl+A, Delete) and the new text typed into it, so ImGui's own editing, undo and
    // callbacks see an ordinary edit. Cancel changes nothing.
    void driveKeyboard() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g) return;
        ImGuiIO& io = ImGui::GetIO();

        bool confirmed = false;
        std::string text;
        if (platform::vita::imePoll(confirmed, text)) {
            if (confirmed) {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_A, true);
                io.AddKeyEvent(ImGuiKey_A, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
                io.AddKeyEvent(ImGuiKey_Delete, true);
                io.AddKeyEvent(ImGuiKey_Delete, false);
                if (!text.empty()) io.AddInputCharactersUTF8(text.c_str());
            }
            return;
        }
        if (platform::vita::imeActive()) return;

        // A text field was tapped (PaperUI::field calls imeRequest) or an ImGui text box took focus.
        std::string initial;
        bool password = false;
        if (platform::vita::imeTakeRequest(initial, password)) {
            platform::vita::imeOpen(initial, password);
            return;
        }
        const ImGuiID active = g->ActiveId;
        const bool editing = io.WantTextInput && active != 0 && g->InputTextState.ID == active;
        const bool tappedAgain = editing && ImGui::IsMouseClicked(0) && g->HoveredIdPreviousFrame == active;
        if (editing && (active != lastActive_ || tappedAgain)) {
            const ImGuiInputTextState& state = g->InputTextState;
            const std::string current = state.TextSrc ? std::string(state.TextSrc, static_cast<std::size_t>(state.TextLen))
                                                      : std::string();
            platform::vita::imeOpen(current, (state.Flags & ImGuiInputTextFlags_Password) != 0);
        }
        lastActive_ = editing ? active : 0;
    }

    VkContext* ctx_;
    bool live_ = false;
    unsigned lastActive_ = 0;
};

}  // namespace rendering

namespace core {

namespace {
// The Vita screen. vglInit* takes the size once; there is no window resizing or fullscreen toggle.
constexpr int kScreenW = 960;
constexpr int kScreenH = 544;
// Headroom for newlib's malloc before vitaGL takes the RAM pool (the value GlProbe measured on the device).
constexpr int kRamThresholdBytes = 24 * 1024 * 1024;
}  // namespace

Window::Window(const WindowConfig& config)
    : config(config)
    , width(kScreenW)
    , height(kScreenH)
    , windowedWidth(kScreenW)
    , windowedHeight(kScreenH)
    , fullscreen(true)
    , vsync(config.vsync) {
}

Window::~Window() {
    shutdown();
}

// There is no surface to lose on the Vita: the app is suspended whole, and vitaGL survives that.
void Window::releaseSurface() {}
bool Window::restoreSurface() { return true; }
bool Window::isSurfaceLost() const { return false; }
void Window::markSwapchainDirty() {}

// Anisotropic filtering is not offered by sceGxm.
void Window::setAnisotropyLimit(float) {}

rendering::IUiTextureService* Window::getUiTextureService() const { return uiTextures.get(); }
rendering::IImGuiBackend* Window::getImGuiBackend() const { return imguiBackend.get(); }

void Window::setWindowIcon() {}

void Window::refreshDrawableSize() {
    drawableWidth = kScreenW;
    drawableHeight = kScreenH;
}

bool Window::initialize() {
    LOG_INFO("Initializing window: ", config.title, " (Vita: vitaGL display, SDL3 for input)");

    // The keyboard is the client's own sceImeDialog (vita_ime.cpp), opened with the field's text; SDL's would open
    // empty and show a second dialog (VITA-55).
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        LOG_ERROR("Failed to initialize SDL: ", SDL_GetError());
        return false;
    }

    // No graphics flag: see the top of this file.
    window = SDL_CreateWindow(config.title.c_str(), kScreenW, kScreenH, 0);
    if (!window) {
        LOG_ERROR("Failed to create window: ", SDL_GetError());
        return false;
    }
    width = kScreenW;
    height = kScreenH;
    refreshDrawableSize();

    // GL work stays on this thread (ADR-001). vglInit* returns GL_TRUE ONLY when the requested resolution had to be
    // lowered and GL_FALSE on success (DEV_SETUP section 19), so success is judged by state.
    // The run-time shader compiler's optimisation level (VITA-53). vitaGL's own default is the fastest-math level and
    // compiled the skinned-character stand-in in 2.1 s on the device; SHARK_OPT_DEFAULT (O2, what GlProbe used) takes
    // 1.05 s with 4.3 s against 5.8 s for the set. WOWEE_SHARK_OPT=0..4 in env.txt overrides it for comparison
    // (SLOW, SAFE, DEFAULT, FAST, UNSAFE). The level is part of every cache key.
    int sharkLevel = SHARK_OPT_DEFAULT;
    if (const char* opt = std::getenv("WOWEE_SHARK_OPT"); opt && *opt >= '0' && *opt <= '4') sharkLevel = *opt - '0';
    LOG_WARNING("Shader compiler optimisation level ", sharkLevel);
    vglSetupRuntimeShaderCompiler(static_cast<shark_opt>(sharkLevel), 0, 0, 0);
    rendering::gl::setShaderCompilerLevel(sharkLevel);
    const GLboolean resolutionFell = vglInitExtended(0, kScreenW, kScreenH, kRamThresholdBytes, SCE_GXM_MULTISAMPLE_NONE);
    if (vglMemTotal(VGL_MEM_VRAM) == 0 || glGetString(GL_VERSION) == nullptr) {
        LOG_ERROR("vitaGL did not initialise (no memory pools, no GL_VERSION)");
        return false;
    }
    if (resolutionFell == GL_TRUE) {
        LOG_WARNING("vitaGL lowered the resolution from ", kScreenW, "x", kScreenH);
    }
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    // Warning level on purpose: the default log level hides INFO and these numbers are VITA-13's device evidence.
    LOG_WARNING("vitaGL: ", renderer ? renderer : "?", ", ", version ? version : "?");
    static const char* kPoolNames[] = {"CDRAM", "RAM", "PHYCONT", "CDLG", "newlib"};
    for (int pool = 0; pool < 5; ++pool) {
        const auto type = static_cast<vglMemType>(pool);
        LOG_WARNING("vitaGL memory ", kPoolNames[pool], ": ",
                 static_cast<unsigned long long>(vglMemFree(type) / 1024), " KB free of ",
                 static_cast<unsigned long long>(vglMemTotal(type) / 1024), " KB");
    }

    // WOWEE_SHADER_REBUILD=1 in env.txt forces every shader to be compiled again (VITA-53).
    if (const char* rebuild = std::getenv("WOWEE_SHADER_REBUILD"); rebuild && *rebuild == '1') {
        LOG_WARNING("WOWEE_SHADER_REBUILD=1: clearing the shader cache");
        rendering::gl::clearShaderCache();
    }

    // Every shader program, from the cache or compiled now behind a progress screen (VITA-53).
    rendering::gl::buildShaders();

    vkContext = std::make_unique<rendering::VkContext>();
    vkContext->up = true;
    uiTextures = std::make_unique<rendering::VkUiTextureService>(*vkContext);
    imguiBackend = std::make_unique<rendering::VkImGuiBackend>(vkContext.get());

    LOG_WARNING("Window initialized successfully (vitaGL)");
    return true;
}

void Window::shutdown() {
    uiTextures.reset();
    if (imguiBackend) imguiBackend->contextGone();
    if (vkContext) {
        vkContext->up = false;
        vkContext.reset();
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
        // vitaGL has no shutdown call that returns to a usable state; the process exits next.
        SDL_Quit();
    }
}

// The Vita has one fixed 960x544 screen: these keep the shared interface callable and do nothing.
void Window::setFullscreen(bool) {}
void Window::setVsync(bool enable) { vsync = enable; }  // vglSwapBuffers always waits for the vertical blank
void Window::applyResolution(int, int) {}

}  // namespace core
}  // namespace wowee

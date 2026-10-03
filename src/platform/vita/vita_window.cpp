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
#include "rendering/imgui_backend.hpp"
#include "rendering/ui_texture.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <vitaGL.h>

#include <atomic>
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
    VkContext* ctx_;
    bool live_ = false;
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
    LOG_INFO("vitaGL: ", renderer ? renderer : "?", ", ", version ? version : "?");
    static const char* kPoolNames[] = {"CDRAM", "RAM", "PHYCONT", "CDLG", "newlib"};
    for (int pool = 0; pool < 5; ++pool) {
        const auto type = static_cast<vglMemType>(pool);
        LOG_INFO("vitaGL memory ", kPoolNames[pool], ": ",
                 static_cast<unsigned long long>(vglMemFree(type) / 1024), " KB free of ",
                 static_cast<unsigned long long>(vglMemTotal(type) / 1024), " KB");
    }

    vkContext = std::make_unique<rendering::VkContext>();
    vkContext->up = true;
    uiTextures = std::make_unique<rendering::VkUiTextureService>(*vkContext);
    imguiBackend = std::make_unique<rendering::VkImGuiBackend>(vkContext.get());

    LOG_INFO("Window initialized successfully (vitaGL)");
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

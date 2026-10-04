// Vita skeleton of Renderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/renderer.hpp"

#include "core/logger.hpp"
#include "platform/vita/ime_dialog.hpp"
#include "core/window.hpp"
#include "rendering/imgui_backend.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/gl/shader_selftest.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/footprint_renderer.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/minimap.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/world_map.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "game/zone_manager.hpp"

#include "rendering/volumetric_fog.hpp"
#include "rendering/sun_shafts.hpp"
#include "rendering/screen_capture.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/render_graph.hpp"
#include "rendering/overlay_system.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/weather.hpp"
#include "rendering/lightning.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <SDL3/SDL.h>
#include <vitaGL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace wowee::rendering {

void Renderer::beginFrame() {
    glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::beginUploadBatch() { }

namespace {
// A screenshot is taken at the end of the frame, after the interface has been drawn and before the swap. Asked for
// through Renderer::captureScreenshot, or by WOWEE_SHOT_FRAME=<n> in env.txt (frame n, to ux0:data/wowee/shot.png): the
// way to see what the Vita draws without a camera (VITA-17).
std::string g_shotPath;

bool writeScreenshot(const std::string& path) {
    constexpr int w = 960, h = 544;
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4), flipped(px.size());
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; ++y) {  // GL rows run bottom to top
        std::copy_n(px.data() + static_cast<std::size_t>(h - 1 - y) * w * 4, w * 4, flipped.data() + static_cast<std::size_t>(y) * w * 4);
    }
    for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
    return stbi_write_png(path.c_str(), w, h, 4, flipped.data(), w * 4) != 0;
}
}  // namespace

bool Renderer::captureScreenshot(const std::string& outputPath) {
    g_shotPath = outputPath;
    return true;
}

void Renderer::clearSelectionCircle() { }

void Renderer::endFrame() {
    static unsigned frames = 0;
    // Where the Vulkan renderer records ImGui into its overlay pass: the interface was built into ImGui's draw
    // data by UIManager::render() (ImGui::Render()) earlier in the frame.
    if (ImGui::GetCurrentContext()) {
        if (ImDrawData* draw = ImGui::GetDrawData()) ImGui_ImplOpenGL3_RenderDrawData(draw);
    }
    // The on-screen keyboard is a system dialog (platform/vita/ime_dialog.hpp). vitaGL draws it only when told a
    // dialog is active at the swap; without this it opens invisibly and takes the touch input.
    if (g_shotPath.empty()) {
        static const long shotFrame = [] {
            const char* v = std::getenv("WOWEE_SHOT_FRAME");
            return v ? std::atol(v) : 0L;
        }();
        static long frameNo = 0;
        if (shotFrame > 0 && ++frameNo == shotFrame) g_shotPath = "ux0:data/wowee/shot.png";
    }
    if (!g_shotPath.empty()) {
        const bool ok = writeScreenshot(g_shotPath);
        LOG_WARNING("Screenshot ", g_shotPath, ok ? " written" : " FAILED");
        g_shotPath.clear();
    }
    vglSwapBuffers(platform::vita::imeActive() ? GL_TRUE : GL_FALSE);
    // Warning level on purpose: the default log level hides INFO, and this line is the Vita3K smoke test's proof that
    // the main loop runs on vitaGL (it cannot read pixels back).
    if (frames++ % 300 == 0) LOG_WARNING("Vita frames presented: ", frames);
}

void Renderer::endUploadBatch() { }

void Renderer::endUploadBatchSync() { }

uint32_t Renderer::getCurrentZoneId() const { return 0; }

int Renderer::getMaxMsaaSamples() const { return 0; }

PostProcessPipeline* Renderer::getPostProcessPipeline() const { return nullptr; }

int Renderer::getTerrainLoadRadius() const { return 0; }

const std::vector<std::pair<const char*, double>>& Renderer::gpuTimings() const {
    static const std::vector<std::pair<const char*, double>> none;
    return none;
}

bool Renderer::initialize(core::Window* win) {
    // The window already brought vitaGL up (src/platform/vita/vita_window.cpp). The scene renderers arrive with
    // VITA-18 and on; until then a frame is a clear colour and the interface.
    window = win;
    LOG_INFO("Renderer (Vita skeleton): initialised, interface only");
    gl::runShaderSelfTest();  // WOWEE_GL_SELFTEST=1 in env.txt (VITA-14)
    return true;
}

bool Renderer::initializeRenderers([[maybe_unused]] pipeline::AssetManager* assetManager, [[maybe_unused]] const std::string& mapName) { return true; }

bool Renderer::isDeviceLost() const { return false; }

bool Renderer::isOnOutdoorPvpObjective() const { return false; }

bool Renderer::isRecording() const { return false; }

bool Renderer::loadTestTerrain([[maybe_unused]] pipeline::AssetManager* assetManager, [[maybe_unused]] const std::string& adtPath) { return false; }

void Renderer::registerPreview([[maybe_unused]] CharacterPreview* preview) { }

void Renderer::reinitQuestMarkers([[maybe_unused]] pipeline::AssetManager* assets) { }

void reportBlockUploadTally() {}

Renderer::Renderer() {}
Renderer::~Renderer() = default;

const std::string& Renderer::getCurrentZoneName() const {
    static const std::string none;
    return none;
}

std::string Renderer::takeRecordingFailure() { return {}; }

void Renderer::renderHUD() { }

void Renderer::renderWorld([[maybe_unused]] game::World* world, [[maybe_unused]] game::GameHandler* gameHandler) { }

void Renderer::resetCombatVisualState() { }

void Renderer::setActiveMapName([[maybe_unused]] const std::string& name) { }

void Renderer::setCharacterFollow([[maybe_unused]] uint32_t instanceId) { }

void Renderer::setFSR2Enabled([[maybe_unused]] bool enabled) { }

void Renderer::setFSREnabled([[maybe_unused]] bool enabled) { }

void Renderer::setGrassDistance([[maybe_unused]] float yards) { }

void Renderer::setGrassEnabled([[maybe_unused]] bool enabled) { }

void Renderer::setGrassScales([[maybe_unused]] float density, [[maybe_unused]] float height) { }

void Renderer::setMsaaSamples([[maybe_unused]] int samples) { }

void Renderer::setRtLightingMode([[maybe_unused]] int mode) { }

void Renderer::setSelectionCircle([[maybe_unused]] const glm::vec3& pos, [[maybe_unused]] float radius, [[maybe_unused]] const glm::vec3& color) { }

void Renderer::setSharpStars([[maybe_unused]] bool enabled) { }

void Renderer::setViewDistance([[maybe_unused]] float distance) { }

void Renderer::setVolumetricFogQuality([[maybe_unused]] int quality) { }

void Renderer::setWaterRefractionEnabled([[maybe_unused]] bool enabled) { }

void Renderer::shutdown() { }

bool Renderer::startRecording([[maybe_unused]] const std::string& path, [[maybe_unused]] std::string& error) { return false; }

core::ScreenRecorder::Stats Renderer::stopRecording() { return {}; }

void Renderer::unregisterPreview([[maybe_unused]] CharacterPreview* preview) { }

void Renderer::update([[maybe_unused]] float deltaTime) { }

void Renderer::waitIdle([[maybe_unused]] const char* where) { }

void Renderer::waitIdleUnlessLost() { }

}  // namespace wowee::rendering

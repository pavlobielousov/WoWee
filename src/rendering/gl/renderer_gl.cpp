// Vita skeleton of Renderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/renderer.hpp"

#include "core/logger.hpp"
#include "platform/vita/ime_dialog.hpp"
#include "core/window.hpp"
#include "rendering/imgui_backend.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/gl/scene_params.hpp"
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

#include <malloc.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <glm/gtc/matrix_transform.hpp>

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
    // In the world the background is the fog colour, so distant terrain fades into the sky rather than into a dark box.
    if (terrainRenderer) {
        const gl::SceneParams sky;
        glClearColor(sky.fogColor.x, sky.fogColor.y, sky.fogColor.z, 1.0f);
    } else {
        glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
    }
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
    // On demand: a file ux0:data/wowee/shot.cmd (uploaded over FTP) takes a screenshot within half a second and is removed.
    {
        static unsigned tick = 0;
        if (g_shotPath.empty() && ++tick % 30 == 0) {
            SceIoStat info;
            if (sceIoGetstat("ux0:data/wowee/shot.cmd", &info) >= 0) {
                sceIoRemove("ux0:data/wowee/shot.cmd");
                g_shotPath = "ux0:data/wowee/shot.png";
            }
        }
    }
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
    if (frames++ % 300 == 0) {
        const struct mallinfo heap = mallinfo();  // newlib: the whole heap is one arena of WOWEE_VITA_HEAP_MB
        LOG_WARNING("Vita frames presented: ", frames, ", heap in use ", static_cast<unsigned>(heap.uordblks) / (1024 * 1024),
                    " MB of ", static_cast<unsigned>(heap.arena) / (1024 * 1024), " MB arena");
        if (camera && terrainManager) {
            const glm::vec3 e = camera->getPosition();
            const auto h = terrainManager->getHeightAt(e.x, e.y);
            LOG_WARNING("Camera eye (", e.x, ", ", e.y, ", ", e.z, ") ground ", h ? *h : -9999.0f, "");
        }
    }
}

void Renderer::endUploadBatch() { }

void Renderer::endUploadBatchSync() { }

uint32_t Renderer::getCurrentZoneId() const { return 0; }

int Renderer::getMaxMsaaSamples() const { return 0; }

PostProcessPipeline* Renderer::getPostProcessPipeline() const { return nullptr; }

int Renderer::getTerrainLoadRadius() const { return TerrainManager::kVitaMaxLoadRadius; }

const std::vector<std::pair<const char*, double>>& Renderer::gpuTimings() const {
    static const std::vector<std::pair<const char*, double>> none;
    return none;
}

bool Renderer::initialize(core::Window* win) {
    // The window already brought vitaGL up (src/platform/vita/vita_window.cpp).
    window = win;
    camera = std::make_unique<Camera>();
    camera->setPosition(glm::vec3(-8900.0f, -170.0f, 150.0f));
    camera->setRotation(0.0f, -5.0f);
    camera->setAspectRatio(window->getAspectRatio());
    camera->setFov(60.0f);
    cameraController = std::make_unique<CameraController>(camera.get());
    cameraController->setUseWoWSpeed(true);
    cameraController->setMouseSensitivity(0.15f);
    LOG_INFO("Renderer (Vita, GL): camera and controller ready");
    gl::runShaderSelfTest();  // WOWEE_GL_SELFTEST=1 in env.txt (VITA-14)
    return true;
}

// The terrain half of the real initializeRenderers: the GL terrain renderer and upstream's TerrainManager over it. Everything
// else (water, M2, WMO, sky, characters) arrives with VITA-19/20/21.
bool Renderer::initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName) {
    if (!assetManager) return false;
    cachedAssetManager = assetManager;
    if (!terrainRenderer) {
        terrainRenderer = std::make_unique<TerrainRenderer>();
        if (!terrainRenderer->glInitialize(assetManager)) {
            terrainRenderer.reset();
            return false;
        }
    }
    if (!terrainManager) {
        terrainManager = std::make_unique<TerrainManager>();
        if (!terrainManager->initialize(assetManager, terrainRenderer.get())) {
            LOG_ERROR("Failed to initialize terrain manager");
            terrainManager.reset();
            return false;
        }
        if (cameraController) cameraController->setTerrainManager(terrainManager.get());
    }
    setActiveMapName(mapName);
    return true;
}

bool Renderer::isDeviceLost() const { return false; }

bool Renderer::isOnOutdoorPvpObjective() const { return false; }

bool Renderer::isRecording() const { return false; }

bool Renderer::loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath) {
    // "World\\Maps\\<map>\\<map>_<x>_<y>.adt"
    std::string mapName;
    int tileX = 32, tileY = 49;
    const std::size_t sep = adtPath.find_last_of("\\/");
    const std::string file = sep == std::string::npos ? adtPath : adtPath.substr(sep + 1);
    const std::size_t u1 = file.find('_');
    const std::size_t u2 = u1 == std::string::npos ? std::string::npos : file.find('_', u1 + 1);
    const std::size_t dot = u2 == std::string::npos ? std::string::npos : file.find('.', u2);
    if (u1 != std::string::npos && u2 != std::string::npos && dot != std::string::npos) {
        mapName = file.substr(0, u1);
        tileX = std::atoi(file.substr(u1 + 1, u2 - u1 - 1).c_str());
        tileY = std::atoi(file.substr(u2 + 1, dot - u2 - 1).c_str());
    }
    if (!initializeRenderers(assetManager, mapName)) return false;
    LOG_WARNING("Terrain: enqueuing the first tile [", tileX, ",", tileY, "] of '", mapName, "'");
    if (!terrainManager->enqueueTile(tileX, tileY)) return false;
    terrainLoaded = true;
    return true;
}

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

void Renderer::renderWorld([[maybe_unused]] game::World* world, [[maybe_unused]] game::GameHandler* gameHandler) {
    if (!camera || !terrainRenderer) return;
    gl::SceneParams scene;
    scene.view = camera->getViewMatrix();
    // OpenGL clip space: the camera's own projection is Vulkan's (depth 0..1, Y flipped), so build the GL one (DEV_SETUP 22).
    scene.projection = glm::perspectiveRH_NO(glm::radians(camera->getFovDegrees()), camera->getAspectRatio(), 0.25f, 4000.0f);
    scene.cullViewProj = camera->getViewProjectionMatrix();
    scene.eye = camera->getPosition();
    scene.viewDistance = viewDistance_;
    scene.fogStart = viewDistance_ * 0.45f;
    scene.fogEnd = viewDistance_ * 0.95f;
    terrainRenderer->glRender(scene);
}

void Renderer::resetCombatVisualState() { }

void Renderer::setActiveMapName(const std::string& name) {
    if (terrainManager && !name.empty()) terrainManager->setMapName(name);
}

void Renderer::setCharacterFollow(uint32_t instanceId) {
    characterInstanceId = instanceId;
    if (cameraController && instanceId > 0) cameraController->setFollowTarget(&characterPosition);
}

void Renderer::setFSR2Enabled([[maybe_unused]] bool enabled) { }

void Renderer::setFSREnabled([[maybe_unused]] bool enabled) { }

void Renderer::setGrassDistance([[maybe_unused]] float yards) { }

void Renderer::setGrassEnabled([[maybe_unused]] bool enabled) { }

void Renderer::setGrassScales([[maybe_unused]] float density, [[maybe_unused]] float height) { }

void Renderer::setMsaaSamples([[maybe_unused]] int samples) { }

void Renderer::setRtLightingMode([[maybe_unused]] int mode) { }

void Renderer::setSelectionCircle([[maybe_unused]] const glm::vec3& pos, [[maybe_unused]] float radius, [[maybe_unused]] const glm::vec3& color) { }

void Renderer::setSharpStars([[maybe_unused]] bool enabled) { }

void Renderer::setViewDistance(float distance) { viewDistance_ = std::clamp(distance, 200.0f, 2400.0f); }

void Renderer::setVolumetricFogQuality([[maybe_unused]] int quality) { }

void Renderer::setWaterRefractionEnabled([[maybe_unused]] bool enabled) { }

void Renderer::shutdown() {
    if (terrainManager) {
        terrainManager->stopWorkers();
        terrainManager.reset();
    }
    if (terrainRenderer) {
        terrainRenderer->shutdown();
        terrainRenderer.reset();
    }
}

bool Renderer::startRecording([[maybe_unused]] const std::string& path, [[maybe_unused]] std::string& error) { return false; }

core::ScreenRecorder::Stats Renderer::stopRecording() { return {}; }

void Renderer::unregisterPreview([[maybe_unused]] CharacterPreview* preview) { }

void Renderer::update(float deltaTime) {
    if (cameraController) cameraController->update(deltaTime);
    if (terrainManager && camera) terrainManager->update(*camera, deltaTime);
}

void Renderer::waitIdle([[maybe_unused]] const char* where) { }

void Renderer::waitIdleUnlessLost() { }

}  // namespace wowee::rendering

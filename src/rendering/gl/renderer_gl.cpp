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
#include "rendering/gl/scene_target.hpp"
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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace wowee::rendering {

namespace gl { extern bool g_charNoAnim, g_charAnimAll; }

namespace {
// Live profiling switches (VITA-19): ux0:data/wowee/gl.cfg is read every 60 frames, words in it: noterrain nowmo nom2 and
// scale=<0.2..1> (the 3D viewport as a fraction of the screen, to tell fill rate from draw-call cost). Delete the file to
// reset. A diagnostic only; the file is not there in normal use.
struct DebugFlags {
    bool noTerrain = false, noWmo = false, noM2 = false, noChar = false, charNoAnim = false, charAnimAll = false;
    float scale = 1.0f;
};
DebugFlags g_debug;

void pollDebugFlags() {
    static unsigned tick = 0;
    if (++tick % 60 != 0) return;
    DebugFlags f;
    if (FILE* fp = fopen("ux0:data/wowee/gl.cfg", "r")) {
        char buf[256] = {0};
        const size_t n = fread(buf, 1, sizeof buf - 1, fp);
        buf[n] = 0;
        fclose(fp);
        f.noTerrain = strstr(buf, "noterrain") != nullptr;
        f.noWmo = strstr(buf, "nowmo") != nullptr;
        f.noM2 = strstr(buf, "nom2") != nullptr;
        f.noChar = strstr(buf, "nochar") != nullptr;
        f.charNoAnim = strstr(buf, "noanim") != nullptr;
        f.charAnimAll = strstr(buf, "animall") != nullptr;
        if (const char* sc = strstr(buf, "scale=")) f.scale = std::clamp(static_cast<float>(atof(sc + 6)), 0.2f, 1.0f);
    }
    if (f.noTerrain != g_debug.noTerrain || f.noWmo != g_debug.noWmo || f.noM2 != g_debug.noM2 || f.noChar != g_debug.noChar || f.charNoAnim != g_debug.charNoAnim || f.charAnimAll != g_debug.charAnimAll || f.scale != g_debug.scale) {
        LOG_WARNING("gl.cfg: noterrain=", f.noTerrain, " nowmo=", f.noWmo, " nom2=", f.noM2, " nochar=", f.noChar, " noanim=", f.charNoAnim, " animall=", f.charAnimAll, " scale=", f.scale);
    }
    g_debug = f;
    gl::g_charNoAnim = f.charNoAnim;
    gl::g_charAnimAll = f.charAnimAll;
}
}  // namespace

void Renderer::beginFrame() {
    pollDebugFlags();
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
        LOG_WARNING("vitaGL memory (free/total MB): vram ", vglMemFree(VGL_MEM_VRAM) / (1024 * 1024), "/", vglMemTotal(VGL_MEM_VRAM) / (1024 * 1024),
                    " ram ", vglMemFree(VGL_MEM_RAM) / (1024 * 1024), "/", vglMemTotal(VGL_MEM_RAM) / (1024 * 1024),
                    " phycont ", vglMemFree(VGL_MEM_PHYCONT) / (1024 * 1024), "/", vglMemTotal(VGL_MEM_PHYCONT) / (1024 * 1024),
                    " budget ", vglMemFree(VGL_MEM_BUDGET) / (1024 * 1024), "/", vglMemTotal(VGL_MEM_BUDGET) / (1024 * 1024));
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
    // WoWee pans the camera round the area after a few idle seconds; on the Vita that only moves the picture under a test.
    cameraController->setIdleOrbitEnabled(false);
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
    if (!m2Renderer) {
        m2Renderer = std::make_unique<M2Renderer>();
        if (!m2Renderer->glInitialize(assetManager)) {
            LOG_ERROR("M2 renderer (GL) did not start, drawing no doodads");
            m2Renderer.reset();
        }
    }
    if (!waterRenderer) {
        waterRenderer = std::make_unique<WaterRenderer>();
        if (!waterRenderer->glInitialize()) {
            LOG_ERROR("Water renderer (GL) did not start, drawing no water");
            waterRenderer.reset();
        }
    }
    if (!wmoRenderer) {
        wmoRenderer = std::make_unique<WMORenderer>();
        if (!wmoRenderer->glInitialize(assetManager)) {
            LOG_ERROR("WMO renderer (GL) did not start, drawing no buildings");
            wmoRenderer.reset();
        }
    }
    if (!characterRenderer) {
        characterRenderer = std::make_unique<CharacterRenderer>();
        if (!characterRenderer->glInitialize(assetManager)) {
            LOG_ERROR("Character renderer (GL) did not start, drawing no characters");
            characterRenderer.reset();
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
        if (m2Renderer) terrainManager->setM2Renderer(m2Renderer.get());
        if (wmoRenderer) terrainManager->setWMORenderer(wmoRenderer.get());
        if (waterRenderer) terrainManager->setWaterRenderer(waterRenderer.get());
        if (cameraController) {
            if (wmoRenderer) cameraController->setWMORenderer(wmoRenderer.get());
            if (m2Renderer) cameraController->setM2Renderer(m2Renderer.get());
            if (waterRenderer) cameraController->setWaterRenderer(waterRenderer.get());
        }
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
    // The Vita draws a small world: 500 yards by default (WOWEE_VIEW_DISTANCE in env.txt). At the desktop's 1200 almost every
    // chunk of the nine loaded tiles was drawn (about 900 draws) and the draw submission alone took 18 ms of the frame.
    static const float kVitaViewDistance = [] {
        const char* v = std::getenv("WOWEE_VIEW_DISTANCE");
        const float d = v ? static_cast<float>(std::atof(v)) : 500.0f;
        return std::clamp(d, 200.0f, 2400.0f);
    }();
    const float viewDistance = std::min(viewDistance_, kVitaViewDistance);
    scene.viewDistance = viewDistance;
    scene.fogStart = viewDistance * 0.45f;
    scene.fogEnd = viewDistance * 0.95f;
    // The 3D scene is drawn at a fraction of the screen size and scaled up (WOWEE_RENDER_SCALE in env.txt, default 1 = full size;
    // 0.75 gave nothing once the view distance was short). Created on first use: the screen is 960x544 on every Vita.
    static gl::SceneTarget target;
    static bool targetTried = false;
    if (!targetTried) {
        targetTried = true;
        float scale = 1.0f;
        if (const char* v = std::getenv("WOWEE_RENDER_SCALE")) scale = static_cast<float>(std::atof(v));
        target.initialize(960, 544, scale);
    }
    if (target.active()) {
        target.begin();
        glClearColor(scene.fogColor.x, scene.fogColor.y, scene.fogColor.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    if (!g_debug.noTerrain) terrainRenderer->glRender(scene);
    if (!g_debug.noWmo && wmoRenderer && wmoRenderer->glReady()) wmoRenderer->glRender(scene);
    if (!g_debug.noM2 && m2Renderer && m2Renderer->glReady()) m2Renderer->glRender(scene);
    if (!g_debug.noChar && characterRenderer && characterRenderer->glReady()) characterRenderer->glRender(scene);
    if (waterRenderer && waterRenderer->glReady()) {
        static const auto start = std::chrono::steady_clock::now();
        waterRenderer->glRender(scene, std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count());
    }
    if (target.active()) target.end();
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
    // The game screen re-applies the saved setting (default on) every time it is shown: keep the idle orbit off here.
    if (cameraController) cameraController->setIdleOrbitEnabled(false);
    if (wmoRenderer && camera) wmoRenderer->glUpdateCollision(camera->getPosition());
    // No character model yet (VITA-20), so nothing ever attached the controller to a player and it ran as a free camera:
    // no prop collision (that mode skips it), no ground from M2s, and the position the server hears about never moved.
    // Attach it to a position-only character once the world is there, in first person (nothing to look at behind it).
    if (cameraController && camera && terrainManager && !cameraController->getFollowTarget() &&
        terrainManager->getLoadedTileCount() > 0) {
        characterPosition = camera->getPosition() - glm::vec3(0.0f, 0.0f, 1.2f);  // eye height above the feet
        cameraController->setFollowTarget(&characterPosition);
        for (int i = 0; i < 40; ++i) cameraController->processMouseWheel(1.0f);  // all the way in
        LOG_WARNING("Camera attached to a position-only character at (", characterPosition.x, ", ", characterPosition.y, ", ",
                    characterPosition.z, "), first person");
    }
    {
        // Where the collision time goes (VITA-57): every WMO query of the previous frame (camera controller and game logic
        // alike), and the camera controller's own update time.
        static double controllerMs = 0.0, wmoMs = 0.0, m2Ms = 0.0;
        static long wmoCalls = 0, m2Calls = 0;
        static int frames = 0;
        if (wmoRenderer) {
            wmoMs += wmoRenderer->getQueryTimeMs();
            wmoCalls += wmoRenderer->getQueryCallCount();
            wmoRenderer->resetQueryStats();
        }
        if (m2Renderer) {
            m2Ms += m2Renderer->getQueryTimeMs();
            m2Calls += m2Renderer->getQueryCallCount();
            m2Renderer->resetQueryStats();
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (cameraController) cameraController->update(deltaTime);
        controllerMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (++frames == 600) {
            LOG_WARNING("Collision cost per frame: camera controller ", controllerMs / frames, " ms, WMO queries (whole frame) ",
                        wmoMs / frames, " ms in ", static_cast<double>(wmoCalls) / frames, " calls; M2 queries ", m2Ms / frames, " ms in ",
                        static_cast<double>(m2Calls) / frames, " calls, ", m2Renderer ? m2Renderer->getInstanceCount() : 0, " collidable instances");
            controllerMs = wmoMs = m2Ms = 0.0;
            wmoCalls = m2Calls = 0;
            frames = 0;
        }
    }
    if (characterRenderer && characterRenderer->glReady() && camera) characterRenderer->update(deltaTime, camera->getPosition());
    if (terrainManager && camera) terrainManager->update(*camera, deltaTime);
}

void Renderer::waitIdle([[maybe_unused]] const char* where) { }

void Renderer::waitIdleUnlessLost() { }

}  // namespace wowee::rendering

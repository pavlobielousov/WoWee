// Destructors of the renderer classes the shared code owns (VITA-52): nothing to release yet, the GL
// resources arrive with VITA-18/19/20 and their destructors move into those files.
#include "rendering/amd_fsr3_runtime.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/celestial.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/clouds.hpp"
#include "rendering/footprint_renderer.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/lightning.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/minimap.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/skybox.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/starfield.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/weather.hpp"
#include "rendering/wmo_renderer.hpp"

namespace wowee::rendering {

AnimationController::~AnimationController() = default;
Celestial::~Celestial() = default;
CharacterRenderer::~CharacterRenderer() = default;
ChargeEffect::~ChargeEffect() = default;
Clouds::~Clouds() = default;
FootprintRenderer::~FootprintRenderer() = default;
LensFlare::~LensFlare() = default;
Lightning::~Lightning() = default;
Minimap::~Minimap() = default;
MountDust::~MountDust() = default;
PerformanceHUD::~PerformanceHUD() = default;
PostProcessPipeline::~PostProcessPipeline() = default;
QuestMarkerRenderer::~QuestMarkerRenderer() = default;
Skybox::~Skybox() = default;
SkySystem::~SkySystem() = default;
StarField::~StarField() = default;
Weather::~Weather() = default;

}  // namespace wowee::rendering

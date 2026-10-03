// GPU-only classes the shared code never calls into (VITA-52): the Vita's shadow headers declare them opaque
// (tools/vita/gen_shadow.py, _opaque), Renderer owns them by unique_ptr, and these are their constructors and
// destructors. Most stay unused: the Vita renderer does not build the effects they implement (ADR-001 cut list).
#include "rendering/swim_effects.hpp"
#include "rendering/volumetric_fog.hpp"
#include "rendering/sun_shafts.hpp"
#include "rendering/screen_capture.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/render_graph.hpp"
#include "rendering/overlay_system.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/amd_fsr3_runtime.hpp"

namespace wowee::rendering {

SwimEffects::SwimEffects() = default;
SwimEffects::~SwimEffects() = default;
VolumetricFog::VolumetricFog() = default;
VolumetricFog::~VolumetricFog() = default;
SunShafts::SunShafts() = default;
SunShafts::~SunShafts() = default;
ScreenCapture::ScreenCapture() = default;
ScreenCapture::~ScreenCapture() = default;
RtScene::RtScene() = default;
RtScene::~RtScene() = default;
RtLighting::RtLighting() = default;
RtLighting::~RtLighting() = default;
RenderGraph::RenderGraph() = default;
RenderGraph::~RenderGraph() = default;
OverlaySystem::OverlaySystem() = default;
OverlaySystem::~OverlaySystem() = default;
HiZSystem::HiZSystem() = default;
HiZSystem::~HiZSystem() = default;
GrassRenderer::GrassRenderer() = default;
GrassRenderer::~GrassRenderer() = default;
AmdFsr3Runtime::AmdFsr3Runtime() = default;
AmdFsr3Runtime::~AmdFsr3Runtime() = default;

}  // namespace wowee::rendering

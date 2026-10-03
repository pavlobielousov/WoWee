// Vita skeleton of WorldMapFacade (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/world_map/world_map_facade.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/world_map/world_map_facade.hpp"

namespace wowee::rendering::world_map {

struct WorldMapFacade::Impl {};

bool WorldMapFacade::canZoomOut() const { return false; }

void WorldMapFacade::clearFrameRect() { }

bool WorldMapFacade::clickMapPoint([[maybe_unused]] float u, [[maybe_unused]] float v) { return false; }

void WorldMapFacade::closeTaxiMap() { }

int WorldMapFacade::currentContinentIndex() const { return 0; }

std::vector<WorldMapFacade::Landmark> WorldMapFacade::currentLandmarks() const { return {}; }

std::vector<OverlayEntry> WorldMapFacade::currentOverlays() const { return {}; }

uint32_t WorldMapFacade::currentWorldMapAreaId() const { return 0; }

int WorldMapFacade::currentZoneIndex() const { return 0; }

bool WorldMapFacade::hasFrameRect() const { return false; }

bool WorldMapFacade::isOpen() const { return false; }

bool WorldMapFacade::isTaxiMapOpen() const { return false; }

bool WorldMapFacade::mapUVForCanonical([[maybe_unused]] float wowX, [[maybe_unused]] float wowY, [[maybe_unused]] float wowZ, [[maybe_unused]] float& u, [[maybe_unused]] float& v) const { return false; }

void WorldMapFacade::openTaxiMap([[maybe_unused]] std::function<std::vector<uint32_t>(uint32_t)> routeProvider, [[maybe_unused]] std::function<void(uint32_t)> onSelect, [[maybe_unused]] std::function<void()> onClose) { }

void WorldMapFacade::render([[maybe_unused]] const glm::vec3& playerRenderPos, [[maybe_unused]] int screenWidth, [[maybe_unused]] int screenHeight, [[maybe_unused]] float playerYawDeg) { }

void WorldMapFacade::setCorpsePos([[maybe_unused]] bool hasCorpse, [[maybe_unused]] glm::vec3 renderPos) { }

void WorldMapFacade::setFrameRect([[maybe_unused]] float x, [[maybe_unused]] float y, [[maybe_unused]] float w, [[maybe_unused]] float h) { }

void WorldMapFacade::setGraveyardPos([[maybe_unused]] bool hasGraveyard, [[maybe_unused]] glm::vec3 renderPos) { }

void WorldMapFacade::setMapName([[maybe_unused]] const std::string& name) { }

void WorldMapFacade::setPartyDots([[maybe_unused]] std::vector<PartyDot> dots) { }

void WorldMapFacade::setPlayerZoneId([[maybe_unused]] uint32_t zoneId) { }

void WorldMapFacade::setQuestPois([[maybe_unused]] std::vector<QuestPOI> pois) { }

void WorldMapFacade::setRares([[maybe_unused]] std::vector<RareMark> rares) { }

void WorldMapFacade::setServerExplorationMask([[maybe_unused]] const std::vector<uint32_t>& masks, [[maybe_unused]] bool hasData) { }

void WorldMapFacade::setTaxiNodes([[maybe_unused]] std::vector<TaxiNode> nodes) { }

bool WorldMapFacade::showMap([[maybe_unused]] int continentIndex, [[maybe_unused]] int zoneIndex) { return false; }

void WorldMapFacade::showPlayerZone() { }

bool WorldMapFacade::showWorldMapArea([[maybe_unused]] uint32_t worldMapAreaId) { return false; }

bool WorldMapFacade::takeViewChanged() { return false; }

void WorldMapFacade::zoomOutOneLevel() { }

WorldMapFacade::WorldMapFacade() = default;
WorldMapFacade::~WorldMapFacade() = default;

std::string WorldMapFacade::currentMapFolder() const { return {}; }
std::string WorldMapFacade::zoneNameAtMapPoint([[maybe_unused]] float u, [[maybe_unused]] float v) const { return {}; }
std::vector<std::string> WorldMapFacade::continentNames() const { return {}; }
std::vector<std::string> WorldMapFacade::zoneNames([[maybe_unused]] int continentIndex) const { return {}; }

}  // namespace wowee::rendering::world_map

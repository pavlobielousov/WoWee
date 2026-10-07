#pragma once
// Character skin composition for the Vita (VITA-20 phase C): base skin + face/underwear overlays + equipment regions, blended into one RGBA
// atlas on the CPU. The blending and the region table are upstream's (CharacterRenderer::compositeWithRegions in character_renderer.cpp,
// which is Vulkan-bound and not built on the Vita); this file carries them without the GPU upload, so a worker thread can run it.
#include "pipeline/blp_loader.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wowee::rendering::gl {

struct CompositeResult {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    bool ok = false;
};

/// `load` returns a decoded (RGBA8, not block-compressed) BLP, invalid when the file is missing. Thread-safe if `load` is.
CompositeResult compositeCharacterSkin(const std::string& basePath, const std::vector<std::string>& baseLayers,
                                       const std::vector<std::pair<int, std::string>>& regionLayers,
                                       const std::function<pipeline::BLPImage(const std::string&)>& load);

}  // namespace wowee::rendering::gl

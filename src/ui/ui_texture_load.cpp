#include "ui/ui_upload_budget.hpp"
#include "ui/ui_texture_load.hpp"

#include "core/window.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"

namespace wowee::ui {

rendering::UiTexture uploadUiTextureFromBlp(pipeline::AssetManager* assetManager,
                                       const std::string& path,
                                       core::Window* window,
                                       UiTextureLoad* why) {
    const auto fail = [&](UiTextureLoad reason) {
        if (why) *why = reason;
        return rendering::kNoUiTexture;
    };

    if (!assetManager) return fail(UiTextureLoad::NotFound);

    auto blpData = assetManager->readFile(path);
    if (blpData.empty()) return fail(UiTextureLoad::NotFound);

    auto image = pipeline::BLPLoader::load(blpData);
    if (!image.isValid()) return fail(UiTextureLoad::DecodeFailed);

    auto* textures = window ? window->getUiTextureService() : nullptr;
    if (!textures) return fail(UiTextureLoad::NoContext);

    if (why) *why = UiTextureLoad::Ok;
    return textures->upload(image.data.data(), image.width, image.height);
}


rendering::UiTexture cachedIconTexture(
    uint32_t iconId, pipeline::AssetManager* assetManager, core::Window* window,
    const std::unordered_map<uint32_t, std::string>& paths,
    std::unordered_map<uint32_t, rendering::UiTexture>& cache) {
    if (iconId == 0 || !assetManager) return rendering::kNoUiTexture;

    auto cit = cache.find(iconId);
    if (cit != cache.end()) return cit->second;

    // Not cached: the budget is per frame, and an icon that misses it shows
    // blank this frame and is asked for again next one. Caching a null here
    // would blacklist it for the life of the panel.
    if (!claimUiTextureUpload()) return rendering::kNoUiTexture;

    auto pit = paths.find(iconId);
    if (pit == paths.end()) {
        cache[iconId] = rendering::kNoUiTexture;
        return rendering::kNoUiTexture;
    }

    // Cached either way, failures included: the file is either there or it is
    // not, and looking again every frame will not change that.
    rendering::UiTexture ds =
        uploadUiTextureFromBlp(assetManager, pit->second + ".blp", window);
    cache[iconId] = ds;
    return ds;
}

rendering::UiTexture itemIconTexture(uint32_t displayInfoId,
                                pipeline::AssetManager* assetManager,
                                core::Window* window) {
    if (displayInfoId == 0 || !assetManager) return rendering::kNoUiTexture;

    // Shared across the interface: the bags, the action bar, tooltips and the
    // dialogs all draw the same items.
    static std::unordered_map<uint32_t, rendering::UiTexture> cache;
    auto it = cache.find(displayInfoId);
    if (it != cache.end()) return it->second;

    // Deferred rather than cached as a miss: the budget refusing an upload
    // this frame says nothing about the icon.
    if (!claimUiTextureUpload()) return rendering::kNoUiTexture;

    auto dbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!dbc) {
        core::Logger::getInstance().warning(
            "itemIconTexture: ItemDisplayInfo.dbc not loadable for displayInfoId=",
            displayInfoId);
        cache[displayInfoId] = rendering::kNoUiTexture;
        return rendering::kNoUiTexture;
    }

    const int32_t recIdx = dbc->findRecordById(displayInfoId);
    if (recIdx < 0) {
        core::Logger::getInstance().warning(
            "itemIconTexture: displayInfoId=", displayInfoId,
            " not found in ItemDisplayInfo.dbc");
        cache[displayInfoId] = rendering::kNoUiTexture;
        return rendering::kNoUiTexture;
    }

    const auto* layout = pipeline::getActiveDBCLayout()
                             ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo")
                             : nullptr;
    const std::string iconName =
        dbc->getString(static_cast<uint32_t>(recIdx), layout ? (*layout)["InventoryIcon"] : 5);
    if (iconName.empty()) {
        core::Logger::getInstance().warning(
            "itemIconTexture: displayInfoId=", displayInfoId, " recIdx=", recIdx,
            " has empty iconName field");
        cache[displayInfoId] = rendering::kNoUiTexture;
        return rendering::kNoUiTexture;
    }

    const std::string iconPath = "Interface\\Icons\\" + iconName + ".blp";
    UiTextureLoad why{};
    rendering::UiTexture ds = uploadUiTextureFromBlp(assetManager, iconPath, window, &why);
    // Which of the two failures happened is worth saying: a missing file is a
    // gap in the assets, an undecodable one is a file we cannot read.
    if (why == UiTextureLoad::NotFound) {
        core::Logger::getInstance().warning(
            "itemIconTexture: BLP not found at '", iconPath,
            "' (displayInfoId=", displayInfoId, ")");
    } else if (why == UiTextureLoad::DecodeFailed) {
        core::Logger::getInstance().warning(
            "itemIconTexture: BLP decode failed for '", iconPath, "'");
    }
    cache[displayInfoId] = ds;
    return ds;
}

}  // namespace wowee::ui

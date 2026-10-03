#include "rendering/vk_ui_texture_service.hpp"

#include "rendering/vk_context.hpp"

namespace wowee::rendering {

UiTexture VkUiTextureService::upload(const uint8_t* rgba, int width, int height) {
    return toUiTexture(ctx_.uploadImGuiTexture(rgba, width, height));
}

uint32_t VkUiTextureService::generation() const { return ctx_.uiTextureGeneration(); }

void VkUiTextureService::beginUploadBatch() { ctx_.beginUploadBatch(); }

void VkUiTextureService::endUploadBatchSync() { ctx_.endUploadBatchSync(); }

}  // namespace wowee::rendering

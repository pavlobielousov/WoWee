#pragma once

// The renderer's own texture object, as the rest of the client sees it (VITA-12, ADR-001).
//
// Code outside rendering/ gets these from CharacterRenderer::loadTexture() and the composite functions, compares
// them with each other and with the white placeholder, and hands them back (setTextureSlotOverride): it never looks
// inside. So it only needs a name, not a Vulkan type. On the Vulkan renderer GpuTexture IS VkTexture (an alias, so
// nothing about the types or the generated code changes); a build with another renderer defines its own
// (cmake/vita/shadow/rendering/gpu_texture.hpp).

namespace wowee::rendering {

class VkTexture;
using GpuTexture = VkTexture;

/// Logs, once per process, what block-compressed texture upload saved this session (MB uploaded against MB
/// decoded). Called when the application shuts down; a no-op if nothing was uploaded or it has already reported.
void reportBlockUploadTally();

}  // namespace wowee::rendering

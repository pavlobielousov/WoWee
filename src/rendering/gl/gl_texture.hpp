#pragma once
// Textures for the Vita's GL renderer (VITA-15): BLP images uploaded as they are (DXT blocks stay compressed on the GPU),
// with the top mip levels optionally skipped to save memory, and a cache keyed by path.
//
// Needs the patched vitaGL (tools/vita/build_vitagl.sh): the SDK's corrupts compressed uploads (DEV_SETUP section 22). The
// order here is the safe one: every level first, then the filter (vitaGL applies the mip count when MIN_FILTER is set).
#include "pipeline/blp_loader.hpp"

#include <vitaGL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::rendering::gl {

struct GlTexture {
    GLuint id = 0;
    std::size_t bytes = 0;
    int width = 0;
    int height = 0;
};

/// Upload a decoded BLP. `skipMips` drops that many of the largest levels (a compressed image only; the result is never
/// smaller than 4x4). Uses `repeat` wrapping (terrain and doodads tile).
GlTexture uploadBlp(const pipeline::BLPImage& image, int skipMips, bool repeat = true);

/// RGBA8 pixels, no mips, linear, clamped.
GlTexture uploadRgba(const uint8_t* rgba, int width, int height, bool repeat = false);

void deleteTexture(GlTexture& texture);

/// Textures by path. A path that fails to load is remembered (white is returned) so it is not retried every chunk.
class TextureCache {
public:
    void setAssetManager(pipeline::AssetManager* assets) { assets_ = assets; }
    void setSkipMips(int n) { skipMips_ = n; }
    /// The GL texture for a BLP path, loading it on first use. Never 0: a missing file gives the white texture.
    GLuint get(const std::string& path);
    /// A texture decoded elsewhere (a worker thread): keep it under `path` unless one is already cached.
    void adopt(const std::string& path, const pipeline::BLPImage& image);
    GLuint white();
    [[nodiscard]] std::size_t bytes() const { return bytes_; }
    [[nodiscard]] std::size_t count() const { return entries_.size(); }
    void clear();

private:
    pipeline::AssetManager* assets_ = nullptr;
    int skipMips_ = 0;
    std::unordered_map<std::string, GlTexture> entries_;
    GlTexture white_;
    std::size_t bytes_ = 0;
};

}  // namespace wowee::rendering::gl

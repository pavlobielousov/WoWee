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

/// Upload a decoded BLP. A decoded RGBA image (palettised or uncompressed BLP; the block-compressed ones are cut with `skipMips`) is
/// halved until it fits `maxDimension` (0 = as it is), stored as 16 bits a texel (RGB565 when opaque, RGBA4444 otherwise) and given a mip
/// chain: a quarter of the memory of RGBA8 before halving (VITA-20).
/// (Original description follows.) `skipMips` drops that many of the largest levels (a compressed image only; the result is never
/// smaller than 4x4). Uses `repeat` wrapping (terrain and doodads tile).
GlTexture uploadBlp(const pipeline::BLPImage& image, int skipMips, bool repeat = true, int maxDimension = 0);

/// RGBA8 pixels, no mips, linear, clamped.
GlTexture uploadRgba(const uint8_t* rgba, int width, int height, bool repeat = false);

/// The same RGBA8 pixels stored as 4 bits a channel (2 bytes a texel instead of 4): for masks that are smooth and already
/// coarse, such as the terrain's blend maps (VITA-23).
GlTexture uploadRgba4444(const uint8_t* rgba, int width, int height, bool repeat = false);

void deleteTexture(GlTexture& texture);

/// Textures by path. A path that fails to load is remembered (white is returned) so it is not retried every chunk.
class TextureCache {
public:
    void setAssetManager(pipeline::AssetManager* assets) { assets_ = assets; }
    void setSkipMips(int n) { skipMips_ = n; }
    /// Drops the largest mip levels of any block-compressed texture bigger than this (0 = keep all): a 960x544 screen cannot
    /// show a 1024 texture on a doodad, and each level skipped is a quarter of the memory (VITA-23).
    void setMaxDimension(int n) { maxDimension_ = n; }
    /// The GL texture for a BLP path, loading it on first use. Never 0: a missing file gives the white texture.
    GLuint get(const std::string& path);
    /// A texture decoded elsewhere (a worker thread): keep it under `path` unless one is already cached.
    void adopt(const std::string& path, const pipeline::BLPImage& image);
    GLuint white();
    [[nodiscard]] std::size_t bytes() const { return bytes_; }
    [[nodiscard]] std::size_t count() const { return entries_.size(); }
    /// How much of bytes() is decoded RGBA8 (palettised or uncompressed BLPs: the size cap does not touch them) and in how many textures.
    [[nodiscard]] std::size_t rgbaBytes() const { return rgbaBytes_; }
    [[nodiscard]] std::size_t rgbaCount() const { return rgbaCount_; }
    void clear();

private:
    pipeline::AssetManager* assets_ = nullptr;
    int skipMips_ = 0;
    int maxDimension_ = 0;
    [[nodiscard]] int skipFor(const pipeline::BLPImage& image) const;
    std::unordered_map<std::string, GlTexture> entries_;
    GlTexture white_;
    std::size_t bytes_ = 0;
    std::size_t rgbaBytes_ = 0, rgbaCount_ = 0;
};

}  // namespace wowee::rendering::gl

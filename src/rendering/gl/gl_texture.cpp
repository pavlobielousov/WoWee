// See gl_texture.hpp (VITA-15).
#include <cstdlib>
#include "rendering/gl/gl_texture.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"

#include <algorithm>
#include <vector>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace wowee::rendering::gl {

namespace {

GLenum dxtFormat(pipeline::BLPCompression c) {
    switch (c) {
        case pipeline::BLPCompression::DXT1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case pipeline::BLPCompression::DXT3: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        case pipeline::BLPCompression::DXT5: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        default: return 0;
    }
}

}  // namespace

GlTexture uploadBlp(const pipeline::BLPImage& image, int skipMips, bool repeat) {
    GlTexture out;
    if (!image.isValid()) return out;

    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;

    const GLenum format = image.isBlockCompressed() ? dxtFormat(image.compression) : 0;
    if (format != 0) {
        // Drop the largest levels, but never go below one 4x4 block.
        const int levels = static_cast<int>(image.mipmaps.size());
        int first = std::clamp(skipMips, 0, std::max(0, levels - 1));
        while (first > 0 && std::max(1, image.width >> first) < 4 && std::max(1, image.height >> first) < 4) --first;
        const std::size_t blockBytes = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
        int uploaded = 0;
        // Diagnostic (VITA-18): WOWEE_GL_BASE_ONLY=1 uploads just the first level, to tell mip trouble from filtering trouble.
        static const bool baseOnly = std::getenv("WOWEE_GL_BASE_ONLY") != nullptr;
        for (int level = first; level < (baseOnly ? first + 1 : levels); ++level) {
            const int w = std::max(1, image.width >> level);
            const int h = std::max(1, image.height >> level);
            const std::vector<uint8_t>& data = image.mipmaps[static_cast<std::size_t>(level)];
            const std::size_t expected = static_cast<std::size_t>((w + 3) / 4) * static_cast<std::size_t>((h + 3) / 4) * blockBytes;
            if (data.size() < expected) break;  // a truncated chain: use what is there
            if (uploaded == 0) { out.width = w; out.height = h; }
            glCompressedTexImage2D(GL_TEXTURE_2D, uploaded, format, w, h, 0, static_cast<GLsizei>(expected), data.data());
            out.bytes += expected;
            ++uploaded;
        }
        if (uploaded == 0) {
            glDeleteTextures(1, &out.id);
            return GlTexture{};
        }
        // After every level is uploaded (vitaGL applies the mip count when the filter is set).
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, uploaded > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    } else {
        // Palettised and uncompressed BLPs arrive decoded as RGBA8.
        const std::vector<uint8_t>& px = image.data;
        if (px.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
            glDeleteTextures(1, &out.id);
            return GlTexture{};
        }
        out.width = image.width;
        out.height = image.height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        out.bytes = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return out;
}

GlTexture uploadRgba(const uint8_t* rgba, int width, int height, bool repeat) {
    GlTexture out;
    if (!rgba || width <= 0 || height <= 0) return out;
    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    out.width = width;
    out.height = height;
    out.bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    return out;
}

GlTexture uploadRgba4444(const uint8_t* rgba, int width, int height, bool repeat) {
    GlTexture out;
    if (!rgba || width <= 0 || height <= 0) return out;
    std::vector<uint16_t> packed(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    auto nibble = [](uint8_t v) { return static_cast<uint16_t>((v * 15 + 127) / 255); };
    for (std::size_t i = 0; i < packed.size(); ++i) {
        packed[i] = static_cast<uint16_t>((nibble(rgba[i * 4]) << 12) | (nibble(rgba[i * 4 + 1]) << 8) |
                                          (nibble(rgba[i * 4 + 2]) << 4) | nibble(rgba[i * 4 + 3]));
    }
    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, packed.data());
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    out.width = width;
    out.height = height;
    out.bytes = packed.size() * 2;
    return out;
}

void deleteTexture(GlTexture& texture) {
    if (texture.id != 0) glDeleteTextures(1, &texture.id);
    texture = GlTexture{};
}

GLuint TextureCache::white() {
    if (white_.id == 0) {
        const uint8_t px[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
        white_ = uploadRgba(px, 2, 2, true);
        bytes_ += white_.bytes;
    }
    return white_.id;
}

int TextureCache::skipFor(const pipeline::BLPImage& image) const {
    int skip = skipMips_;
    if (maxDimension_ > 0 && image.isBlockCompressed()) {
        const int biggest = std::max(image.width, image.height);
        while ((biggest >> skip) > maxDimension_ && skip + 1 < static_cast<int>(image.mipmaps.size())) ++skip;
    }
    return skip;
}

GLuint TextureCache::get(const std::string& path) {
    auto it = entries_.find(path);
    if (it != entries_.end()) return it->second.id != 0 ? it->second.id : white();
    GlTexture tex;
    if (assets_) {
        const pipeline::BLPImage image = assets_->loadTexture(path, /*keepCompressed=*/true);
        tex = uploadBlp(image, skipFor(image));
        if (tex.id == 0) LOG_WARNING("Texture ", path, ": could not be loaded, drawing white");
    }
    bytes_ += tex.bytes;
    entries_.emplace(path, tex);
    return tex.id != 0 ? tex.id : white();
}

void TextureCache::adopt(const std::string& path, const pipeline::BLPImage& image) {
    if (entries_.count(path)) return;
    GlTexture tex = uploadBlp(image, skipFor(image));
    bytes_ += tex.bytes;
    entries_.emplace(path, tex);
}

void TextureCache::clear() {
    for (auto& e : entries_) deleteTexture(e.second);
    entries_.clear();
    deleteTexture(white_);
    bytes_ = 0;
}

}  // namespace wowee::rendering::gl

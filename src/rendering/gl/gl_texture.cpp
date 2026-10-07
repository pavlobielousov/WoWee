// See gl_texture.hpp (VITA-15).
#include <cstdlib>
#include "rendering/gl/gl_texture.hpp"

#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/asset_manager.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
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

GlTexture uploadBlp(const pipeline::BLPImage& image, int skipMips, bool repeat, int maxDimension, GLuint reuse) {
    GlTexture out;
    if (!image.isValid()) return out;

    // `reuse` is a texture name that already exists (an async placeholder): the image goes into it, and a failure leaves it alone.
    if (reuse != 0) out.id = reuse;
    else glGenTextures(1, &out.id);
    auto discard = [&]() {
        if (reuse == 0) glDeleteTextures(1, &out.id);
        return GlTexture{};
    };
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
        if (uploaded == 0) return discard();
        // After every level is uploaded (vitaGL applies the mip count when the filter is set).
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, uploaded > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    } else {
        // Palettised and uncompressed BLPs arrive decoded as RGBA8.
        const std::vector<uint8_t>& px = image.data;
        if (px.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) return discard();
        // Halve (2x2 box) until the image fits the cap; sizes that are not a power of two stop where they cannot halve evenly.
        std::vector<uint8_t> level(px.begin(), px.begin() + static_cast<std::ptrdiff_t>(image.width) * image.height * 4);
        int w = image.width, h = image.height;
        auto halve = [](const std::vector<uint8_t>& src, int sw, int sh) {
            const int dw = std::max(1, sw / 2), dh = std::max(1, sh / 2);
            std::vector<uint8_t> dst(static_cast<std::size_t>(dw) * dh * 4);
            for (int y = 0; y < dh; ++y) {
                const int y0 = std::min(sh - 1, y * 2), y1 = std::min(sh - 1, y * 2 + 1);
                for (int x = 0; x < dw; ++x) {
                    const int x0 = std::min(sw - 1, x * 2), x1 = std::min(sw - 1, x * 2 + 1);
                    for (int c = 0; c < 4; ++c) {
                        const int sum = src[(static_cast<std::size_t>(y0) * sw + x0) * 4 + c] + src[(static_cast<std::size_t>(y0) * sw + x1) * 4 + c] +
                                        src[(static_cast<std::size_t>(y1) * sw + x0) * 4 + c] + src[(static_cast<std::size_t>(y1) * sw + x1) * 4 + c];
                        dst[(static_cast<std::size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                    }
                }
            }
            return dst;
        };
        while (maxDimension > 0 && std::max(w, h) > maxDimension && w % 2 == 0 && h % 2 == 0 && w > 4 && h > 4) {
            level = halve(level, w, h);
            w /= 2;
            h /= 2;
        }
        bool opaque = true;
        for (std::size_t i = 3; i < level.size() && opaque; i += 4) opaque = level[i] == 255;
        const bool pow2 = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
        out.width = w;
        out.height = h;
        int uploaded = 0;
        std::vector<uint16_t> packed;
        for (;;) {
            packed.resize(static_cast<std::size_t>(w) * h);
            for (std::size_t i = 0; i < packed.size(); ++i) {
                const uint8_t r = level[i * 4], g = level[i * 4 + 1], b = level[i * 4 + 2], a = level[i * 4 + 3];
                if (opaque) packed[i] = static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
                else packed[i] = static_cast<uint16_t>(((r * 15 + 127) / 255) << 12 | ((g * 15 + 127) / 255) << 8 | ((b * 15 + 127) / 255) << 4 | ((a * 15 + 127) / 255));
            }
            if (opaque) glTexImage2D(GL_TEXTURE_2D, uploaded, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, packed.data());
            else glTexImage2D(GL_TEXTURE_2D, uploaded, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, packed.data());
            out.bytes += packed.size() * 2;
            ++uploaded;
            if (!pow2 || (w <= 4 && h <= 4) || w < 2 || h < 2) break;  // a mip chain only for sizes that halve to the end
            level = halve(level, w, h);
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        // After every level is uploaded (vitaGL applies the mip count when the filter is set).
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, uploaded > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
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
    if (async_ && assets_) {
        if (!assets_->fileExists(path)) {
            entries_.emplace(path, tex);  // a missing file: white, remembered
            return white();
        }
        // A placeholder now (a neutral grey, 1x1), the image when a worker has it.
        glGenTextures(1, &tex.id);
        glBindTexture(GL_TEXTURE_2D, tex.id);
        const uint8_t grey[4] = {150, 145, 135, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        entries_.emplace(path, tex);
        ++pending_;
        jobs_.fetch_add(1);
        pipeline::AssetManager* assets = assets_;
        core::ThreadPool::ioWorkers().submit([this, assets, path]() {
            pipeline::BLPImage image = assets->loadTexture(path, /*keepCompressed=*/true);
            {
                std::lock_guard<std::mutex> lock(readyMutex_);
                ready_.emplace_back(path, std::move(image));
            }
            jobs_.fetch_sub(1);
        });
        return tex.id;
    }
    if (assets_) {
        const pipeline::BLPImage image = assets_->loadTexture(path, /*keepCompressed=*/true);
        tex = uploadBlp(image, skipFor(image), true, maxDimension_);
        if (tex.id == 0) LOG_WARNING("Texture ", path, ": could not be loaded, drawing white");
        if (tex.id != 0 && !image.isBlockCompressed()) { rgbaBytes_ += tex.bytes; ++rgbaCount_; }
    }
    bytes_ += tex.bytes;
    entries_.emplace(path, tex);
    return tex.id != 0 ? tex.id : white();
}

void TextureCache::adopt(const std::string& path, const pipeline::BLPImage& image) {
    if (entries_.count(path)) return;
    GlTexture tex = uploadBlp(image, skipFor(image), true, maxDimension_);
    bytes_ += tex.bytes;
    if (tex.id != 0 && !image.isBlockCompressed()) { rgbaBytes_ += tex.bytes; ++rgbaCount_; }
    entries_.emplace(path, tex);
}

void TextureCache::pump(double budgetMs) {
    if (pending_ == 0) return;
    const auto start = std::chrono::steady_clock::now();
    bool first = true;
    for (;;) {
        std::pair<std::string, pipeline::BLPImage> item;
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            if (ready_.empty()) return;
            item = std::move(ready_.front());
            ready_.pop_front();
        }
        auto it = entries_.find(item.first);
        if (it != entries_.end() && pending_ > 0) {
            --pending_;
            const GLuint id = it->second.id;
            GlTexture up = uploadBlp(item.second, skipFor(item.second), true, maxDimension_, id);
            if (up.id != 0) {
                it->second.bytes = up.bytes;
                it->second.width = up.width;
                it->second.height = up.height;
                bytes_ += up.bytes;
                if (!item.second.isBlockCompressed()) { rgbaBytes_ += up.bytes; ++rgbaCount_; }
            } else {
                LOG_WARNING("Texture ", item.first, ": could not be loaded, keeping the placeholder");
            }
        }
        first = false;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs) return;
    }
    (void)first;
}

void TextureCache::waitIdle() {
    while (jobs_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void TextureCache::clear() {
    waitIdle();
    {
        std::lock_guard<std::mutex> lock(readyMutex_);
        ready_.clear();
    }
    pending_ = 0;
    for (auto& e : entries_) deleteTexture(e.second);
    entries_.clear();
    deleteTexture(white_);
    bytes_ = 0;
    rgbaBytes_ = rgbaCount_ = 0;
}

}  // namespace wowee::rendering::gl

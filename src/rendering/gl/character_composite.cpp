// See character_composite.hpp. The pixel code is upstream's (src/rendering/character_renderer.cpp), copied because that file is built only
// with Vulkan; keep the two in step when upstream changes its atlas layout.
#include "rendering/gl/character_composite.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wowee::rendering::gl {

namespace {

constexpr int kBaseTexSize = 256;     // NPC baked texture default
constexpr int kUpscaleTexSize = 512;  // target size for region compositing

std::string normalizeKey(std::string key) {
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

bool isMagentaKeyCandidate(const uint8_t* rgba) {
    const int r = rgba[0], g = rgba[1], b = rgba[2];
    const int rbDelta = (r > b) ? (r - b) : (b - r);
    return r >= 170 && b >= 170 && g <= 120 && r >= g + 70 && b >= g + 70 && rbDelta <= 96;
}

bool shouldApplyMagentaKey(const std::string& p) {
    return p.find("character\\") == 0 || p.find("item\\texturecomponents\\") == 0 || p.find("item\\objectcomponents\\") == 0 ||
           p.find("\\hair") != std::string::npos || p.find("hair") == 0 || p.find("\\cape\\") != std::string::npos || p.find("cape\\") == 0;
}

// The art uses magenta as "nothing here": those pixels become transparent, their colour bled in from the nearest real neighbours.
size_t bleedAndStripMagentaKey(std::vector<uint8_t>& rgba, int width, int height) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) return 0;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> source = rgba;
    std::vector<uint8_t> mask(pixelCount, 0);
    size_t stripped = 0;
    for (size_t p = 0; p < pixelCount; ++p) {
        if (!isMagentaKeyCandidate(&source[p * 4])) continue;
        mask[p] = 1;
        ++stripped;
    }
    if (stripped == 0) return 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t p = static_cast<size_t>(y) * width + x;
            if (!mask[p]) continue;
            uint32_t rSum = 0, gSum = 0, bSum = 0, samples = 0;
            for (int radius = 1; radius <= 8 && samples == 0; ++radius) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= height) continue;
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::abs(dx) != radius && std::abs(dy) != radius) continue;
                        const int nx = x + dx;
                        if (nx < 0 || nx >= width) continue;
                        const size_t np = static_cast<size_t>(ny) * width + nx;
                        if (mask[np]) continue;
                        const size_t ni = np * 4;
                        if (source[ni + 3] == 0) continue;
                        rSum += source[ni + 0];
                        gSum += source[ni + 1];
                        bSum += source[ni + 2];
                        ++samples;
                    }
                }
            }
            const size_t i = p * 4;
            if (samples > 0) {
                rgba[i + 0] = static_cast<uint8_t>(rSum / samples);
                rgba[i + 1] = static_cast<uint8_t>(gSum / samples);
                rgba[i + 2] = static_cast<uint8_t>(bSum / samples);
            } else {
                rgba[i + 0] = rgba[i + 1] = rgba[i + 2] = 0;
            }
            rgba[i + 3] = 0;
        }
    }
    return stripped;
}

void applyMagentaKeyIfNeeded(pipeline::BLPImage& image, const std::string& path) {
    if (!image.isValid() || image.data.empty()) return;
    if (!shouldApplyMagentaKey(normalizeKey(path))) return;
    bleedAndStripMagentaKey(image.data, image.width, image.height);
}

// Alpha-blend `overlay` onto `composite` at (dstX, dstY), each overlay texel written as a scale x scale block (1 = as it is).
void blit(std::vector<uint8_t>& composite, int compW, int compH, const pipeline::BLPImage& overlay, int dstX, int dstY, int scale = 1) {
    if (scale < 1) scale = 1;
    for (int sy = 0; sy < overlay.height; ++sy) {
        for (int sx = 0; sx < overlay.width; ++sx) {
            const size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            const uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;
            for (int dy2 = 0; dy2 < scale; ++dy2) {
                const int dy = dstY + sy * scale + dy2;
                if (dy < 0 || dy >= compH) continue;
                for (int dx2 = 0; dx2 < scale; ++dx2) {
                    const int dx = dstX + sx * scale + dx2;
                    if (dx < 0 || dx >= compW) continue;
                    const size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;
                    if (srcA == 255) {
                        composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                        composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                        composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                        composite[dstIdx + 3] = 255;
                    } else {
                        const float a = srcA / 255.0f, inv = 1.0f - a;
                        for (int c = 0; c < 3; ++c) composite[dstIdx + c] = static_cast<uint8_t>(overlay.data[srcIdx + c] * a + composite[dstIdx + c] * inv);
                        composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
                    }
                }
            }
        }
    }
}

// Nearest-neighbour downscale blit: every Nth texel of the overlay.
void blitDownscale(std::vector<uint8_t>& composite, int compW, int compH, const pipeline::BLPImage& overlay, int dstX, int dstY, int scale) {
    if (scale < 2) { blit(composite, compW, compH, overlay, dstX, dstY); return; }
    const int outW = overlay.width / scale, outH = overlay.height / scale;
    for (int oy = 0; oy < outH; ++oy) {
        const int dy = dstY + oy;
        if (dy < 0 || dy >= compH) continue;
        for (int ox = 0; ox < outW; ++ox) {
            const int dx = dstX + ox;
            if (dx < 0 || dx >= compW) continue;
            const size_t srcIdx = (static_cast<size_t>(oy * scale) * overlay.width + ox * scale) * 4;
            const size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;
            const uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;
            if (srcA == 255) {
                composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                composite[dstIdx + 3] = 255;
            } else {
                const float a = srcA / 255.0f, inv = 1.0f - a;
                for (int c = 0; c < 3; ++c) composite[dstIdx + c] = static_cast<uint8_t>(overlay.data[srcIdx + c] * a + composite[dstIdx + c] * inv);
                composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
            }
        }
    }
}

}  // namespace

CompositeResult compositeCharacterSkin(const std::string& basePath, const std::vector<std::string>& baseLayers,
                                       const std::vector<std::pair<int, std::string>>& regionLayers,
                                       const std::function<pipeline::BLPImage(const std::string&)>& load) {
    CompositeResult out;
    // Region index -> pixel coordinates and size on the 256x256 base atlas (scaled up for larger atlases).
    static constexpr int regionCoords256[][2] = {{0, 0}, {0, 64}, {0, 128}, {128, 0}, {128, 64}, {128, 96}, {128, 160}, {128, 224}};
    static constexpr int regionSizes256[][2] = {{128, 64}, {128, 64}, {128, 32}, {128, 64}, {128, 32}, {128, 64}, {128, 64}, {128, 32}};

    pipeline::BLPImage base = load(basePath);
    if (!base.isValid() || base.data.empty()) return out;
    applyMagentaKeyIfNeeded(base, basePath);

    std::vector<uint8_t> composite;
    int width = base.width, height = base.height;
    // A 256 base (a baked NPC texture) with equipment to place is first doubled, so the equipment lands at its authored 512 coordinates.
    const bool upscaled = (width == kBaseTexSize && height == kBaseTexSize && !regionLayers.empty());
    if (upscaled) {
        width = height = kUpscaleTexSize;
        composite.resize(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t s = (static_cast<size_t>(y / 2) * kBaseTexSize + x / 2) * 4, d = (static_cast<size_t>(y) * width + x) * 4;
                for (int c = 0; c < 4; ++c) composite[d + c] = base.data[s + c];
            }
        }
    } else {
        composite = base.data;
    }

    // Face and underwear overlays.
    for (const std::string& ul : baseLayers) {
        if (ul.empty()) continue;
        pipeline::BLPImage overlay = load(ul);
        if (!overlay.isValid() || overlay.data.empty()) continue;
        applyMagentaKeyIfNeeded(overlay, ul);
        if (overlay.width == width && overlay.height == height) {
            blit(composite, width, height, overlay, 0, 0);
            continue;
        }
        int dstX = 0, dstY = 0;
        const int coordScale = std::max(1, width / 256);
        bool useScale = true;
        std::string p = ul;
        for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (p.find("faceupper") != std::string::npos) { dstX = 0; dstY = 160; }
        else if (p.find("facelower") != std::string::npos) { dstX = 0; dstY = 192; }
        else if (p.find("pelvis") != std::string::npos) { dstX = 128; dstY = 96; }
        else if (p.find("torso") != std::string::npos) { dstX = 128; dstY = 0; }
        else if (p.find("armupper") != std::string::npos) { dstX = 0; dstY = 0; }
        else if (p.find("armlower") != std::string::npos) { dstX = 0; dstY = 64; }
        else if (p.find("hand") != std::string::npos) { dstX = 0; dstY = 128; }
        else if (p.find("foot") != std::string::npos || p.find("feet") != std::string::npos) { dstX = 128; dstY = 224; }
        else if (p.find("legupper") != std::string::npos || p.find("leg") != std::string::npos) { dstX = 128; dstY = 160; }
        else { dstX = (width - overlay.width) / 2; dstY = (height - overlay.height) / 2; useScale = false; }
        if (useScale) { dstX *= coordScale; dstY *= coordScale; }
        blit(composite, width, height, overlay, dstX, dstY, upscaled ? 2 : 1);
    }

    // Equipment regions at explicit coordinates.
    const int scaleX = std::max(1, width / 256), scaleY = std::max(1, height / 256);
    for (const auto& rl : regionLayers) {
        const int idx = rl.first;
        if (idx < 0 || idx >= 8) continue;
        pipeline::BLPImage overlay = load(rl.second);
        if (!overlay.isValid() || overlay.data.empty()) continue;
        applyMagentaKeyIfNeeded(overlay, rl.second);
        const int dstX = regionCoords256[idx][0] * scaleX, dstY = regionCoords256[idx][1] * scaleY;
        const int expectedW = regionSizes256[idx][0] * scaleX, expectedH = regionSizes256[idx][1] * scaleY;
        if (overlay.width == expectedW && overlay.height == expectedH) {
            blit(composite, width, height, overlay, dstX, dstY);
        } else if (overlay.width * 2 == expectedW && overlay.height * 2 == expectedH) {
            blit(composite, width, height, overlay, dstX, dstY, 2);
        } else if (overlay.width > expectedW && overlay.height > expectedH && expectedW > 0 && expectedH > 0) {
            const int ds = std::min(overlay.width / expectedW, overlay.height / expectedH);
            if (ds >= 2) blitDownscale(composite, width, height, overlay, dstX, dstY, ds);
            else blit(composite, width, height, overlay, dstX, dstY);
        } else {
            blit(composite, width, height, overlay, dstX, dstY);
        }
    }

    bleedAndStripMagentaKey(composite, width, height);
    out.rgba = std::move(composite);
    out.width = width;
    out.height = height;
    out.ok = true;
    return out;
}

}  // namespace wowee::rendering::gl

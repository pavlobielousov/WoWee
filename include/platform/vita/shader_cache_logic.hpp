#pragma once

// The pure half of the Vita shader cache (VITA-53): cache keys, the on-disk entry format, which files are stale, and
// the weighted progress estimate of the first-run compile. No GL and no Vita SDK here, so it is unit-tested on the host
// (tools/vita/depcheck/shadercache_test.cpp); src/rendering/gl/shader_cache.cpp is the half that talks to vitaGL.
//
// Why a cache at all: vitaGL compiles GLSL at run time (0.36 s for a terrain shader to over 1 s for a skinned
// character, DEV_SETUP section 19), but loads a precompiled program in 0.05 ms. Each program is compiled once on the
// user's own console and its binary kept in ux0:data/wowee/shaders/<key>.bin.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::platform::vita::shadercache {

/// Bump when the entry layout below changes: every old file then fails to decode and is recompiled.
inline constexpr uint32_t kFormatVersion = 1;

enum class Stage : uint32_t { Vertex = 0, Fragment = 1 };

inline uint64_t fnv1a64(std::string_view data, uint64_t hash = 14695981039346656037ull) {
    for (const unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::string hex16(uint64_t v) {
    static const char* kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4) out[static_cast<std::size_t>(i)] = kDigits[v & 0xF];
    return out;
}

/// The cache key of one shader: its stage, its full source, and the build tag (the vitaGL library and the entry
/// format; a different library serialises differently, DEV_SETUP section 19). Two hashes, different seeds, so a
/// 64-bit collision on the source alone cannot pick the wrong binary.
inline std::string cacheKey(Stage stage, std::string_view source, std::string_view buildTag) {
    const std::string head = std::string(buildTag) + "|v" + std::to_string(kFormatVersion) + "|" +
                             (stage == Stage::Vertex ? "vs" : "fs") + "|";
    const uint64_t a = fnv1a64(source, fnv1a64(head));
    const uint64_t b = fnv1a64(source, fnv1a64(head, 0x9E3779B97F4A7C15ull));
    return hex16(a) + hex16(b).substr(0, 8);
}

inline std::string fileNameForKey(const std::string& key) { return key + ".bin"; }

/// An entry on disk: magic, format version, payload length, payload checksum, payload. A torn or edited file fails to
/// decode and is treated as a miss (the rename in the writer makes a torn file unlikely; the checksum is the belt).
inline std::string encodeEntry(std::string_view payload) {
    std::string out = "WSC1";
    auto put32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    put32(kFormatVersion);
    put32(static_cast<uint32_t>(payload.size()));
    put32(static_cast<uint32_t>(fnv1a64(payload) & 0xFFFFFFFFu));
    out.append(payload);
    return out;
}

inline bool decodeEntry(std::string_view bytes, std::string& payload) {
    if (bytes.size() < 16 || bytes.substr(0, 4) != "WSC1") return false;
    auto get32 = [&bytes](std::size_t at) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
        return v;
    };
    if (get32(4) != kFormatVersion) return false;
    const uint32_t size = get32(8);
    if (bytes.size() != 16u + size) return false;
    const std::string_view body = bytes.substr(16);
    if (get32(12) != static_cast<uint32_t>(fnv1a64(body) & 0xFFFFFFFFu)) return false;
    payload.assign(body);
    return true;
}

/// Files in the cache directory that no manifest entry refers to (left over from an older shader or build tag) and
/// leftovers of an interrupted write (*.tmp). Names only; the caller removes them one by one.
inline std::vector<std::string> staleFiles(const std::vector<std::string>& present,
                                           const std::set<std::string>& referencedKeys) {
    std::vector<std::string> stale;
    for (const std::string& name : present) {
        const bool tmp = name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0;
        const bool bin = name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0;
        if (tmp || !bin || referencedKeys.count(name.substr(0, name.size() - 4)) == 0) stale.push_back(name);
    }
    return stale;
}

/// What a program costs to compile, by kind (device measurements, DEV_SETUP section 19).
enum class ProgramClass { Ui, Plain, Lit, Skinned };

inline double defaultCostMs(ProgramClass c) {
    switch (c) {
        case ProgramClass::Ui: return 330.0;
        case ProgramClass::Plain: return 400.0;
        case ProgramClass::Lit: return 700.0;
        case ProgramClass::Skinned: return 1100.0;
    }
    return 700.0;
}

/// The determinate progress of the first-run compile. Each program starts with its class's measured cost; as programs
/// finish, the estimate of the rest is scaled by how far the real times differ from the guesses (a slower console
/// clock makes everything proportionally slower), so the time left sharpens as the bar fills. A program loaded from the
/// cache costs nothing and moves the bar by its guessed weight, so a half-cached run still ends at 100 %.
class Progress {
public:
    void add(ProgramClass c) { guess_.push_back(defaultCostMs(c)); }
    [[nodiscard]] std::size_t total() const { return guess_.size(); }
    [[nodiscard]] std::size_t done() const { return done_; }

    /// Program number `done()` finished in `actualMs` of compile time (0 for a cache hit).
    void finished(double actualMs, bool compiled) {
        if (done_ >= guess_.size()) return;
        if (compiled) {
            guessCompiled_ += guess_[done_];
            actualCompiled_ += actualMs;
        }
        doneGuess_ += guess_[done_];
        ++done_;
    }

    [[nodiscard]] double fraction() const {
        double all = 0;
        for (double g : guess_) all += g;
        return all > 0 ? std::min(1.0, doneGuess_ / all) : 1.0;
    }

    [[nodiscard]] double remainingMs() const {
        double rest = 0;
        for (std::size_t i = done_; i < guess_.size(); ++i) rest += guess_[i];
        const double scale = guessCompiled_ > 0 ? actualCompiled_ / guessCompiled_ : 1.0;
        return rest * scale;
    }

private:
    std::vector<double> guess_;
    std::size_t done_ = 0;
    double doneGuess_ = 0;
    double guessCompiled_ = 0;
    double actualCompiled_ = 0;
};

}  // namespace wowee::platform::vita::shadercache

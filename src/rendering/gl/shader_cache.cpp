// See shader_cache.hpp (VITA-53).
#include "rendering/gl/shader_cache.hpp"

#include "core/logger.hpp"
#include "platform/vita/shader_cache_logic.hpp"
#include "platform/vita/vita_platform.hpp"

#include <vitaGL.h>

#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#ifndef WOWEE_VITA_GL_TAG
#define WOWEE_VITA_GL_TAG "untagged"
#endif

namespace sc = wowee::platform::vita::shadercache;
namespace fs = std::filesystem;

namespace wowee::rendering::gl {

namespace {

struct ShaderInfo {
    GLenum type = 0;
    std::string source;
    std::string key;
    bool fromCache = false;
    bool compiledByUs = false;
};

std::map<GLuint, ShaderInfo> g_shaders;
std::map<GLuint, std::vector<GLuint>> g_attached;  // program -> shaders
std::set<std::string> g_usedKeys;
ShaderCacheStats g_stats;
bool g_enabled = true;
std::string g_dir;

uint64_t nowUs() { return sceKernelGetProcessTimeWide(); }

sc::Stage stageOf(GLenum type) { return type == GL_VERTEX_SHADER ? sc::Stage::Vertex : sc::Stage::Fragment; }

fs::path entryPath(const std::string& key) { return fs::path(shaderCacheDir()) / sc::fileNameForKey(key); }

bool readEntry(const std::string& key, std::string& payload) {
    std::ifstream in(entryPath(key), std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (sc::decodeEntry(bytes, payload)) return true;
    std::error_code ec;
    fs::remove(entryPath(key), ec);  // torn or from another format: a miss, and gone
    return false;
}

bool writeEntry(const std::string& key, const std::string& payload) {
    const fs::path finalPath = entryPath(key);
    const fs::path tmp = finalPath.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string bytes = sc::encodeEntry(payload);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, finalPath, ec);
    return !ec;
}

}  // namespace

const std::string& shaderCacheDir() {
    if (g_dir.empty()) {
        g_dir = std::string(platform::vita::kAppDir) + "/shaders";
        std::error_code ec;
        fs::create_directories(g_dir, ec);
    }
    return g_dir;
}

void setShaderCacheEnabled(bool enabled) { g_enabled = enabled; }

void clearShaderCache() {
    std::error_code ec;
    for (fs::directory_iterator it(shaderCacheDir(), ec), end; !ec && it != end; it.increment(ec)) {
        fs::remove(it->path(), ec);  // file by file: remove_all never returns on a device path (DEV_SETUP 11)
        ec.clear();
    }
}

void sweepShaderCache() {
    std::vector<std::string> present;
    std::error_code ec;
    for (fs::directory_iterator it(shaderCacheDir(), ec), end; !ec && it != end; it.increment(ec)) {
        present.push_back(it->path().filename().string());
    }
    ec.clear();
    for (const std::string& name : sc::staleFiles(present, g_usedKeys)) {
        fs::remove(fs::path(shaderCacheDir()) / name, ec);
        ec.clear();
    }
}

const ShaderCacheStats& shaderCacheStats() { return g_stats; }

}  // namespace wowee::rendering::gl

// ---- the wrappers: -Wl,--wrap=<name> sends every call of <name> here, __real_<name> is vitaGL's -------------------
extern "C" {

GLuint __real_glCreateShader(GLenum type);
void __real_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void __real_glCompileShader(GLuint shader);
void __real_glAttachShader(GLuint program, GLuint shader);
void __real_glLinkProgram(GLuint program);
void __real_glDeleteShader(GLuint shader);

GLuint __wrap_glCreateShader(GLenum type) {
    using namespace wowee::rendering::gl;
    const GLuint shader = __real_glCreateShader(type);
    if (shader != 0) g_shaders[shader].type = type;
    return shader;
}

void __wrap_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    using namespace wowee::rendering::gl;
    __real_glShaderSource(shader, count, string, length);
    auto it = g_shaders.find(shader);
    if (it == g_shaders.end()) return;
    std::string all;
    for (GLsizei i = 0; i < count; ++i) {
        if (!string[i]) continue;
        if (length && length[i] >= 0) all.append(string[i], static_cast<std::size_t>(length[i]));
        else all.append(string[i]);
    }
    it->second.source = std::move(all);
}

void __wrap_glCompileShader(GLuint shader) {
    using namespace wowee::rendering::gl;
    auto it = g_shaders.find(shader);
    if (!g_enabled || it == g_shaders.end() || it->second.source.empty()) {
        __real_glCompileShader(shader);
        return;
    }
    ShaderInfo& info = it->second;
    info.key = sc::cacheKey(stageOf(info.type), info.source, WOWEE_VITA_GL_TAG);
    g_usedKeys.insert(info.key);
    std::string payload;
    if (readEntry(info.key, payload) && !payload.empty()) {
        // The program that links this shader skips the translator (0.05 ms against 0.3 to 1 s).
        glShaderBinary(1, &shader, 0, payload.data(), static_cast<GLsizei>(payload.size()));
        info.fromCache = true;
        ++g_stats.hits;
        return;
    }
    // A miss: vitaGL compiles at glLinkProgram, so the time is taken there.
    __real_glCompileShader(shader);
    info.compiledByUs = true;
    ++g_stats.compiled;
}

void __wrap_glAttachShader(GLuint program, GLuint shader) {
    using namespace wowee::rendering::gl;
    __real_glAttachShader(program, shader);
    g_attached[program].push_back(shader);
}

void __wrap_glLinkProgram(GLuint program) {
    using namespace wowee::rendering::gl;
    bool anyToCompile = false;
    auto att = g_attached.find(program);
    if (g_enabled && att != g_attached.end()) {
        for (GLuint s : att->second) {
            auto it = g_shaders.find(s);
            if (it != g_shaders.end() && it->second.compiledByUs) anyToCompile = true;
        }
    }
    const uint64_t t0 = nowUs();
    __real_glLinkProgram(program);
    const double ms = static_cast<double>(nowUs() - t0) / 1000.0;
    if (!anyToCompile) return;

    g_stats.compileMs += ms;
    if (ms > g_stats.longestCompileMs) g_stats.longestCompileMs = ms;
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    unsigned stored = 0;
    if (linked) {
        static std::vector<char> buffer(256 * 1024);
        for (GLuint s : att->second) {
            auto it = g_shaders.find(s);
            if (it == g_shaders.end() || !it->second.compiledByUs) continue;
            GLsizei length = 0;
            vglGetShaderBinary(s, static_cast<GLsizei>(buffer.size()), &length, buffer.data());
            if (length > 0 && writeEntry(it->second.key, std::string(buffer.data(), static_cast<std::size_t>(length)))) {
                ++g_stats.stored;
                ++stored;
            } else {
                ++g_stats.failed;
            }
            it->second.compiledByUs = false;  // stored, or not worth retrying for this shader object
        }
    } else {
        ++g_stats.failed;
    }
    LOG_WARNING("Shader cache: program ", program, " compiled in ", ms, " ms, ", stored, " stored",
                linked ? "" : " (LINK FAILED)");
}

void __wrap_glDeleteShader(GLuint shader) {
    using namespace wowee::rendering::gl;
    __real_glDeleteShader(shader);
    // vitaGL keeps a deleted shader alive while it is attached; the id is not reused before it is detached, and a
    // stale entry only costs a few bytes, so the record stays until the process ends.
}

}  // extern "C"

// env.txt support (VITA-6): a Vita has no environment, so the getenv-based debug switches read
// KEY=VALUE lines from ux0:data/wowee/env.txt, applied with setenv at startup.
#include "platform/vita/vita_platform.hpp"

#include "core/env.hpp"

#include <cstdio>
#include <string>

namespace wowee::platform::vita {

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}
}  // namespace

bool parseEnvLine(std::string_view line, std::string& key, std::string& value) {
    line = trim(line);
    if (line.empty() || line.front() == '#') return false;
    if (line.substr(0, 7) == "export ") line = trim(line.substr(7));
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string_view k = trim(line.substr(0, eq));
    if (k.empty()) return false;
    std::string_view v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
    }
    key.assign(k);
    value.assign(v);
    return true;
}

std::vector<std::string> loadEnvFile(const char* path, bool& fileFound) {
    std::vector<std::string> keys;
    fileFound = false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return keys;
    fileFound = true;
    std::string line, key, value;
    auto apply = [&] {
        if (parseEnvLine(line, key, value)) {
            core::setEnvVar(key.c_str(), value.c_str(), true);
            keys.push_back(key);
        }
        line.clear();
    };
    char buf[512];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') apply();
            else if (line.size() < 4096) line.push_back(buf[i]);
        }
    }
    apply();
    std::fclose(f);
    return keys;
}

}  // namespace wowee::platform::vita

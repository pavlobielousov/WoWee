#pragma once

// Path helpers for PS Vita device paths ("ux0:data/x", "uma0:/data/x", "app0:").
//
// libstdc++'s std::filesystem does not know the "<device>:" root: is_absolute() is false, and
// absolute(), canonical() and weakly_canonical() glue the working directory in front, giving
// "app0:/ux0:data/x". remove_all() never returns on a non-empty tree. Create, rename, remove,
// file_size, directory iteration and current_path("ux0:...") are fine (VITA-3 / VITA-35,
// docs/vita/DEPENDENCIES.md gap G2).
//
// Everything here except removeTree() is pure string work, so it is unit-tested on the host
// (tools/vita/depcheck/devpath_test.cpp) as well as on Vita3K. The one spelling produced is
// "<dev>:/<rest>", which is also what current_path() returns after a chdir.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wowee::platform::vita {

// True for "<name>:" with a name of two or more [A-Za-z0-9_] characters (ux0, uma0, app0, imc0,
// host0, ...). Two characters minimum keeps a one-letter DOS drive from matching.
inline bool hasDeviceRoot(std::string_view p) {
    const std::size_t colon = p.find(':');
    if (colon == std::string_view::npos || colon < 2) return false;
    for (std::size_t i = 0; i < colon; ++i) {
        const char c = p[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// Splits "dev:rest" and normalises the rest only: removes ".", empty segments and "..", and clamps
// ".." at the device root. Returns "dev:/a/b" ("dev:/" for the root). Input without a device
// root is returned unchanged.
inline std::string normalizeDevicePath(std::string_view p) {
    if (!hasDeviceRoot(p)) return std::string(p);
    const std::size_t colon = p.find(':');
    std::string out(p.substr(0, colon + 1));
    std::vector<std::string_view> parts;
    std::string_view rest = p.substr(colon + 1);
    std::size_t pos = 0;
    while (pos <= rest.size()) {
        std::size_t next = rest.find('/', pos);
        if (next == std::string_view::npos) next = rest.size();
        const std::string_view seg = rest.substr(pos, next - pos);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        pos = next + 1;
    }
    out += '/';
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

// Makes `p` absolute against `cwd` (a device path, as returned by current_path()) and normalises
// it. A path that already has a device root is only normalised. A leading "/" means the root of
// cwd's device. If neither p nor cwd has a device root there is nothing to anchor to and p is
// returned unchanged.
inline std::string resolveDevicePath(std::string_view p, std::string_view cwd) {
    if (hasDeviceRoot(p)) return normalizeDevicePath(p);
    if (!hasDeviceRoot(cwd)) return std::string(p);
    if (!p.empty() && p.front() == '/') {
        return normalizeDevicePath(std::string(cwd.substr(0, cwd.find(':') + 1)) + std::string(p));
    }
    return normalizeDevicePath(std::string(cwd) + "/" + std::string(p));
}

// The same against the real working directory. Never calls absolute()/canonical().
inline std::filesystem::path resolveDevicePath(const std::filesystem::path& p) {
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    return std::filesystem::path(resolveDevicePath(p.string(), ec ? std::string_view{} : std::string_view(cwd)));
}

// Recursive delete that does not use remove_all(): entries are collected first, then removed
// deepest first, then the root. Returns the number of entries removed; `ec` holds the first
// error (and the walk stops there).
inline std::uintmax_t removeTree(const std::filesystem::path& root, std::error_code& ec) {
    namespace fs = std::filesystem;
    ec.clear();
    std::vector<fs::path> entries;
    const bool isDir = fs::is_directory(root, ec);
    ec.clear();  // a missing path is not an error here: there is nothing to remove
    if (isDir) {
        for (auto it = fs::recursive_directory_iterator(root, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            entries.push_back(it->path());
        }
    }
    if (ec) return 0;
    std::uintmax_t removed = 0;
    // Pre-order listing: reversing puts every child before its parent.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!fs::remove(*it, ec) || ec) return removed;
        ++removed;
    }
    if (fs::remove(root, ec) && !ec) ++removed;  // false with no error: it was not there
    return removed;
}

}  // namespace wowee::platform::vita

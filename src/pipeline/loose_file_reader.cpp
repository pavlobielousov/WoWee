#include "pipeline/loose_file_reader.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <filesystem>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace wowee {
namespace pipeline {

std::vector<uint8_t> LooseFileReader::readFile(const std::string& filesystemPath) {
#ifdef __vita__
    // Straight to sceIo: an ifstream on the Vita's C library stat()s the file it just opened (to size its buffer), and on the memory
    // card every path lookup in a big directory (weapon models, creature skins) is one scan of it: 340 ms each in the log, two per
    // file. This is one (VITA-20).
    const SceUID fd = sceIoOpen(filesystemPath.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return {};
    const SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (end <= 0 || sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) {
        sceIoClose(fd);
        return {};
    }
    std::vector<uint8_t> data(static_cast<size_t>(end));
    size_t done = 0;
    while (done < data.size()) {
        const int n = sceIoRead(fd, data.data() + done, static_cast<SceSize>(data.size() - done));
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    sceIoClose(fd);
    if (done < data.size()) {
        LOG_WARNING("Incomplete read of: ", filesystemPath);
        data.resize(done);
    }
    return data;
#else
    std::ifstream file(filesystemPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    auto size = file.tellg();
    if (size <= 0) {
        return {};
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (!file.good()) {
        LOG_WARNING("Incomplete read of: ", filesystemPath);
        data.resize(static_cast<size_t>(file.gcount()));
    }

    return data;
#endif
}

bool LooseFileReader::fileExists(const std::string& filesystemPath) {
    std::error_code ec;
    return std::filesystem::exists(filesystemPath, ec);
}
} // namespace pipeline
} // namespace wowee

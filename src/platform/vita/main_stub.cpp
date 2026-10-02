// Stub entry point for the first Vita VPK (VITA-4): logs "WoWee Vita" and exits.
// The real logger arm comes with VITA-6; this mirrors tools/vita/devcheck/main.c
// (docs/vita/DEV_SETUP.md section 5).
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kDirPath = "ux0:data/wowee";
constexpr const char* kLogPath = "ux0:data/wowee/wowee.log";

void logLine(const char* text) {
    char line[128];
    int n = std::snprintf(line, sizeof line, "%s\n", text);
    if (n <= 0) return;
    SceUID fd = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, static_cast<unsigned>(n));
        sceIoClose(fd);
    }
}

} // namespace

int main() {
    sceIoMkdir(kDirPath, 0777);
    logLine("WoWee Vita");
    sceKernelExitProcess(0);
    return 0;
}

#include "core/memory_monitor.hpp"
#include "core/logger.hpp"
#include "core/size_utils.hpp"
#include <fstream>
#include <string>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__vita__)
// No <sys/sysinfo.h> and no /proc. The Vita has 512 MB shared with the system; what the client
// may use is its newlib heap (VITA-6), so that is the "total" here. The real budget is VITA-23.
#else
#include <sys/sysinfo.h>
#endif

#ifdef __vita__
extern "C" int _newlib_heap_size_user;  // defined by every Vita executable
#endif

namespace wowee {
namespace core {

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__vita__)
namespace {
size_t readMemAvailableBytesFromProc() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return 0;

    std::string line;
    while (std::getline(meminfo, line)) {
        // /proc/meminfo format: "MemAvailable:  123456789 kB"
        static constexpr size_t kFieldPrefixLen = 13; // strlen("MemAvailable:")
        if (line.rfind("MemAvailable:", 0) != 0) continue;
        std::istringstream iss(line.substr(kFieldPrefixLen));
        size_t kb = 0;
        iss >> kb;
        if (kb > 0) return kb * 1024ull;
        break;
    }
    return 0;
}
} // namespace
#endif // !_WIN32 && !__APPLE__ && !__vita__

MemoryMonitor& MemoryMonitor::getInstance() {
    static MemoryMonitor instance;
    return instance;
}

void MemoryMonitor::initialize() {
    constexpr size_t kOneGB = 1024ull * 1024 * 1024;
    // Fallback if OS API unavailable - 16 GB is a safe conservative estimate
    // that prevents over-aggressive asset caching on unknown hardware.
    constexpr size_t kFallbackRAM = 16 * kOneGB;

#ifdef _WIN32
    ULONGLONG totalKB = 0;
    if (GetPhysicallyInstalledSystemMemory(&totalKB)) {
        totalRAM_ = static_cast<size_t>(totalKB) * 1024ull;
        LOG_INFO("System RAM detected: ", totalRAM_ / kOneGB, " GB");
    } else {
        totalRAM_ = kFallbackRAM;
        LOG_WARNING("Could not detect system RAM, assuming 16GB");
    }
#elif defined(__APPLE__)
    int64_t physmem = 0;
    size_t len = sizeof(physmem);
    if (sysctlbyname("hw.memsize", &physmem, &len, nullptr, 0) == 0) {
        totalRAM_ = static_cast<size_t>(physmem);
        LOG_INFO("System RAM detected: ", totalRAM_ / kOneGB, " GB");
    } else {
        totalRAM_ = kFallbackRAM;
        LOG_WARNING("Could not detect system RAM, assuming 16GB");
    }
#elif defined(__vita__)
    // The heap each Vita executable asks for (src/platform/vita/vita_main.cpp; the client's differs from the others').
    totalRAM_ = static_cast<size_t>(_newlib_heap_size_user);
    LOG_INFO("Vita heap: ", totalRAM_ / (1024 * 1024), " MB");
#else
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        totalRAM_ = static_cast<size_t>(info.totalram) * info.mem_unit;
        LOG_INFO("System RAM detected: ", totalRAM_ / kOneGB, " GB");
    } else {
        totalRAM_ = kFallbackRAM;
        LOG_WARNING("Could not detect system RAM, assuming 16GB");
    }
#endif
}

size_t MemoryMonitor::getAvailableRAM() const {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<size_t>(status.ullAvailPhys);
    }
    return totalRAM_ / 2;
#elif defined(__APPLE__)
    // hw.usermem is a 32-bit kernel sysctl on macOS: on systems with ≥16 GB RAM
    // the value overflows signed int32, truncating to ~2 GB and causing false
    // severe-pressure reports.  Use the Mach VM statistics API instead, which
    // is the same source that Activity Monitor and `vm_stat` use.
    {
        mach_port_t host_port = mach_host_self();
        vm_size_t page_size = 0;
        host_page_size(host_port, &page_size);
        if (page_size > 0) {
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            vm_statistics64_data_t vm_stat;
            if (host_statistics64(host_port, HOST_VM_INFO64,
                                  reinterpret_cast<host_info64_t>(&vm_stat),
                                  &count) == KERN_SUCCESS) {
                // free + inactive pages are reclaimable on demand
                return (static_cast<size_t>(vm_stat.free_count) +
                        static_cast<size_t>(vm_stat.inactive_count)) *
                       static_cast<size_t>(page_size);
            }
        }
    }
    return totalRAM_ / 2;
#elif defined(__vita__)
    return totalRAM_ / 2;  // no way to ask the heap; half is the same guess the other fallbacks make
#else
    // Best source on Linux for reclaimable memory headroom.
    if (size_t memAvailable = readMemAvailableBytesFromProc(); memAvailable > 0) {
        return memAvailable;
    }

    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        // Fallback approximation if /proc/meminfo is unavailable.
        size_t freeBytes = static_cast<size_t>(info.freeram) * info.mem_unit;
        size_t bufferBytes = static_cast<size_t>(info.bufferram) * info.mem_unit;
        size_t available = freeBytes + bufferBytes;
        return (totalRAM_ > 0 && available > totalRAM_) ? totalRAM_ : available;
    }
    return totalRAM_ / 2;  // Fallback: assume 50% available
#endif
}

size_t MemoryMonitor::getRecommendedCacheBudget() const {
    size_t available = getAvailableRAM();
    // Use 50% of available RAM for caches, hard-capped at 16 GB.
    // Arithmetic in 64 bits: `available * 50` overflows a 32-bit size_t above ~85 MB.
    static constexpr uint64_t kHardCapBytes = 16ull * 1024 * 1024 * 1024;  // 16 GB
    uint64_t budget = static_cast<uint64_t>(available) * 50 / 100;
    return clampToSizeT(budget < kHardCapBytes ? budget : kHardCapBytes);
}

bool MemoryMonitor::isMemoryPressure() const {
    size_t available = getAvailableRAM();
    // Memory pressure if < 10% RAM available
    return available < static_cast<size_t>(static_cast<uint64_t>(totalRAM_) * 10 / 100);
}

bool MemoryMonitor::isSevereMemoryPressure() const {
    size_t available = getAvailableRAM();
    // Severe pressure if < 15% RAM available - background workers should
    // pause entirely to avoid OOM-killing other applications.
    return available < static_cast<size_t>(static_cast<uint64_t>(totalRAM_) * 15 / 100);
}

} // namespace core
} // namespace wowee

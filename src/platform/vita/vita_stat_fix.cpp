// stat() of a path inside a directory that does not exist (VITA-18).
//
// newlib turns the sceIo error codes it knows into errno values and everything else into EINVAL. A file under a missing
// directory (ux0:.../PASSIVEDOODADS/SHOVEL/SHOVEL.wom, where SHOVEL/ is not there) comes back as EINVAL, not ENOENT, so
// std::filesystem::exists(path) - which returns false only for "not found" and throws for every other error - throws
// std::filesystem::filesystem_error. 33 places in the client call the throwing overload, and on a worker thread the exception
// is fatal (std::terminate). Linked with -Wl,--wrap=stat (cmake/vita/Vita.cmake, wowee_vita_executable): a stat that fails
// with EINVAL is reported as ENOENT. The first one is printed with the raw sceIo code, to learn which it was.
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>

#include <cerrno>
#include <sys/stat.h>

extern "C" {
int __real_stat(const char* path, struct stat* buffer);

int __wrap_stat(const char* path, struct stat* buffer) {
    const int result = __real_stat(path, buffer);
    if (result != 0 && errno == EINVAL) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            SceIoStat info;
            const int code = path ? sceIoGetstat(path, &info) : 0;
            sceClibPrintf("wowee: stat(%s) failed with EINVAL, sceIoGetstat says 0x%08x: reporting ENOENT\n", path ? path : "(null)",
                          static_cast<unsigned>(code));
        }
        errno = ENOENT;
    }
    return result;
}
}  // extern "C"

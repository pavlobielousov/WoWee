// Throw trace for the Vita client (VITA-23): when a std::length_error, std::bad_alloc, std::logic_error, std::out_of_range or
// similar is thrown, append the call stack at the throw point to ux0:data/wowee/throw_trace.txt. The crash dump of an uncaught
// exception only shows terminate()/abort(), never who threw. Linked with -Wl,--wrap=__cxa_throw (cmake/vita/wowee_client.cmake).
// Resolve the frames with: arm-vita-eabi-addr2line -f -C -e <elf> <0x81000000 + offset> (the offsets are relative to the
// start of the module, printed as "base").
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <typeinfo>

extern "C" {
// The toolchain ships no <unwind.h>; these are the ARM EHABI entry points of libgcc (_URC_NO_REASON = 0, _URC_END_OF_STACK = 5,
// _UVRSC_CORE = 0, _UVRSD_UINT32 = 0, r15 = the instruction pointer).
struct _Unwind_Context;
typedef int _Unwind_Reason_Code;
typedef _Unwind_Reason_Code (*_Unwind_Trace_Fn)(struct _Unwind_Context*, void*);
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn fn, void* data);
int _Unwind_VRS_Get(struct _Unwind_Context* ctx, int regclass, unsigned long regno, int representation, void* valuep);
extern char __executable_start;
void __real___cxa_throw(void* object, std::type_info* type, void (*destructor)(void*));

namespace {

struct Frames {
    unsigned long pc[24];
    int count = 0;
};

_Unwind_Reason_Code collect(struct _Unwind_Context* ctx, void* data) {
    auto* f = static_cast<Frames*>(data);
    if (f->count >= 24) return 5;
    unsigned long ip = 0;
    _Unwind_VRS_Get(ctx, 0, 15, 0, &ip);
    f->pc[f->count++] = ip & ~1ul;
    return 0;
}

bool interesting(const std::type_info* t) {
    // Not invalid_argument or out_of_range: the Lua unit API throws those from std::stoul as control flow, hundreds a second.
    return *t == typeid(std::length_error) || *t == typeid(std::bad_alloc) || *t == typeid(std::domain_error) ||
           *t == typeid(std::bad_array_new_length);
}

}  // namespace

void __wrap___cxa_throw(void* object, std::type_info* type, void (*destructor)(void*)) {
    static bool inside = false;  // the trace itself must not recurse
    static int written = 0;      // a runaway throw must not fill the card
    if (!inside && written < 200 && interesting(type)) {
        ++written;
        inside = true;
        Frames frames;
        _Unwind_Backtrace(&collect, &frames);
        char line[600];
        int n = std::snprintf(line, sizeof line, "THROW %s base=0x%lx frames:", type->name(),
                              reinterpret_cast<unsigned long>(&__executable_start));
        for (int i = 0; i < frames.count && n < static_cast<int>(sizeof line) - 14; ++i) {
            n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " +0x%lx",
                               frames.pc[i] - reinterpret_cast<unsigned long>(&__executable_start));
        }
        if (n < static_cast<int>(sizeof line) - 2) line[n++] = '\n';
        sceClibPrintf("%.*s", n, line);
        const SceUID fd = sceIoOpen("ux0:data/wowee/throw_trace.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, line, static_cast<SceSize>(n));
            sceIoClose(fd);
        }
        inside = false;
    }
    __real___cxa_throw(object, type, destructor);
}
}  // extern "C"

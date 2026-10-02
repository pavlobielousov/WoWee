# PlayStation Vita build (VITA-4). Included from the root CMakeLists.txt when the VitaSDK toolchain
# file is in use (VITA is set by $VITASDK/share/vita.toolchain.cmake). It defines the Vita targets
# itself and the root file returns right after, so none of the desktop targets are configured.
# For now it only builds a stub eboot; the real sources are switched on by later items.

set(WOWEE_PLATFORM_VITA ON)
add_compile_definitions(WOWEE_PLATFORM_VITA=1)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# Everything below cannot or should not build for the Vita. The root file returns before it would
# be configured, but the cache values are set so a later shared include cannot switch them on.
set(WOWEE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WOWEE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(WOWEE_ENABLE_AMD_FSR2 OFF CACHE BOOL "" FORCE)
set(WOWEE_ENABLE_AMD_FSR3_FRAMEGEN OFF CACHE BOOL "" FORCE)
set(WOWEE_BUILD_AMD_FSR3_RUNTIME OFF CACHE BOOL "" FORCE)
# Off for now; the 32-bit cleanup (VITA-5) makes it possible to turn it back on.
set(WOWEE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WOWEE_VITA_TITLEID "WOWE00001" CACHE STRING "Vita title ID (9 characters)")

# -g is required: crash symbolization needs the unstripped ELF of the same build
# (docs/vita/DEV_SETUP.md section 6). CPU flags (armv7-a+simd, NEON, hard float, cortex-a9) are
# already the compiler defaults, so no -mcpu/-mfpu.
foreach(lang C CXX)
    set(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} -O2 -g -ffunction-sections -fdata-sections")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

set(WOWEE_VITA_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/platform/vita)
set(WOWEE_VITA_RES_DIR ${CMAKE_CURRENT_LIST_DIR}/../../resources/vita)

add_executable(wowee ${WOWEE_VITA_SRC_DIR}/main_stub.cpp)
target_link_libraries(wowee pthread
    SceIofilemgr_stub SceLibKernel_stub SceSysmodule_stub)

# GCC 15's __gthread_active_p() tests a weak reference to pthread_cancel; without this std::thread
# throws "Enable multithreading to use std::thread: Not owner" (see tools/vita/depcheck/CMakeLists.txt).
target_link_options(wowee PRIVATE -Wl,-u,pthread_cancel)

# UNSAFE: extended memory and some sysmodules.
vita_create_self(eboot.bin wowee UNSAFE)
vita_create_vpk(wowee.vpk ${WOWEE_VITA_TITLEID} eboot.bin
    VERSION "00.01"
    NAME "WoWee"
    FILE ${WOWEE_VITA_RES_DIR}/sce_sys/icon0.png sce_sys/icon0.png
         ${WOWEE_VITA_RES_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
         ${WOWEE_VITA_RES_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
         ${WOWEE_VITA_RES_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)

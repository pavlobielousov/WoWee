# PlayStation Vita build (VITA-4). Included from the root CMakeLists.txt when the VitaSDK toolchain
# file is in use (VITA is set by $VITASDK/share/vita.toolchain.cmake). It defines the Vita targets
# itself and the root file returns right after, so none of the desktop targets are configured.
# For now it builds the startup path only (VITA-6); the real sources are switched on by later items.

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

# The startup path (VITA-6): the shared main.cpp up to the point where the Application would be
# built, with the logger and the config paths it uses, plus the Vita platform layer. The rest of
# the tree joins as the headless core (VITA-9) and the Vulkan-free UI (VITA-12) land.
set(WOWEE_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
set(WOWEE_VITA_HEAP_MB 192 CACHE STRING "newlib heap in MB (compile-time: newlib reads it before main)")
set(WOWEE_VITA_STACK_MB 4 CACHE STRING "main thread stack in MB")

# include/core/version.hpp, generated the way the root file does (cmake/GitVersion.cmake).
set(WOWEE_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/core/version.hpp)
set(WOWEE_VERSION_SCRIPT_ARGS
    -DSRC_DIR=${WOWEE_ROOT_DIR}
    -DIN_FILE=${WOWEE_ROOT_DIR}/include/core/version.hpp.in
    -DOUT_FILE=${WOWEE_VERSION_HEADER}
    -P ${WOWEE_ROOT_DIR}/cmake/GitVersion.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${WOWEE_VERSION_SCRIPT_ARGS})
add_custom_target(wowee_version ALL
    COMMAND ${CMAKE_COMMAND} ${WOWEE_VERSION_SCRIPT_ARGS}
    BYPRODUCTS ${WOWEE_VERSION_HEADER}
    COMMENT "Resolving version from git"
    VERBATIM)

add_executable(wowee
    ${WOWEE_ROOT_DIR}/src/main.cpp
    ${WOWEE_ROOT_DIR}/src/core/logger.cpp
    ${WOWEE_ROOT_DIR}/src/core/config_paths.cpp
    ${WOWEE_VITA_SRC_DIR}/vita_main.cpp
    ${WOWEE_VITA_SRC_DIR}/vita_cxa_guard.cpp
    ${WOWEE_VITA_SRC_DIR}/vita_env.cpp
    ${WOWEE_VITA_SRC_DIR}/vita_log_sink.cpp)
add_dependencies(wowee wowee_version)
target_include_directories(wowee PRIVATE ${WOWEE_ROOT_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(wowee PRIVATE
    WOWEE_VITA_HEAP_MB=${WOWEE_VITA_HEAP_MB} WOWEE_VITA_STACK_MB=${WOWEE_VITA_STACK_MB})
target_link_libraries(wowee pthread
    SceIofilemgr_stub SceLibKernel_stub SceSysmodule_stub SceNet_stub SceNetCtl_stub ScePower_stub)

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

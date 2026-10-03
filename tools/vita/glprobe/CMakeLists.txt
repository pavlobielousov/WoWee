cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "Build with tools/vita/build.sh (VitaSDK toolchain required)")
endif()
project(glprobe C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# GlProbe (VITA-13): vitaGL, DXT upload, FBO scaling and ImGui, with PASS/FAIL lines in the log.
#   SRC=tools/vita/glprobe BUILD=build-vita/glprobe tools/vita/build.sh
#   tools/vita/vita3k_macos.sh build-vita/glprobe/glprobe.vpk GLPR00001 --seconds 20
set(VITA_TITLEID "GLPR00001")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Same section flags as cmake/vita/Vita.cmake; without them vita-elf-create failed once the probe grew
# ("Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps").
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2 -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O2 -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# The vendored ImGui (1.92), not the SDK's 1.61: it is what WoWee's UI is written against.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/imgui)
add_library(imgui_vgl STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_vgl SYSTEM PUBLIC ${IMGUI_DIR})
# imgui.cpp's default shell functions use fork/exec/waitpid (ImOsOpenInShell), which newlib on the Vita lacks.
target_compile_definitions(imgui_vgl PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
# The stock GLES2-profile backend (shaders, compiled at run time by vitaGL's translator).
target_compile_definitions(imgui_vgl PRIVATE IMGUI_IMPL_OPENGL_ES2)
target_include_directories(imgui_vgl SYSTEM PUBLIC ${IMGUI_DIR}/backends)

# -DVITAGL_CUSTOM=<dir> links a vitaGL built from source (dir holds libvitaGL.a and include/vitaGL.h), e.g. with
# `make HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1`; the SDK's library has neither the shader cache nor a way to drop the
# boot splash. Default: the SDK's libvitaGL.
set(VITAGL_LIB vitaGL)
add_executable(glprobe main.cpp)
if(VITAGL_CUSTOM)
  set(VITAGL_LIB ${VITAGL_CUSTOM}/libvitaGL.a)
  target_include_directories(glprobe BEFORE PRIVATE ${VITAGL_CUSTOM}/include)
  target_compile_definitions(glprobe PRIVATE GLPROBE_CUSTOM_VGL=1)
endif()
target_link_libraries(glprobe imgui_vgl
  ${VITAGL_LIB} vitashark SceShaccCg_stub SceShaccCgExt taihen_stub mathneon
  SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceKernelDmacMgr_stub
  SceCtrl_stub ScePower_stub SceSysmodule_stub SceLibKernel_stub SceIofilemgr_stub
  pthread z m)

vita_create_self(eboot.bin glprobe UNSAFE)
vita_create_vpk(glprobe.vpk ${VITA_TITLEID} eboot.bin
  VERSION 01.00
  NAME "WoWee GlProbe")

cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "Build with tools/vita/build.sh (VitaSDK toolchain required)")
endif()
project(threadcheck C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_TITLEID "THRC00001")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(THREADCHECK_SECTIONS "-ffunction-sections -fdata-sections" CACHE STRING "what the app builds with; empty to drop")
set(THREADCHECK_OPT "-O2" CACHE STRING "optimisation flag (the app uses -O2)")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra ${THREADCHECK_OPT} -g ${THREADCHECK_SECTIONS}")
if(THREADCHECK_SECTIONS)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(threadcheck main.cpp
  ${ROOT}/src/platform/vita/vita_cxa_guard.cpp
  ${ROOT}/src/platform/vita/vita_threads.cpp
  ${ROOT}/src/platform/vita/vita_log_sink.cpp
  ${ROOT}/src/platform/vita/vita_main.cpp
  ${ROOT}/src/platform/vita/vita_env.cpp
  ${ROOT}/src/core/config_paths.cpp
  ${ROOT}/src/core/logger.cpp)
target_include_directories(threadcheck PRIVATE ${ROOT}/include)
target_link_libraries(threadcheck pthread SceIofilemgr_stub SceLibKernel_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub ScePower_stub)
# See tools/vita/depcheck/CMakeLists.txt: std::thread needs this on GCC 15.
target_link_options(threadcheck PRIVATE -Wl,-u,pthread_cancel -Wl,--wrap=pthread_create)

vita_create_self(eboot.bin threadcheck UNSAFE)
vita_create_vpk(threadcheck.vpk ${VITA_TITLEID} eboot.bin
  VERSION 01.00
  NAME "WoWee ThreadCheck")

cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "Build with tools/vita/build.sh (VitaSDK toolchain required)")
endif()
project(depcheck C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_TITLEID "DEPC00001")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O1")

# The vendored Lua exactly as WoWee builds it on desktop: plain C, no LUA_USE_* defines.
set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/lua-5.1.5/src)
file(GLOB LUA_SRC ${LUA_DIR}/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|print)\\.c$")
add_library(lua51 STATIC ${LUA_SRC})
target_include_directories(lua51 SYSTEM PUBLIC ${LUA_DIR})

add_executable(depcheck main.cpp)
target_link_libraries(depcheck lua51 ssl crypto z pthread
  SceIofilemgr_stub SceLibKernel_stub SceSysmodule_stub SceNet_stub SceNetCtl_stub)

vita_create_self(eboot.bin depcheck UNSAFE)
vita_create_vpk(depcheck.vpk ${VITA_TITLEID} eboot.bin
  VERSION 01.00
  NAME "WoWee DepCheck")

# GCC 15's __gthread_active_p() tests a WEAK reference to pthread_cancel. In a static link nothing pulls
# that libpthread.a member in, the reference resolves to 0 and std::thread throws "Enable
# multithreading to use std::thread: Not owner" (std::mutex, condition_variable and raw pthread_create
# are unaffected). Referencing the symbol strongly fixes it. WoWee's Vita CMake (VITA-4) needs the same.
target_link_options(depcheck PRIVATE -Wl,-u,pthread_cancel)

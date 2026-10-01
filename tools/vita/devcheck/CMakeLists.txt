cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "Build with tools/vita/build.sh (VitaSDK toolchain required)")
endif()
project(devcheck C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_TITLEID "DEVC00001")
# -g keeps symbols in the ELF; vita-parse-core needs the unstripped ELF of THIS build.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0")

add_executable(devcheck main.c)
target_link_libraries(devcheck SceCtrl_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub SceIofilemgr_stub SceLibKernel_stub)

vita_create_self(eboot.bin devcheck UNSAFE)
vita_create_vpk(devcheck.vpk ${VITA_TITLEID} eboot.bin
  VERSION 01.00
  NAME "WoWee DevCheck")

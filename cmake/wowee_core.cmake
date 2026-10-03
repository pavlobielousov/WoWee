# wowee_core (VITA-47): the part of the client that needs no renderer, window or UI toolkit:
# auth, network, game logic, asset loaders, audio managers, the clean part of core/. It is the
# library the headless client (VITA-48) and the Vita build link; the desktop client is unchanged
# and does not use it.
#
# Included by the root CMakeLists.txt (desktop, when -DWOWEE_BUILD_CORE=ON) and by
# cmake/vita/Vita.cmake (always). It gets its sources by directory, not from WOWEE_SOURCES, so
# the Vita world (which never sees that list) and the desktop agree, and a file upstream adds under
# one of these directories joins the core and fails the link check below if it reaches the renderer.
#
# Inputs (set before including):
#   WOWEE_CORE_NULL_AUDIO  ON = build src/platform/vita/audio_engine_null.cpp (a silent AudioEngine)
#                          instead of src/audio/audio_engine.cpp (miniaudio). Always ON on the Vita.
#   WOWEE_PLATFORM_VITA    ON on the Vita: Warden is stubbed out (src/platform/vita/warden_stub.cpp).
# Defines: the wowee_core library and the wowee_core_link_check executable.

set(WOWEE_CORE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

set(WOWEE_CORE_SOURCES)
foreach(dir auth network math pipeline game audio)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${WOWEE_CORE_ROOT}/src/${dir}/*.cpp)
    list(APPEND WOWEE_CORE_SOURCES ${_sources})
endforeach()

# core/ is not one layer: most of it is the application shell. These are the files that need
# nothing from it.
foreach(file
        src/core/logger.cpp
        src/core/config_paths.cpp
        src/core/memory_monitor.cpp
        src/core/app_clock.cpp)
    list(APPEND WOWEE_CORE_SOURCES ${WOWEE_CORE_ROOT}/${file})
endforeach()

# Found clean by VITA-44: they live in rendering/ and ui/ but need only the standard library, glm,
# the logger and the DBC loader. The carve is by source file, not by directory.
foreach(file
        src/rendering/animation/animation_ids.cpp
        src/rendering/animation/emote_registry.cpp
        src/ui/framexml_takeover.cpp)
    list(APPEND WOWEE_CORE_SOURCES ${WOWEE_CORE_ROOT}/${file})
endforeach()

# Definitions the core needs that live in files it does not have (see each file's header).
list(APPEND WOWEE_CORE_SOURCES
    ${WOWEE_CORE_ROOT}/src/platform/headless/stb_image_impl.cpp
    ${WOWEE_CORE_ROOT}/src/platform/headless/cvar_defaults.cpp)

if(WOWEE_CORE_NULL_AUDIO)
    list(REMOVE_ITEM WOWEE_CORE_SOURCES ${WOWEE_CORE_ROOT}/src/audio/audio_engine.cpp)
    list(APPEND WOWEE_CORE_SOURCES ${WOWEE_CORE_ROOT}/src/platform/vita/audio_engine_null.cpp)
endif()

if(WOWEE_PLATFORM_VITA)
    list(REMOVE_ITEM WOWEE_CORE_SOURCES
        ${WOWEE_CORE_ROOT}/src/game/warden_module.cpp
        ${WOWEE_CORE_ROOT}/src/game/warden_emulator.cpp)
    list(APPEND WOWEE_CORE_SOURCES ${WOWEE_CORE_ROOT}/src/platform/vita/warden_stub.cpp)
endif()

# Objects first, so the link check can use them directly (a static library would only pull in the
# objects something asks for, which is exactly what the check must not rely on).
add_library(wowee_core_objects OBJECT ${WOWEE_CORE_SOURCES})
target_include_directories(wowee_core_objects PUBLIC
    ${WOWEE_CORE_ROOT}/include
    ${WOWEE_CORE_ROOT}/src
    ${CMAKE_BINARY_DIR}/generated)
target_include_directories(wowee_core_objects SYSTEM PUBLIC ${WOWEE_CORE_ROOT}/extern)
if(TARGET wowee_version)
    add_dependencies(wowee_core_objects wowee_version)
endif()

add_library(wowee_core STATIC $<TARGET_OBJECTS:wowee_core_objects>)
target_include_directories(wowee_core PUBLIC
    ${WOWEE_CORE_ROOT}/include
    ${WOWEE_CORE_ROOT}/src
    ${CMAKE_BINARY_DIR}/generated)

# What the core needs from the platform. Set by the includer: WOWEE_CORE_LIBS is a list of
# libraries (OpenSSL, zlib, threads, ... or their Vita equivalents).
if(WOWEE_CORE_LIBS)
    target_link_libraries(wowee_core_objects PUBLIC ${WOWEE_CORE_LIBS})
    target_link_libraries(wowee_core PUBLIC ${WOWEE_CORE_LIBS})
endif()

# Every object, not only the ones a program happens to reach: a symbol the core needs but cannot
# find (a definition left behind in rendering/ or ui/) is a link error here, however clean the
# headers look. No ctest target: it is a build check, and the link is the test.
add_executable(wowee_core_link_check ${WOWEE_CORE_ROOT}/cmake/wowee_core_link_check.cpp
    $<TARGET_OBJECTS:wowee_core_objects>)
target_include_directories(wowee_core_link_check PRIVATE ${WOWEE_CORE_ROOT}/include)
if(WOWEE_CORE_LIBS)
    target_link_libraries(wowee_core_link_check PRIVATE ${WOWEE_CORE_LIBS})
endif()

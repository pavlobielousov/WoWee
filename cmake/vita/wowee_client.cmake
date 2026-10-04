# wowee_client (VITA-52): the shared application shell on the Vita. src/core and src/ui compiled as they are
# against the Vita's shadow headers (cmake/vita/shadow, ADR-001), the Vulkan-free half of src/rendering, the
# Vita's own core::Window and renderer skeletons (src/platform/vita/vita_window.cpp, src/rendering/gl/), over
# wowee_core. Included by cmake/vita/Vita.cmake after wowee_core. See docs/vita/DEV_SETUP.md section 20.
#
# Sources are listed by directory with explicit exclusions, so a file upstream adds to src/core or src/ui joins
# the Vita build and shows up as a compile or link error naming it, not as a silent gap.

find_package(SDL3 CONFIG REQUIRED)

# The vendored ImGui (1.92), not the SDK's 1.61: it is what WoWee's UI is written against (GlProbe, DEV_SETUP 19).
set(WOWEE_IMGUI_DIR ${WOWEE_ROOT_DIR}/extern/imgui)
add_library(imgui_vita STATIC
    ${WOWEE_IMGUI_DIR}/imgui.cpp
    ${WOWEE_IMGUI_DIR}/imgui_draw.cpp
    ${WOWEE_IMGUI_DIR}/imgui_tables.cpp
    ${WOWEE_IMGUI_DIR}/imgui_widgets.cpp
    ${WOWEE_IMGUI_DIR}/imgui_demo.cpp
    ${WOWEE_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${WOWEE_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_vita SYSTEM PUBLIC ${WOWEE_IMGUI_DIR} ${WOWEE_IMGUI_DIR}/backends)
# imgui.cpp's default shell functions use fork/exec/waitpid, which newlib on the Vita lacks.
target_compile_definitions(imgui_vita PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_definitions(imgui_vita PRIVATE IMGUI_IMPL_OPENGL_ES2)
target_link_libraries(imgui_vita PUBLIC SDL3::SDL3)

# --- sources -------------------------------------------------------------------------------------------------
set(WOWEE_CLIENT_SOURCES)
file(GLOB _core_shell CONFIGURE_DEPENDS ${WOWEE_ROOT_DIR}/src/core/*.cpp)
file(GLOB_RECURSE _ui CONFIGURE_DEPENDS ${WOWEE_ROOT_DIR}/src/ui/*.cpp)
file(GLOB_RECURSE _addons CONFIGURE_DEPENDS ${WOWEE_ROOT_DIR}/src/addons/*.cpp)
list(APPEND WOWEE_CLIENT_SOURCES ${_core_shell} ${_ui} ${_addons})

# Replaced on the Vita: the desktop window (Vulkan surface; vita_window.cpp is the Vita's) and the detached map
# window (a second OS window; map_window_stub.cpp never opens one), and open_url.cpp (spawns a browser; the Vita has none).
list(REMOVE_ITEM WOWEE_CLIENT_SOURCES
    ${WOWEE_ROOT_DIR}/src/core/window.cpp
    ${WOWEE_ROOT_DIR}/src/ui/map_window.cpp
    ${WOWEE_ROOT_DIR}/src/core/open_url.cpp)
# Already in wowee_core.
list(REMOVE_ITEM WOWEE_CLIENT_SOURCES ${WOWEE_CORE_SOURCES})

# The Vulkan-free half of src/rendering: files that compile against the shadow headers as they are
# (tools/vita/shadow_check.sh lists what does not). The GPU half is src/rendering/gl/.
set(WOWEE_CLIENT_RENDERING_SOURCES
    animation/activity_fsm.cpp
    animation/anim_capability_probe.cpp
    animation/animation_manager.cpp
    animation/character_animator.cpp
    animation/combat_fsm.cpp
    animation/locomotion_fsm.cpp
    animation/mount_fsm.cpp
    animation/sfx_state_driver.cpp
    camera.cpp
    camera_controller.cpp
    frustum.cpp
    levelup_effect.cpp
    lighting_manager.cpp
    loot_sparkles.cpp
    m2_model_classifier.cpp
    m2_renderer_collision.cpp
    material.cpp
    normal_map.cpp
    polygon_triangulate.cpp
    renderer_player_pose.cpp
    renderer_screen_effects.cpp
    renderer_spell_visuals.cpp
    renderer_transport_targets.cpp
    spell_visual_system.cpp
    terrain_manager.cpp
    wmo_renderer_collision.cpp
    zone_ambience.cpp
    world_map/coordinate_projection.cpp
    world_map/data_repository.cpp
    world_map/exploration_state.cpp
    world_map/input_handler.cpp
    world_map/layers/coordinate_display.cpp
    world_map/layers/party_dot_layer.cpp
    world_map/layers/poi_marker_layer.cpp
    world_map/layers/quest_poi_layer.cpp
    world_map/layers/rare_tracker_layer.cpp
    world_map/layers/subzone_tooltip_layer.cpp
    world_map/layers/taxi_node_layer.cpp
    world_map/map_resolver.cpp
    world_map/overlay_renderer.cpp
    world_map/view_state_machine.cpp
    world_map/zone_metadata.cpp)
list(TRANSFORM WOWEE_CLIENT_RENDERING_SOURCES PREPEND ${WOWEE_ROOT_DIR}/src/rendering/)
list(REMOVE_ITEM WOWEE_CLIENT_RENDERING_SOURCES ${WOWEE_CORE_SOURCES})

file(GLOB _gl CONFIGURE_DEPENDS ${WOWEE_ROOT_DIR}/src/rendering/gl/*.cpp)

list(APPEND WOWEE_CLIENT_SOURCES ${WOWEE_CLIENT_RENDERING_SOURCES} ${_gl}
    ${WOWEE_VITA_SRC_DIR}/vita_window.cpp
    ${WOWEE_VITA_SRC_DIR}/map_window_stub.cpp
    ${WOWEE_VITA_SRC_DIR}/open_url_vita.cpp
    ${WOWEE_VITA_SRC_DIR}/process_shim.cpp
    ${WOWEE_VITA_SRC_DIR}/vita_ime.cpp
    ${WOWEE_VITA_SRC_DIR}/vita_throw_trace.cpp)

# The shadow headers come FIRST on the include path (they hide upstream's renderer headers), so this is a
# separate OBJECT library that links wowee_vita_shadow before anything else.
add_library(wowee_client_objects OBJECT ${WOWEE_CLIENT_SOURCES})
# BEFORE on the target itself: an INTERFACE directory of a linked target would come after include/.
target_include_directories(wowee_client_objects BEFORE PRIVATE ${WOWEE_ROOT_DIR}/cmake/vita/shadow)
target_include_directories(wowee_client_objects PUBLIC
    ${WOWEE_ROOT_DIR}/include ${WOWEE_ROOT_DIR}/src ${CMAKE_BINARY_DIR}/generated)
target_include_directories(wowee_client_objects SYSTEM PUBLIC
    ${WOWEE_ROOT_DIR}/extern ${WOWEE_ROOT_DIR}/extern/lua-5.1.5/src)
target_link_libraries(wowee_client_objects PUBLIC imgui_vita)
add_dependencies(wowee_client_objects wowee_version)

# The Lua 5.1 the addon API runs on: the vendored copy (the SDK has LuaJIT only; verified in VITA-3).
set(_lua_names lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm
    lundump lvm lzio lauxlib lbaselib ldblib liolib lmathlib loslib ltablib lstrlib linit)
set(_lua)
foreach(_n ${_lua_names})
    list(APPEND _lua ${WOWEE_ROOT_DIR}/extern/lua-5.1.5/src/${_n}.c)
endforeach()
add_library(lua51_vita STATIC ${_lua})
target_compile_options(lua51_vita PRIVATE -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(lua51_vita SYSTEM PUBLIC ${WOWEE_ROOT_DIR}/extern/lua-5.1.5/src)

# vitaGL and what it needs (the link line GlProbe proved on the device). -DVITAGL_CUSTOM=<dir> links a vitaGL built
# by tools/vita/build_vitagl.sh (dir holds libvitaGL.a and include/vitaGL.h): the SDK's copy shows a boot splash that
# holds the first frame for about a second (VITA-53).
set(WOWEE_VITAGL_LIB vitaGL)
set(WOWEE_VITAGL_FILE "$ENV{VITASDK}/arm-vita-eabi/lib/libvitaGL.a")
# The client needs the patched build: the SDK's vitaGL samples DXT textures as zero depending on upload history (DEV_SETUP
# section 22). Default to build-vita/vitagl; configure fails with the fix if it is missing.
if(NOT VITAGL_CUSTOM AND NOT WOWEE_ALLOW_SDK_VITAGL)
    if(EXISTS "${CMAKE_SOURCE_DIR}/build-vita/vitagl/libvitaGL.a")
        set(VITAGL_CUSTOM "build-vita/vitagl")
    else()
        message(FATAL_ERROR "wowee_client needs the patched vitaGL: run tools/vita/build_vitagl.sh first "
                            "(or pass -DWOWEE_ALLOW_SDK_VITAGL=ON to build with the SDK's, which breaks DXT textures).")
    endif()
endif()
if(VITAGL_CUSTOM)
    get_filename_component(_vgl_dir "${VITAGL_CUSTOM}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(WOWEE_VITAGL_LIB ${_vgl_dir}/libvitaGL.a)
    set(WOWEE_VITAGL_FILE ${_vgl_dir}/libvitaGL.a)
    target_include_directories(wowee_client_objects BEFORE PRIVATE ${_vgl_dir}/include)
endif()
set(WOWEE_VITA_GL_LIBS
    ${WOWEE_VITAGL_LIB} vitashark SceShaccCg_stub SceShaccCgExt taihen_stub mathneon
    SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceKernelDmacMgr_stub
    SceCtrl_stub SceIme_stub ScePower_stub SceSysmodule_stub SceLibKernel_stub SceIofilemgr_stub m)

# WOWEE_VITA_CLIENT turns main.cpp's Application arm on (src/main.cpp).
add_executable(wowee_client
    ${WOWEE_ROOT_DIR}/src/main.cpp
    $<TARGET_OBJECTS:wowee_client_objects>
    ${WOWEE_VITA_PLATFORM_SOURCES})
target_compile_definitions(wowee_client PRIVATE WOWEE_VITA_CLIENT=1)
target_link_libraries(wowee_client PRIVATE wowee_vita_shadow wowee_core imgui_vita lua51_vita
    ${WOWEE_CORE_LIBS} ${WOWEE_VITA_GL_LIBS} SDL3::SDL3)
# The client's heap: 288 MB, which only starts under the extended memory mode (ATTRIBUTE2=12 in the VPK below). Without that
# attribute, anything above about 200 MB makes the app fail to start; with it, vitaGL also gets a 26 MB RAM pool (13 MB before).
set(WOWEE_VITA_CLIENT_HEAP_MB 288 CACHE STRING "newlib heap of wowee_client in MB (needs the extended memory mode)")
wowee_vita_executable(wowee_client ${WOWEE_VITA_CLIENT_HEAP_MB})

# The shader cache (VITA-53, src/rendering/gl/shader_cache.cpp) sits in front of vitaGL's shader calls. The build tag is
# a hash of the vitaGL library the client links, so a different library (its binaries are not interchangeable,
# DEV_SETUP section 19) never reads another's cache entries.
file(SHA256 "${WOWEE_VITAGL_FILE}" _vgl_hash)
string(SUBSTRING "${_vgl_hash}" 0 16 WOWEE_VITA_GL_TAG)
set_source_files_properties(${WOWEE_ROOT_DIR}/src/rendering/gl/shader_cache.cpp PROPERTIES
    COMPILE_DEFINITIONS "WOWEE_VITA_GL_TAG=\"${WOWEE_VITA_GL_TAG}\"")
# vita-elf-create appends the SCE import data (about 4.7 KB) after the text segment and fails with "Cannot allocate N bytes
# for SCE data at end of segment 0; segment 1 overlaps" when the data segment (aligned to 64 KB) starts less than that after
# the end of the code and constants: a coin flip on the code size (GlProbe hit it; VITA-17's few hundred lines did; a
# larger page alignment turns it into "overlapping sections" instead). The padding moves the end of the segment. When the
# error comes back after code growth, raise WOWEE_VITA_TEXT_PAD_KB by 16 (or 32), or lower it, until the link passes.
set(WOWEE_VITA_TEXT_PAD_KB 56 CACHE STRING "padding in the read-only segment of wowee_client, in KB (see CMake comment)")
target_sources(wowee_client PRIVATE ${WOWEE_VITA_SRC_DIR}/vita_text_pad.cpp)
target_compile_definitions(wowee_client PRIVATE WOWEE_VITA_TEXT_PAD_KB=${WOWEE_VITA_TEXT_PAD_KB})
target_link_options(wowee_client PRIVATE -Wl,-u,wowee_vita_text_pad)
# Throw trace (src/platform/vita/vita_throw_trace.cpp): log the stack of length_error, bad_alloc and friends at the throw.
target_link_options(wowee_client PRIVATE -Wl,--wrap=__cxa_throw)
target_link_options(wowee_client PRIVATE
    -Wl,--wrap=glCreateShader -Wl,--wrap=glShaderSource -Wl,--wrap=glCompileShader
    -Wl,--wrap=glAttachShader -Wl,--wrap=glLinkProgram -Wl,--wrap=glDeleteShader)

# The VPK (own title ID, so it installs beside wowee.vpk and wowee_headless.vpk). vita_create_vpk is a macro that
# leaks its file list into the next call (see Vita.cmake): reset first.
set(WOWEE_VITA_CLIENT_TITLEID "WOWC00001" CACHE STRING "Title ID of the application shell build (9 characters)")
unset(VITA_PACK_VPK_FLAGS)
unset(VITA_MKSFOEX_FLAGS)
unset(resources)
# Extended memory mode (VITA-23): without it the app cannot get a newlib heap above about 200 MB (208 MB failed to start,
# measured). ATTRIBUTE2=12 in param.sfo, which needs the UNSAFE flag the eboot already has.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_self(client.bin wowee_client UNSAFE)
vita_create_vpk(wowee_client.vpk ${WOWEE_VITA_CLIENT_TITLEID} client.bin
    VERSION "00.01"
    NAME "WoWee Client"
    FILE ${WOWEE_VITA_RES_DIR}/sce_sys/icon0.png sce_sys/icon0.png
         ${WOWEE_VITA_RES_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
         ${WOWEE_VITA_RES_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
         ${WOWEE_VITA_RES_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
         # Files the client opens relative to its own folder (app0:assets/...): the login backdrop and the window icon.
         ${WOWEE_ROOT_DIR}/assets/krayonsignin.png assets/krayonsignin.png
         ${WOWEE_ROOT_DIR}/assets/krayonload.png assets/krayonload.png
         ${WOWEE_ROOT_DIR}/assets/Wowee.png assets/Wowee.png
         ${WOWEE_ROOT_DIR}/assets/grass_biomes.json assets/grass_biomes.json)

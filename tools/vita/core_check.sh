#!/bin/sh
# Regression check for wowee_core (VITA-47) on the desktop, in the desktop-builder container:
# 1. every object of wowee_core compiles and links into an executable (wowee_core_link_check: an
#    undefined reference means the core needs a definition that lives in rendering/ui/addons);
# 2. none of the objects reaches a Vulkan, SDL, ImGui, VMA or Lua header (checked from ninja's
#    dependency records, so it sees headers however they were included).
#   tools/vita/core_check.sh                 # real AudioEngine (miniaudio)
#   NULL_AUDIO=1 tools/vita/core_check.sh    # the silent AudioEngine, as the Vita builds it
# Uses its own build directory (build-core, or build-core-null), so it does not disturb
# desktop_check.sh. (Tests stay ON for the configure: upstream's tests-OFF configure fails on wowee_link_stormlib; only the link check is built.) Needs the image from `tools/vita/desktop_check.sh image`. The Vita side is
# tools/vita/depcheck/sweep.sh and the Vita build (cmake/vita/Vita.cmake).
set -eu

. "$(dirname "$0")/lib.sh"
runtime=$(vita_runtime)
IMAGE=wowee-desktop-builder
null=OFF; dir=build-core
[ "${NULL_AUDIO:-0}" = 1 ] && { null=ON; dir=build-core-null; }

limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-10G} --cpus ${CPUS:-8}"

# imgui and vk-bootstrap are empty submodules until initialised; the configure step needs them.
if [ ! -e "$VITA_ROOT/extern/imgui/imgui.h" ] || [ -z "$(ls -A "$VITA_ROOT/extern/vk-bootstrap" 2>/dev/null)" ]; then
    git -C "$VITA_ROOT" submodule update --init --depth 1 extern/imgui extern/vk-bootstrap
fi

# shellcheck disable=SC2086
"$runtime" run --rm $limits --entrypoint /bin/bash -v "$VITA_ROOT:/workspace" -w /workspace "$IMAGE" -c "
set -e
cmake -S . -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DWOWEE_BUILD_TESTS=ON -DWOWEE_BUILD_CORE=ON -DWOWEE_CORE_NULL_AUDIO=$null >/dev/null
cmake --build $dir --target wowee_core_link_check --parallel \$(nproc)
echo '--- wowee_core_link_check linked (audio null=$null)'
cd $dir
ninja -t deps | awk '/wowee_core_objects.dir/ {obj=\$1} /^    / && obj {print obj, \$1}' > /tmp/core_deps.txt
echo \"objects checked: \$(cut -d: -f1 /tmp/core_deps.txt | sort -u | wc -l)\"
# lua_api_registrations.hpp only forward-declares lua_State, so it is not a Lua dependency.
bad=\$(grep -iE 'vulkan|SDL3|imgui|vk_mem|vk-bootstrap|/extern/lua|lua\\.h|lauxlib' /tmp/core_deps.txt | grep -v lua_api_registrations.hpp || true)
if [ -n \"\$bad\" ]; then echo 'FORBIDDEN headers reached from wowee_core:'; echo \"\$bad\" | head -20; exit 1; fi
echo 'no Vulkan, SDL, ImGui, VMA or Lua header reached from wowee_core'
" </dev/null 2>&1 | grep -v '^\['

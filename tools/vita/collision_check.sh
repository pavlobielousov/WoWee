#!/bin/sh
# Proves that a *_collision.cpp file needs nothing the GPU half owns (VITA-51): compiles it with the
# VitaSDK compiler against a Vulkan-free copy of its header (tools/vita/collision_check/make_shadow.py)
# and with no Vulkan headers on the include path (build-vita/vulkan-headers is NOT used here).
#   tools/vita/collision_check.sh            # every *_collision.cpp listed below
# Syntax only (-fsyntax-only), a few seconds per file. The Vita link is checked by the full build.
set -eu

. "$(dirname "$0")/lib.sh"
runtime=$(vita_runtime)
cd "$VITA_ROOT"
OUT=build-vita/collision_check
rm -rf "$OUT"; mkdir -p "$OUT/rendering"

# <header> <source> [--drop ...]: add a line when another collision file joins.
python3 tools/vita/collision_check/make_shadow.py include/rendering/wmo_renderer.hpp "$OUT/rendering/wmo_renderer.hpp" \
    --drop shadow_params.hpp --drop ShadowParamsSet
FILES="src/rendering/wmo_renderer_collision.cpp"

limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-6G} --cpus ${CPUS:-2}"
# shellcheck disable=SC2086
"$runtime" run --rm $limits -e FILES="$FILES" -e OUT="$OUT" -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c '
cd /workspace
rc=0
for f in $FILES; do
    if arm-vita-eabi-g++ -std=gnu++20 -fsyntax-only -Wall -Wextra -Wno-missing-field-initializers \
        -DGLM_ENABLE_EXPERIMENTAL -DGLM_FORCE_DEPTH_ZERO_TO_ONE \
        -I"$OUT" -Iinclude -Isrc -isystem extern "$f" > "$OUT/$(basename "$f").log" 2>&1; then
        echo "OK    $f"
    else
        echo "FAIL  $f"; head -30 "$OUT/$(basename "$f").log"; rc=1
    fi
done
exit $rc' </dev/null 2>&1 | grep -v '^\[[0-9]/'

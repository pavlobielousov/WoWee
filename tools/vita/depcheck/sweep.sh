#!/bin/sh
# Syntax-only compile of WoWee's real translation units with the VitaSDK compiler (VITA-3).
# Nothing is linked or run; the point is to list what fails to even parse on Vita (missing headers,
# 32-bit and newlib gaps) so VITA-5/9/12 start from facts. Output in build-vita/sweep/:
# results.tsv (dir, file, status, warnings, first error), summary.txt, logs/<file>.log.
# Run from anywhere:
#   tools/vita/depcheck/sweep.sh [dir ...]          # default: every src/ subdirectory
#   JOBS=8 tools/vita/depcheck/sweep.sh network
#   WARN_FLAGS=-Wconversion OUT=build-vita/sweep-conv tools/vita/depcheck/sweep.sh auth network   # VITA-37: extra warnings, own output dir
#   SHADOW=1 OUT=build-vita/sweep-shadow tools/vita/depcheck/sweep.sh core ui   # VITA-52: the Vita's shadow headers first on the path, NO Vulkan headers at all
set -eu
. "$(dirname "$0")/../lib.sh"
VITA_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)   # lib.sh assumes a script directly in tools/vita
runtime=$(vita_runtime)
export OUT="${OUT:-build-vita/sweep}"
mkdir -p "$VITA_ROOT/$OUT"
DIRS="${*:-addons audio auth core game math network pipeline rendering ui}"

# Apple's `container` gives a VM only 1 GB / 4 CPUs by default; parallel g++ on WoWee's headers
# (100k+ lines each) thrashes in that. Ask for more there; docker/podman are not limited this way.
limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-10G} --cpus ${CPUS:-8}"

exec "$runtime" run --rm $limits -v "$VITA_ROOT:/workspace" -e JOBS="${JOBS:-7}" -e DIRS="$DIRS" -e OUT="$OUT" -e WARN_FLAGS="${WARN_FLAGS:-}" -e SHADOW="${SHADOW:-}" \
    "$VITASDK_IMAGE" sh -c '
cd /workspace
rm -rf "$OUT/logs"; mkdir -p "$OUT/logs"
: > "$OUT/files.txt"
for d in $DIRS; do find "src/$d" -name "*.cpp" | sort >> "$OUT/files.txt"; done

# gnu++20 is what CMake passes by default (CXX_EXTENSIONS ON); strict c++20 hides M_PI, setenv, strnlen...
# VK_USE_64_BIT_PTR_DEFINES=1 is an analysis aid only: on 32-bit targets Vulkan makes every
# non-dispatchable handle a plain uint64_t, so the overloads destroy(VkDevice, VkPipeline&)
# / destroy(VkDevice, VkPipelineLayout&) collide (98 files). Pointer handles keep them distinct, so the
# rest of each file is still checked. Vulkan itself is dropped on Vita (VITA-11/12).
# Worker: one translation unit per call (POSIX sh cannot export functions to xargs).
cat > "$OUT/one.sh" <<EOS
#!/bin/sh
f=\$1; log="$OUT/logs/\$(echo "\$f" | tr / _).log"
if arm-vita-eabi-g++ -std=gnu++20 -fsyntax-only -Wall -Wextra -Wno-missing-field-initializers \$WARN_FLAGS \
    -DGLM_ENABLE_EXPERIMENTAL -DGLM_FORCE_DEPTH_ZERO_TO_ONE \
    -DVK_USE_64_BIT_PTR_DEFINES=1 -DWOWEE_HAS_AMD_FSR2=0 -DWOWEE_HAS_AMD_FSR3_FRAMEGEN=0 -DWOWEE_AMD_FFX_SDK_KITS=0 \
    \$SHADOWINC -Iinclude -Isrc -I$OUT/gen -isystem extern -isystem extern/imgui -isystem extern/imgui/backends -isystem extern/lua-5.1.5/src -isystem extern/vk-bootstrap/src \
    \$VKINC "\$f" > "\$log" 2>&1; then st=OK; else st=FAIL; fi
err=\$(grep -m1 "error:" "\$log" | sed "s/^[^ ]* //" | cut -c1-160)
w=\$(grep -c "warning:" "\$log" || true)
printf "%s\t%s\t%s\t%s\t%s\n" "\$(echo "\$f" | cut -d/ -f2)" "\$f" "\$st" "\$w" "\$err"
EOS
chmod +x "$OUT/one.sh"
# stand-ins for what the desktop CMake generates or fetches
mkdir -p "$OUT/gen/core"
sed -e "s/@WOWEE_GIT_VERSION@/sweep/" -e "s/@WOWEE_BUILD_DATE@/today/" include/core/version.hpp.in > "$OUT/gen/core/version.hpp"
# Vulkan headers are not in VitaSDK. If a checkout is at build-vita/vulkan-headers (git clone --depth 1
# https://github.com/KhronosGroup/Vulkan-Headers), use it so Vulkan-including files get past the
# first include and show their OTHER errors; without it they all stop at vulkan/vulkan.h.
VKINC=""; [ -d build-vita/vulkan-headers/include ] && VKINC="-isystem build-vita/vulkan-headers/include"
# SHADOW=1 (VITA-52): the Vita'"'"'s own declarations of the renderer first, and no Vulkan headers to fall back on.
SHADOWINC=""
if [ -n "$SHADOW" ]; then SHADOWINC="-Icmake/vita/shadow"; VKINC=""; fi
export VKINC SHADOWINC
xargs -P "$JOBS" -n 1 "$OUT/one.sh" < "$OUT/files.txt" | sort > "$OUT/results.tsv"

{
  echo "per directory (dir ok fail total warnings):"
  awk -F"\t" "{t[\$1]++; if(\$3==\"OK\")o[\$1]++; else f[\$1]++; w[\$1]+=\$4}
              END{for(d in t) printf \"%s\t%d\t%d\t%d\t%d\n\", d, o[d], f[d], t[d], w[d]}" "$OUT/results.tsv" | sort
  echo; echo "top first-errors (numbers folded to N):"
  awk -F"\t" "\$3==\"FAIL\"{print \$5}" "$OUT/results.tsv" | sed -E "s/[0-9]+/N/g" | sort | uniq -c | sort -rn | head -25
  echo; echo "warning kinds:"
  cat "$OUT"/logs/*.log | grep -oE "\[-W[a-z0-9-]+\]" | sort | uniq -c | sort -rn | head -15
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
'

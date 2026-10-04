#!/bin/sh
# Build vitaGL from source in the VitaSDK container (VITA-53): the SDK's copy shows a boot splash that holds the first
# frame for about a second, and its DXT5 upload is broken (patched, tools/vita/patches). Output: build-vita/vitagl/libvitaGL.a and include/ (pass -DVITAGL_CUSTOM=build-vita/vitagl to
# CMake). Pinned to one commit so the build tag (a hash of the library) is reproducible; override with VITAGL_REF.
#   tools/vita/build_vitagl.sh
set -eu
. "$(dirname "$0")/lib.sh"
runtime=$(vita_runtime)
cd "$VITA_ROOT"
ref="${VITAGL_REF:-1ae86f65718675b797d3cd8ffc021ad2527096cb}"
limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-4G} --cpus ${CPUS:-4}"
# shellcheck disable=SC2086
"$runtime" run --rm $limits -e REF="$ref" -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c '
set -e
cd /workspace
mkdir -p build-vita/vitagl
if [ ! -d build-vita/vitagl-src ]; then git clone https://github.com/Rinnegatamante/vitaGL build-vita/vitagl-src; fi
cd build-vita/vitagl-src
git fetch -q origin && git checkout -q "$REF"
git checkout -q -- . 2>/dev/null || true
# Compressed (DXT1/3/5) uploads: vitaGL copies each level with an asynchronous hardware transfer from a temporary buffer
# that the next upload can reuse, so textures sample as zero depending on upload history (and may hang); the patch makes
# every compressed upload use the CPU swizzle (found and verified in VITA-14, DEV_SETUP 22).
git apply /workspace/tools/vita/patches/vitagl-compressed-cpu-swizzle.patch
make clean >/dev/null 2>&1 || true
make -j4 NO_SPLASHSCREEN=1 >/dev/null
cp libvitaGL.a ../vitagl/
mkdir -p ../vitagl/include && cp source/vitaGL.h ../vitagl/include/ 2>/dev/null || cp vitaGL.h ../vitagl/include/
echo "vitaGL $(git rev-parse HEAD) + vitagl-compressed-cpu-swizzle.patch, built with NO_SPLASHSCREEN=1" | tee ../vitagl/BUILT_FROM.txt' </dev/null

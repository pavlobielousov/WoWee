#!/bin/sh
# Build vitaGL from source in the VitaSDK container (VITA-53): the SDK's copy shows a boot splash that holds the first
# frame for about a second. Output: build-vita/vitagl/libvitaGL.a and include/ (pass -DVITAGL_CUSTOM=build-vita/vitagl to
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
make clean >/dev/null 2>&1 || true
make -j4 NO_SPLASHSCREEN=1 >/dev/null
cp libvitaGL.a ../vitagl/
mkdir -p ../vitagl/include && cp source/vitaGL.h ../vitagl/include/ 2>/dev/null || cp vitaGL.h ../vitagl/include/
echo "vitaGL $(git rev-parse HEAD) built with NO_SPLASHSCREEN=1" | tee ../vitagl/BUILT_FROM.txt' </dev/null

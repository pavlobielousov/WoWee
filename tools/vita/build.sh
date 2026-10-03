#!/bin/sh
# Build a CMake project with the VitaSDK container image (one command, any host).
#   tools/vita/build.sh                       # builds the repo root into build-vita/
#   SRC=tools/vita/devcheck BUILD=build-vita/devcheck tools/vita/build.sh
# macOS uses Apple's `container` CLI, everything else uses docker (or podman).
set -eu

. "$(dirname "$0")/lib.sh"
SRC="${SRC:-.}"
BUILD="${BUILD:-build-vita}"

runtime=$(vita_runtime)

# Apple's `container` gives a VM only 1 GB / 4 CPUs by default; compiling the core's 250 files in
# parallel thrashes there and never finishes (VITA-47). docker/podman are not limited this way.
# Even with 10 GB, eight parallel -O2 -g compiles of the game sources can be killed for memory:
# JOBS=5 tools/vita/build.sh (empty = as many as there are CPUs).
limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-10G} --cpus ${CPUS:-8}"

# shellcheck disable=SC2086
exec "$runtime" run --rm $limits -e JOBS="${JOBS:-}" -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c \
    'src=$1; bld=$2; shift 2
     cmake -S "$src" -B "$bld" -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake "$@" &&
     cmake --build "$bld" --parallel $JOBS' \
    sh "$SRC" "$BUILD" "$@"

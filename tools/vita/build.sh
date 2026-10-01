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

exec "$runtime" run --rm -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c \
    'src=$1; bld=$2; shift 2
     cmake -S "$src" -B "$bld" -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake "$@" &&
     cmake --build "$bld" --parallel' \
    sh "$SRC" "$BUILD" "$@"

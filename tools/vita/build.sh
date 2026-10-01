#!/bin/sh
# Build a CMake project with the VitaSDK container image (one command, any host).
#   tools/vita/build.sh                       # builds the repo root into build-vita/
#   SRC=tools/vita/devcheck BUILD=build-vita/devcheck tools/vita/build.sh
# macOS uses Apple's `container` CLI, everything else uses docker (or podman).
set -eu

# Pin a dated tag for reproducible builds. See docs/vita/DEV_SETUP.md before bumping it.
VITASDK_IMAGE="${VITASDK_IMAGE:-vitasdk/vitasdk:2026.08-20260925}"

root=$(cd "$(dirname "$0")/../.." && pwd)
SRC="${SRC:-.}"
BUILD="${BUILD:-build-vita}"

if [ -n "${CONTAINER_RUNTIME:-}" ]; then
    runtime=$CONTAINER_RUNTIME
elif [ "$(uname -s)" = Darwin ]; then
    runtime=container
elif command -v docker >/dev/null 2>&1; then
    runtime=docker
elif command -v podman >/dev/null 2>&1; then
    runtime=podman
else
    echo "build.sh: no container runtime found (container, docker or podman)" >&2
    exit 1
fi

exec "$runtime" run --rm -v "$root:/workspace" "$VITASDK_IMAGE" sh -c \
    'src=$1; bld=$2; shift 2
     cmake -S "$src" -B "$bld" -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake "$@" &&
     cmake --build "$bld" --parallel' \
    sh "$SRC" "$BUILD" "$@"

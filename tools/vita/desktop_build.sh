#!/bin/sh
# Desktop (Linux) build and test of WoWee in a container, to check that a Vita PR did not break the
# desktop client. Nothing is installed on the host. Uses a copy of upstream's container/builder-linux.Dockerfile
# (tools/vita/desktop-builder.Dockerfile; Ubuntu 24.04, SDL3 built from source, Vulkan, FFmpeg, ...), but not its run script, which is
# Docker-only, copies the tree, clones the FSR SDKs and builds with LTO.
#   tools/vita/desktop_build.sh image       # build the builder image (once, a few minutes)
#   tools/vita/desktop_build.sh configure   # cmake configure into build-desktop/
#   tools/vita/desktop_build.sh build       # configure if needed, then build (incremental)
#   tools/vita/desktop_build.sh test        # ctest (needs a finished build)
#   tools/vita/desktop_build.sh all         # image if missing, configure, build, test
#   tools/vita/desktop_build.sh shell       # interactive shell in the builder
# Env: MEMORY (default 10G, Apple container VMs get 1 GB otherwise), CPUS (default 8),
#      BUILD_TYPE (default Release), CMAKE_ARGS (extra configure arguments).
set -eu

. "$(dirname "$0")/lib.sh"
runtime=$(vita_runtime)
IMAGE=wowee-desktop-builder
BUILD_DIR=build-desktop
cmd="${1:-all}"

limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-10G} --cpus ${CPUS:-8}"

image_exists() {
    case "$runtime" in
        container) container image list 2>/dev/null | grep -q "^$IMAGE " ;;
        *) "$runtime" image inspect "$IMAGE" >/dev/null 2>&1 ;;
    esac
}

build_image() {
    "$runtime" build -f "$VITA_ROOT/tools/vita/desktop-builder.Dockerfile" -t "$IMAGE" "$VITA_ROOT/tools/vita"
}

# imgui and vk-bootstrap are empty submodules until initialised (the other two are not needed).
init_submodules() {
    if [ ! -e "$VITA_ROOT/extern/imgui/imgui.h" ] || [ -z "$(ls -A "$VITA_ROOT/extern/vk-bootstrap" 2>/dev/null)" ]; then
        git -C "$VITA_ROOT" submodule update --init --depth 1 extern/imgui extern/vk-bootstrap
    fi
}

in_container() {
    # shellcheck disable=SC2086
    "$runtime" run --rm $limits --entrypoint /bin/bash -v "$VITA_ROOT:/workspace" -w /workspace \
        -e BUILD_TYPE="${BUILD_TYPE:-Release}" -e CMAKE_ARGS="${CMAKE_ARGS:-}" "$IMAGE" -c "$1"
}

configure() {
    init_submodules
    in_container 'cmake -S . -B '$BUILD_DIR' -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DWOWEE_BUILD_TESTS=ON $CMAKE_ARGS'
}

case "$cmd" in
    image) build_image ;;
    configure) image_exists || build_image; configure ;;
    build)
        image_exists || build_image
        [ -f "$VITA_ROOT/$BUILD_DIR/build.ninja" ] || configure
        in_container "cmake --build $BUILD_DIR --parallel \$(nproc)" ;;
    test)
        in_container "cd $BUILD_DIR && ctest --output-on-failure -j \$(nproc)" ;;
    all)
        image_exists || build_image
        configure
        in_container "cmake --build $BUILD_DIR --parallel \$(nproc) && cd $BUILD_DIR && ctest --output-on-failure -j \$(nproc)" ;;
    shell)
        # shellcheck disable=SC2086
        exec "$runtime" run --rm -it $limits --entrypoint /bin/bash -v "$VITA_ROOT:/workspace" -w /workspace "$IMAGE" ;;
    *) echo "usage: $0 image|configure|build|test|all|shell" >&2; exit 1 ;;
esac

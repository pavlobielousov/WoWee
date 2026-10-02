#!/bin/sh
# 32-bit check of WoWee's unit tests (VITA-37, VITA-38): cross-build the GPU-free tests for armhf (ARMv7, hard float, ILP32:
# the Vita's ABI) and run them under qemu-user, all in a container. Nothing is installed on the host. It finds 32-bit
# bugs the 64-bit desktop check cannot (a bounds check `size_t(x) + n > size` wraps here), see docs/vita/DEV_SETUP.md section 10.
# Run it from any directory:
#   tools/vita/check32.sh image       # build the wowee-armhf image (needs wowee-desktop-builder; built first if missing)
#   tools/vita/check32.sh configure   # cross-configure into build-armhf/
#   tools/vita/check32.sh build       # configure if needed, then build every test executable (incremental)
#   tools/vita/check32.sh test        # ctest under qemu-arm (needs a finished build)
#   tools/vita/check32.sh all         # image if missing, configure, build, test
#   tools/vita/check32.sh shell       # interactive shell in the container
# Env: MEMORY (default 10G, Apple container VMs get 1 GB otherwise), CPUS (default 8), BUILD_TYPE (default Release),
#      CTEST_ARGS (extra ctest arguments, e.g. "-R warden").
# Excluded on purpose (see DEV_SETUP.md section 10): extract_progress (needs an armhf StormLib; an asset-extraction test),
# sweep_guard (host Python sweeps, not 32-bit relevant, exceeds a 120 s timeout under emulation).
set -eu

. "$(dirname "$0")/lib.sh"
runtime=$(vita_runtime)
IMAGE=wowee-armhf
BASE_IMAGE=wowee-desktop-builder
BUILD_DIR=build-armhf
EXCLUDE_TESTS='extract_progress|sweep_guard'
cmd="${1:-all}"

limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-10G} --cpus ${CPUS:-8}"

image_exists() {
    case "$runtime" in
        container) container image list 2>/dev/null | grep -q "^$1 " ;;
        *) "$runtime" image inspect "$1" >/dev/null 2>&1 ;;
    esac
}

build_image() {
    image_exists "$BASE_IMAGE" || "$VITA_ROOT/tools/vita/desktop_check.sh" image
    "$runtime" build -f "$VITA_ROOT/tools/vita/check32/Dockerfile" -t "$IMAGE" "$VITA_ROOT/tools/vita/check32"
}

# imgui and vk-bootstrap are empty submodules until initialised (same as desktop_check.sh).
init_submodules() {
    if [ ! -e "$VITA_ROOT/extern/imgui/imgui.h" ] || [ -z "$(ls -A "$VITA_ROOT/extern/vk-bootstrap" 2>/dev/null)" ]; then
        git -C "$VITA_ROOT" submodule update --init --depth 1 extern/imgui extern/vk-bootstrap
    fi
}

in_container() {
    # shellcheck disable=SC2086
    "$runtime" run --rm $limits --entrypoint /bin/bash -v "$VITA_ROOT:/workspace" -w /workspace \
        -e BUILD_TYPE="${BUILD_TYPE:-Release}" -e CTEST_ARGS="${CTEST_ARGS:-}" "$IMAGE" -c "$1"
}

# The explicit armhf library paths matter: CMake otherwise picks the arm64 libssl/libz/libvulkan from
# /usr/lib/aarch64-linux-gnu and the link fails with "file format not recognized". libvulkan is an empty stub
# (see the Dockerfile): no Vulkan code is compiled or run.
configure() {
    init_submodules
    in_container 'L=/usr/lib/arm-linux-gnueabihf
cmake -S . -B '$BUILD_DIR' -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DWOWEE_BUILD_TESTS=ON -DCMAKE_TOOLCHAIN_FILE=/opt/armhf.cmake \
  -DOPENSSL_SSL_LIBRARY=$L/libssl.so -DOPENSSL_CRYPTO_LIBRARY=$L/libcrypto.so -DOPENSSL_INCLUDE_DIR=/usr/include \
  -DZLIB_LIBRARY=$L/libz.so -DZLIB_INCLUDE_DIR=/usr/include \
  -DVulkan_LIBRARY=/opt/stub/libvulkan.so -DVulkan_INCLUDE_DIR=/usr/include'
}

# Every executable ctest runs (test_* and helpers such as framexml_compile_check), minus the excluded tests, found from
# ctest's own command list so a new test needs no edit here. ctest wraps each command in the qemu emulator, so the
# executable is the argument under the build directory.
BUILD_CMD='targets=$(ctest --test-dir '$BUILD_DIR' --show-only=json-v1 | python3 -c "
import json, os, re, sys
skip = re.compile(r\"^(%s)\$\" % \"'$EXCLUDE_TESTS'\")
for t in json.load(sys.stdin)[\"tests\"]:
    if skip.match(t[\"name\"]): continue
    exe = [a for a in t.get(\"command\", []) if \"/'$BUILD_DIR'/bin/\" in a]
    if exe: print(os.path.basename(exe[0]))
" | sort -u | tr "\n" " ")
echo "building $(echo $targets | wc -w) test executables"
ninja -C '$BUILD_DIR' -k 0 $targets'

TEST_CMD='cd '$BUILD_DIR' && ctest --output-on-failure -j $(nproc) --timeout 120 -E "'$EXCLUDE_TESTS'" $CTEST_ARGS'

case "$cmd" in
    image) build_image ;;
    configure) image_exists "$IMAGE" || build_image; configure ;;
    build)
        image_exists "$IMAGE" || build_image
        [ -f "$VITA_ROOT/$BUILD_DIR/build.ninja" ] || configure
        in_container "$BUILD_CMD" ;;
    test) in_container "$TEST_CMD" ;;
    all)
        image_exists "$IMAGE" || build_image
        configure
        in_container "$BUILD_CMD && $TEST_CMD" ;;
    shell)
        # shellcheck disable=SC2086
        exec "$runtime" run --rm -it $limits --entrypoint /bin/bash -v "$VITA_ROOT:/workspace" -w /workspace "$IMAGE" ;;
    *) echo "usage: $0 image|configure|build|test|all|shell" >&2; exit 1 ;;
esac

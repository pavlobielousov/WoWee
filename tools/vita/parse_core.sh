#!/bin/sh
# Symbolize a Vita crash dump.
#   tools/vita/parse_core.sh <file.psp2dmp> <unstripped.elf>
#   e.g. tools/vita/parse_core.sh build-vita/logs/psp2core-*.psp2dmp build-vita/devcheck/devcheck
# The ELF must be the one produced by the SAME build as the eboot.bin that crashed (not eboot.bin,
# not .velf). Runs vita-parse-core inside the VitaSDK image, because it needs arm-vita-eabi-objdump
# and arm-vita-eabi-addr2line from the toolchain.
set -eu

VITASDK_IMAGE="${VITASDK_IMAGE:-vitasdk/vitasdk:2026.08-20260925}"
# xyzz/vita-parse-core master, pinned
PARSE_CORE_REV=644b5f081c5f3c9b205180793ab8f4209dfd9d97

core="${1:?usage: parse_core.sh <core.psp2dmp> <elf>}"
elf="${2:?usage: parse_core.sh <core.psp2dmp> <elf>}"
root=$(cd "$(dirname "$0")/../.." && pwd)

# Paths must live under the repo root so the container can see them.
rel() { python3 -c 'import os,sys;print(os.path.relpath(os.path.realpath(sys.argv[1]),sys.argv[2]))' "$1" "$root"; }
core_rel=$(rel "$core"); elf_rel=$(rel "$elf")

if [ -n "${CONTAINER_RUNTIME:-}" ]; then runtime=$CONTAINER_RUNTIME
elif [ "$(uname -s)" = Darwin ]; then runtime=container
elif command -v docker >/dev/null 2>&1; then runtime=docker
else runtime=podman; fi

exec "$runtime" run --rm -v "$root:/workspace" "$VITASDK_IMAGE" sh -c '
    set -e
    d=$(mktemp -d)
    git clone -q https://github.com/xyzz/vita-parse-core "$d"
    git -C "$d" checkout -q '"$PARSE_CORE_REV"'
    # The image has no pip, and the tool predates pyelftools 0.30 (needs elftools.common.py3compat).
    apt-get update -qq && apt-get install -y -qq python3-pip >/dev/null
    pip install -q --break-system-packages pyelftools==0.29 2>/dev/null
    python3 "$d/main.py" "$1" "$2"' sh "$core_rel" "$elf_rel"

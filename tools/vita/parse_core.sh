#!/bin/sh
# Symbolize a Vita crash dump.
#   tools/vita/parse_core.sh <file.psp2dmp> <unstripped.elf>
#   e.g. tools/vita/parse_core.sh build-vita/logs/psp2core-*.psp2dmp build-vita/devcheck/devcheck
# The ELF must be the one produced by the SAME build as the eboot.bin that crashed (not eboot.bin,
# not .velf). Runs vita-parse-core inside the VitaSDK image, because it needs arm-vita-eabi-objdump
# and arm-vita-eabi-addr2line from the toolchain.
set -eu

. "$(dirname "$0")/lib.sh"
# xyzz/vita-parse-core master, pinned
PARSE_CORE_REV=644b5f081c5f3c9b205180793ab8f4209dfd9d97

core="${1:?usage: parse_core.sh <core.psp2dmp> <elf>}"
elf="${2:?usage: parse_core.sh <core.psp2dmp> <elf>}"
core_rel=$(vita_rel "$core"); elf_rel=$(vita_rel "$elf")
runtime=$(vita_runtime)

exec "$runtime" run --rm -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c '
    set -e
    d=$(mktemp -d)
    git clone -q https://github.com/xyzz/vita-parse-core "$d"
    git -C "$d" checkout -q '"$PARSE_CORE_REV"'
    # The image has no pip, and the tool predates pyelftools 0.30 (needs elftools.common.py3compat).
    apt-get update -qq && apt-get install -y -qq python3-pip >/dev/null
    pip install -q --break-system-packages pyelftools==0.29 2>/dev/null
    python3 "$d/main.py" "$1" "$2"' sh "$core_rel" "$elf_rel"

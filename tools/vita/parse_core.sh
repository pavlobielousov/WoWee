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

# Apple's `container` gives a VM 1 GB by default, and objdump of a big debug ELF (the client's is 380 MB) is killed by it.
limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-8G} --cpus ${CPUS:-4}"
# shellcheck disable=SC2086
exec "$runtime" run --rm $limits -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c '
    set -e
    d=$(mktemp -d)
    git clone -q https://github.com/xyzz/vita-parse-core "$d"
    git -C "$d" checkout -q '"$PARSE_CORE_REV"'
    # The image has no pip, and the tool predates pyelftools 0.30 (needs elftools.common.py3compat).
    apt-get update -qq && apt-get install -y -qq python3-pip >/dev/null
    pip install -q --break-system-packages pyelftools==0.29 2>/dev/null
    # The tool is Python 2 era: util.c_str appends bytes to a str (a TypeError on the first real
    # dump, VITA-7 on hardware). Patch it to indexing that works on both.
    sed -i "s/out += buf\[off\]/out += chr(buf[off]) if isinstance(buf[off], int) else buf[off]/" "$d/util.py"
    python3 "$d/main.py" "$1" "$2"' sh "$core_rel" "$elf_rel"

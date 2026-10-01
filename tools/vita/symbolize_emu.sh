#!/bin/sh
# Symbolize a crash from a Vita3K log (no core dump exists in the emulator; it prints PC/LR itself).
#   tools/vita/symbolize_emu.sh build-vita/logs/vita3k.log build-vita/devcheck/devcheck
# Uses the unstripped ELF of the same build. The emulator loads the app at the ELF's own link
# address (0x81000000), so PCs map directly. Prints nothing and exits 1 if the log has no crash.
set -eu
. "$(dirname "$0")/lib.sh"
log="${1:?usage: symbolize_emu.sh <vita3k.log> <elf>}"
elf="${2:?usage: symbolize_emu.sh <vita3k.log> <elf>}"

addrs=$(grep -m1 '^PC: ' "$log" | sed -n 's/^PC: \(0x[0-9a-f]*\),.*LR: \(0x[0-9a-f]*\).*/\1 \2/p')
[ -n "$addrs" ] || { echo "no crash (PC line) in $log" >&2; exit 1; }
grep -m1 -E '\|E\| \[Memory(Read|Write)\]: Invalid' "$log" | cut -c14- || true
echo "PC / LR: $addrs"
# LR has the Thumb bit set; subtract 1 so it points into the calling instruction.
set -- $addrs
lr=$(printf '0x%x' $(( $2 - 1 )))
exec "$(vita_runtime)" run --rm -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" \
    arm-vita-eabi-addr2line -f -C -p -e "$(vita_rel "$elf")" "$1" "$lr"

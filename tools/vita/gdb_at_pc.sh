#!/bin/sh
# Stop a Vita3K run at a given address and print the backtrace, locals, registers and stack.
#   tools/vita/gdb_at_pc.sh <TITLEID> <unstripped.elf> [--pc 0x810004d6] [--skip N]
#                           [--cmd crash] [--wait S] [--log build-vita/logs/vita3k.log]
# Two-pass crash workflow (macOS, Vita3K): Vita3K's gdbstub does not report a crash to gdb, but its log
# prints the crash PC. Pass 1: tools/vita/vita3k_macos.sh ... (crashes, log has "PC: 0x..."). Pass 2: this
# script re-runs the app under the stub with a breakpoint at that PC, so gdb stops just before the
# faulting instruction. Needs a repeatable crash, and the app already installed (pass 1 installs it).
#   --pc      address to break at (default: the first "PC:" line of --log)
#   --skip N  ignore the first N hits (when the crashing line also runs earlier without crashing)
#   --cmd C   write C into ux0:data/wowee/devcheck.cmd once gdb is connected (devcheck: "crash")
#   --wait S  seconds to wait for the breakpoint (default 40)
# Output: build-vita/logs/gdb_at_pc.txt. Limits (checked on this Vita3K build): no Ctrl-C, no single-step,
# no detach, so the session can only break and inspect; gdb connection timeouts are retried, and a second run
# started soon after a first waits (can be minutes) for the first session's lingering socket on port 2159.
set -eu

[ "$(uname -s)" = Darwin ] || { echo "gdb_at_pc.sh runs on macOS only (found $(uname -s))" >&2; exit 1; }
. "$(dirname "$0")/lib.sh"

titleid="${1:?usage: gdb_at_pc.sh <TITLEID> <elf> [--pc ADDR] [--skip N] [--cmd C] [--wait S] [--log FILE]}"
elf="${2:?usage: gdb_at_pc.sh <TITLEID> <elf> ...}"
shift 2
pc=""; skip=0; cmd=""; wait_s=40; log="build-vita/logs/vita3k.log"
while [ $# -gt 0 ]; do
    case "$1" in
        --pc) pc=$2; shift 2 ;;
        --skip) skip=$2; shift 2 ;;
        --cmd) cmd=$2; shift 2 ;;
        --wait) wait_s=$2; shift 2 ;;
        --log) log=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

if [ -z "$pc" ]; then
    [ -f "$log" ] || { echo "no --pc given and $log does not exist (run vita3k_macos.sh first)" >&2; exit 1; }
    pc=$(sed -n 's/^PC: \(0x[0-9a-f]*\),.*/\1/p' "$log" | head -1)
    [ -n "$pc" ] || { echo "no 'PC:' line in $log: nothing crashed in pass 1" >&2; exit 1; }
    echo "crash PC from $log: $pc"
fi

bin="${VITA3K_BIN:-/Applications/Vita3K.app/Contents/MacOS/Vita3K}"
[ -x "$bin" ] || { echo "Vita3K not found at $bin (set VITA3K_BIN)" >&2; exit 1; }
data="$HOME/Library/Application Support/Vita3K/Vita3K"
pref="${VITA3K_PREF:-}"
if [ -z "$pref" ] && [ -f "$data/config.yml" ]; then
    pref=$(sed -n 's/^pref-path: *//p' "$data/config.yml" | head -1)
fi
pref="${pref:-$data/fs}"
appdata="$pref/ux0/data/wowee"
[ -d "$pref/ux0/app/$titleid" ] || { echo "$titleid is not installed in Vita3K (run vita3k_macos.sh first)" >&2; exit 1; }

hostip=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)
[ -n "$hostip" ] || { echo "cannot determine this Mac's LAN address (en0/en1); the container reaches the stub through it" >&2; exit 1; }

out=build-vita/logs; mkdir -p "$out"
# Throwaway config with the stub on, so the user's own Vita3K config is never modified.
cfgdir=$(mktemp -d)
sed 's/^gdbstub: .*/gdbstub: true/' "$data/config.yml" > "$cfgdir/config.yml"
grep -q '^gdbstub: true' "$cfgdir/config.yml" || echo 'gdbstub: true' >> "$cfgdir/config.yml"

emu_pid=""; sender_pid=""
cleanup() {
    [ -n "$sender_pid" ] && kill "$sender_pid" 2>/dev/null || true
    [ -n "$emu_pid" ] && kill -9 "$emu_pid" 2>/dev/null || true
    rm -rf "$cfgdir"
}
trap cleanup EXIT INT TERM

# The stub cannot detach, so a finished session leaves its TCP connection in CLOSE_WAIT/LAST_ACK after the
# emulator is killed, and that blocks the next bind on port 2159 (the stub then silently never listens).
# Wait for the old socket to clear; it takes a while (up to a few minutes).
waited=0
while netstat -an -p tcp | grep -q -E '\.2159 '; do
    [ "$waited" -eq 0 ] && echo "waiting for port 2159 to clear (socket left by the previous gdb session)..."
    [ "$waited" -ge 300 ] && { echo "port 2159 still busy after 300s (netstat -an -p tcp | grep 2159)" >&2; exit 1; }
    sleep 3; waited=$((waited + 3))
done

mkdir -p "$appdata"; rm -f "$appdata/devcheck.cmd"
"$bin" -c "$cfgdir/config.yml" -w -r "$titleid" >"$out/vita3k-gdb.log" 2>&1 &
emu_pid=$!
for _ in $(seq 1 30); do lsof -nP -iTCP:2159 -sTCP:LISTEN >/dev/null 2>&1 && break; sleep 1; done
lsof -nP -iTCP:2159 -sTCP:LISTEN >/dev/null 2>&1 || { echo "gdbstub is not listening on 2159, see $out/vita3k-gdb.log" >&2; exit 1; }

# Trigger the crash only once gdb is attached (and has had time to set the breakpoint), so a retried
# connection cannot miss it.
if [ -n "$cmd" ]; then
    ( while ! grep -q 'GDB Server Received Connection' "$out/vita3k-gdb.log"; do sleep 0.5; done
      sleep 4; printf '%s\n' "$cmd" > "$appdata/devcheck.cmd" ) &
    sender_pid=$!
fi

runtime=$(vita_runtime)
elf_rel=$(vita_rel "$elf")
rc=3; try=1
while [ "$rc" -eq 3 ] && [ "$try" -le 4 ]; do
    set +e
    "$runtime" run --rm -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" \
        python3 tools/vita/gdb_drive.py "$elf_rel" "$hostip:2159" "$pc" "$skip" "$wait_s" \
        >"$out/gdb_at_pc.txt" 2>"$out/gdb_at_pc.err"
    rc=$?
    set -e
    [ "$rc" -eq 3 ] && { echo "gdb could not connect (attempt $try), retrying"; try=$((try + 1)); }
done

sed -n '/hit Breakpoint/,$p' "$out/gdb_at_pc.txt" | grep -v -E '^\(gdb\) quit|Remote doesn.t know|^Detaching' || tail -5 "$out/gdb_at_pc.txt"
echo "--- full session: $out/gdb_at_pc.txt (exit $rc: 0 ok, 3 no connection, 4 breakpoint never hit)"
exit "$rc"

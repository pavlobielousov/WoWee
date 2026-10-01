#!/bin/sh
# Install, run and observe a VPK in Vita3K from the command line: no clicking, logs come back as files.
#   tools/vita/vita3k.sh <app.vpk> <TITLEID> [--seconds N] [--cmd crash|quit] [--cmd-after S] [--elf <elf>]
# Writes to build-vita/logs/: <TITLEID>.app.log (the app's own ux0:data/wowee/*.log) and
# vita3k.log (the emulator log, which shows aborts and crashes). Exit status 0 if the app log has content.
# Env: VITA3K_BIN (emulator binary), VITA3K_PREF (its pref-path; default read from config.yml).
# Smoke tests only: emulator speed and memory say nothing about a real Vita.
set -eu

vpk="${1:?usage: vita3k.sh <app.vpk> <TITLEID> [--seconds N] [--cmd crash|quit] [--cmd-after S]}"
titleid="${2:?usage: vita3k.sh <app.vpk> <TITLEID> ...}"
shift 2
seconds=15; cmd=""; cmd_after=8; elf=""
while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) seconds=$2; shift 2 ;;
        --cmd) cmd=$2; shift 2 ;;
        --cmd-after) cmd_after=$2; shift 2 ;;
        --elf) elf=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

case "$(uname -s)" in
    Darwin) bin_default=/Applications/Vita3K.app/Contents/MacOS/Vita3K
            data_default="$HOME/Library/Application Support/Vita3K/Vita3K" ;;
    *)      bin_default=$(command -v Vita3K || echo Vita3K)
            data_default="${XDG_DATA_HOME:-$HOME/.local/share}/Vita3K/Vita3K" ;;
esac
bin="${VITA3K_BIN:-$bin_default}"
[ -x "$bin" ] || { echo "Vita3K not found at $bin (set VITA3K_BIN)" >&2; exit 1; }

pref="${VITA3K_PREF:-}"
if [ -z "$pref" ] && [ -f "$data_default/config.yml" ]; then
    pref=$(sed -n 's/^pref-path: *//p' "$data_default/config.yml" | head -1)
fi
pref="${pref:-$data_default/fs}"
appdata="$pref/ux0/data/wowee"

out="build-vita/logs"; mkdir -p "$out"
rm -f "$appdata"/*.log "$appdata/devcheck.cmd"
mkdir -p "$appdata"

# Step 1: install. Given a vpk path, Vita3K installs it within a second but its auto-boot does not
# start the app reliably, so stop that instance and launch with -r (step 2).
"$bin" "$vpk" >"$out/vita3k-install.log" 2>&1 &
pid=$!
sleep 4
# Vita3K ignores SIGTERM while running an app, so escalate. Only the instance started here is killed.
stop_emu() {
    kill $pid 2>/dev/null || true
    sleep 2
    kill -9 $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
}
trap stop_emu EXIT INT TERM
grep -q 'installed successfully' "$out/vita3k-install.log" || { echo "install failed, see $out/vita3k-install.log" >&2; exit 1; }
stop_emu
rm -f "$appdata"/*.log "$appdata/devcheck.cmd"
"$bin" -r "$titleid" >"$out/vita3k.log" 2>&1 &
pid=$!

if [ -n "$cmd" ]; then
    sleep "$cmd_after"
    printf '%s\n' "$cmd" > "$appdata/devcheck.cmd"
    sleep $((seconds > cmd_after ? seconds - cmd_after : 3))
else
    sleep "$seconds"
fi
stop_emu
trap - EXIT INT TERM

: > "$out/$titleid.app.log"
for f in "$appdata"/*.log; do [ -f "$f" ] && cat "$f" >> "$out/$titleid.app.log"; done
echo "--- app log ($out/$titleid.app.log)"; cat "$out/$titleid.app.log"
echo "--- emulator log: $out/vita3k.log ($(wc -l < "$out/vita3k.log") lines)"
if [ -n "$elf" ] && grep -q '^PC: ' "$out/vita3k.log"; then
    echo "--- crash"; "$(dirname "$0")/symbolize_emu.sh" "$out/vita3k.log" "$elf" || true
fi
[ -s "$out/$titleid.app.log" ]

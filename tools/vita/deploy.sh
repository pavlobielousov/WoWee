#!/bin/sh
# Push eboot.bin (or any file) to a Vita running VitaShell's FTP server, without reinstalling the VPK.
#   VITA_IP=192.168.1.50 tools/vita/deploy.sh build-vita/devcheck/eboot.bin DEVC00001
#   VITA_IP=... tools/vita/deploy.sh --logs          # fetch ux0:data/wowee/* into build-vita/logs/
# The app must be installed once from a VPK first (creates ux0:app/<TITLEID>).
# Optional (vitacompanion plugin, command port 1338): VITA_RESTART=1 kills and relaunches the app.
set -eu

: "${VITA_IP:?set VITA_IP to the Vita address (VitaShell shows it after pressing SELECT)}"
FTP_PORT="${VITA_FTP_PORT:-1337}"
CMD_PORT="${VITA_CMD_PORT:-1338}"

if [ "${1:-}" = "--logs" ]; then
    mkdir -p build-vita/logs
    for f in $(curl -s --list-only "ftp://$VITA_IP:$FTP_PORT/ux0:/data/wowee/"); do
        curl -s -o "build-vita/logs/$f" "ftp://$VITA_IP:$FTP_PORT/ux0:/data/wowee/$f"
        echo "fetched build-vita/logs/$f"
    done
    exit 0
fi

file="${1:?usage: deploy.sh <eboot.bin> <TITLEID>}"
titleid="${2:?usage: deploy.sh <eboot.bin> <TITLEID>}"

curl --fail -T "$file" "ftp://$VITA_IP:$FTP_PORT/ux0:/app/$titleid/$(basename "$file")"
echo "uploaded $file to ux0:app/$titleid/"

if [ -n "${VITA_RESTART:-}" ]; then
    printf 'destroy\n' | nc -w 2 "$VITA_IP" "$CMD_PORT" || true
    printf 'launch %s\n' "$titleid" | nc -w 2 "$VITA_IP" "$CMD_PORT"
fi

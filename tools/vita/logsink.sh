#!/bin/sh
# Print UDP log lines sent by the Vita.
# WoWee: put WOWEE_LOG_UDP=<this machine's LAN ip>[:port] in ux0:data/wowee/env.txt.
# devcheck: put "<ip>[:port]" in ux0:data/wowee/loghost.txt. Then run this.
# BSD nc (macOS) and OpenBSD nc take `-u -l PORT`; traditional netcat needs `-p`.
PORT="${1:-9999}"
echo "listening for Vita logs on UDP $PORT (Ctrl-C to stop)"
if nc -h 2>&1 | grep -q -- '-p '; then
    exec nc -u -l "$PORT"
fi
exec nc -u -l -p "$PORT"

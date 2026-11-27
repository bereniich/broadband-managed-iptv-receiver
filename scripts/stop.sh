#!/usr/bin/env bash
# Stops the system started by scripts/start.sh (NFR-12, HLR-23): P1 Chromium (the kiosk profile of
# start.sh), P2 DTV middleware, P3 head-end, P4 OTT MW server, in this order. P2 gets SIGTERM and up to
# 3 s for its orderly stop, in which it sends POST /stop for its head-end session (REQ-70, Section 4.2.5);
# a process that has not ended after its time gets SIGKILL. Exit code 1 if a process is left.
#
# Usage: scripts/stop.sh
set -u

CHROMIUM_PROFILE="$HOME/snap/chromium/common/m15-kiosk"   # as in scripts/start.sh

# stop_matching <component> <seconds> <pgrep arguments...>
stop_matching() {
    local name=$1 seconds=$2
    shift 2
    pgrep "$@" > /dev/null || { echo "stop.sh: $name not running"; return 0; }
    pkill -TERM "$@"
    for _ in $(seq 1 $(( seconds * 10 ))); do
        pgrep "$@" > /dev/null || { echo "stop.sh: $name stopped"; return 0; }
        sleep 0.1
    done
    pkill -KILL "$@"
    sleep 0.2
    echo "stop.sh: $name did not stop within $seconds s, killed"
}

# Process names are matched exactly (-x), so that the shell running this script never matches.
stop_matching "Chromium" 5 -f -- "--user-data-dir=$CHROMIUM_PROFILE"
stop_matching "DTV middleware (dtv-middleware)" 3 -x dtv-middleware
stop_matching "head-end (headend)" 3 -x headend
stop_matching "OTT MW server (mw_server)" 3 -x mw_server

LEFT=$( { pgrep -f -- "--user-data-dir=$CHROMIUM_PROFILE"; pgrep -x 'dtv-middleware|headend|mw_server'; } | tr '\n' ' ')
if [ -n "$LEFT" ]; then
    echo "stop.sh: FAILED: processes left: $LEFT" >&2
    exit 1
fi
echo "stop.sh: all components stopped"

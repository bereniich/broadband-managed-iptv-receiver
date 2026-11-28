#!/usr/bin/env bash
# Starts the complete system (NFR-12, HLR-23, Section 4.1.1): P4 OTT MW server, P3 head-end,
# P2 DTV middleware and P1 Chromium in kiosk mode, in this order, from the repository root.
# After each process the script checks that this process has opened its TCP port, and before
# Chromium that GET / of the Web application answers 200 (ER-40). On a failure it names the
# component, stops the components it has already started and exits with code 1 (ER-37).
#
# Usage: scripts/start.sh [--diag] [--test-set]
#   --diag      page with ?diag=1, standard error of Chromium in logs/chromium.log (Section 5.1.8)
#   --test-set  P3 and P4 with server/config.test.json (test set of Section 5.2.1)
#
# Ports come from the configuration files (NFR-16, TST-718). The UDP port of the tuner is opened
# only when a service is tuned (ITuner::tune), so it is not checked here.
set -u

cd "$(dirname "$0")/.." || exit 1

LIMIT_S=15                       # NFR-12: everything within 15 s
SERVER_CONFIG=server/config.json
MW_CONFIG=middleware/main/middleware.json
CHROMIUM_PROFILE="$HOME/snap/chromium/common/m15-kiosk"   # own profile: never a tab of another Chromium
DIAG=0

for arg in "$@"; do
    case "$arg" in
        --diag) DIAG=1 ;;
        --test-set) SERVER_CONFIG=server/config.test.json ;;
        *) echo "start.sh: unknown option $arg (usage: scripts/start.sh [--diag] [--test-set])" >&2; exit 1 ;;
    esac
done

STARTED=()                       # PIDs started by this run, in start order
T0=$(date +%s%N)

elapsed_ms() { echo $(( ($(date +%s%N) - T0) / 1000000 )); }

# Value of an integer key of a JSON configuration file, or the default if it cannot be read
# (an invalid file is reported by the process itself, ER-37).
config_port() {
    local file=$1 key=$2 default=$3 value
    value=$(grep -oE "\"$key\"[[:space:]]*:[[:space:]]*[0-9]+" "$file" 2>/dev/null | grep -oE '[0-9]+$' | head -n 1)
    echo "${value:-$default}"
}

# Stops the processes of this run in reverse order (used on a failure).
stop_started() {
    local i pid
    for (( i=${#STARTED[@]}-1; i>=0; i-- )); do
        pid=${STARTED[i]}
        kill -TERM "$pid" 2>/dev/null
        for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$pid" 2>/dev/null
    done
}

fail() {
    echo "start.sh: FAILED: $1" >&2
    [ -n "${2:-}" ] && [ -s "$2" ] && { echo "--- last lines of $2:" >&2; tail -n 5 "$2" >&2; }
    stop_started
    [ ${#STARTED[@]} -gt 0 ] && echo "start.sh: stopped the components started by this run" >&2
    exit 1
}

port_owner() {   # PID of the process listening on 127.0.0.1:<port> (TCP), empty if none
    ss -ltnpH "sport = :$1" 2>/dev/null | grep -oE 'pid=[0-9]+' | head -n 1 | cut -d= -f2
}

# start_process <component> <port> <log> <command...>
start_process() {
    local name=$1 port=$2 out=$3
    shift 3
    [ -x "$1" ] || fail "$name: $1 not found (build first: cmake -S middleware -B build && cmake --build build -j)"
    [ -z "$(port_owner "$port")" ] || fail "$name: TCP port $port is already in use (run scripts/stop.sh)"
    "$@" > "$out" 2>&1 &
    local pid=$!
    STARTED+=("$pid")
    while [ "$(elapsed_ms)" -lt $(( LIMIT_S * 1000 )) ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid"
            fail "$name exited with code $? before opening TCP port $port" "$out"
        fi
        if [ "$(port_owner "$port")" = "$pid" ]; then
            echo "start.sh: $name started (pid $pid, TCP port $port, $(elapsed_ms) ms)"
            return 0
        fi
        sleep 0.1
    done
    fail "$name did not open TCP port $port within $LIMIT_S s" "$out"
}

mkdir -p logs || fail "cannot create logs/"

MW_PORT=$(config_port "$SERVER_CONFIG" mw_port 8080)
HE_PORT=$(config_port "$SERVER_CONFIG" headend_port 8081)
UI_PORT=$(config_port "$MW_CONFIG" ui_http_port 8090)

# Standard output and error of the processes go to logs/<component>.out; their log files
# logs/<component>.log are written by the processes themselves (NFR-13).
start_process "OTT MW server (mw_server, $SERVER_CONFIG)" "$MW_PORT" logs/mw_server.out \
    build/bin/mw_server --config "$SERVER_CONFIG"
start_process "head-end (headend, $SERVER_CONFIG)" "$HE_PORT" logs/headend.out \
    build/bin/headend --config "$SERVER_CONFIG"
start_process "DTV middleware (dtv-middleware, $MW_CONFIG)" "$UI_PORT" logs/middleware.out \
    build/bin/dtv-middleware --config "$MW_CONFIG"

URL="http://127.0.0.1:$UI_PORT/"
STATUS=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$URL")
[ "$STATUS" = "200" ] || fail "Web application files: GET $URL answered $STATUS (check webui_dir in $MW_CONFIG)" \
    logs/middleware.out

command -v chromium > /dev/null || fail "Chromium: command chromium not found (sudo snap install chromium)"
CHROMIUM_ARGS=(--user-data-dir="$CHROMIUM_PROFILE" --kiosk --no-first-run --autoplay-policy=no-user-gesture-required)
if [ "$DIAG" = 1 ]; then
    chromium "${CHROMIUM_ARGS[@]}" --enable-logging=stderr "${URL}?diag=1" > /dev/null 2> logs/chromium.log &
else
    chromium "${CHROMIUM_ARGS[@]}" "$URL" > /dev/null 2>&1 &
fi
CHROMIUM_PID=$!
sleep 1
kill -0 "$CHROMIUM_PID" 2>/dev/null || pgrep -f -- "--user-data-dir=$CHROMIUM_PROFILE" > /dev/null \
    || fail "Chromium exited at start-up"
echo "start.sh: Chromium started (kiosk, $URL$([ "$DIAG" = 1 ] && echo '?diag=1, logs/chromium.log'))"
echo "start.sh: system started in $(elapsed_ms) ms; stop it with scripts/stop.sh"

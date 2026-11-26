#!/bin/sh
# TST-158 (process level): dtv-middleware exits with code 1 and a message naming the file for a missing
# or invalid configuration file (IN-11, ER-37), and with its usage for missing arguments.
# Usage: main_exit_code_test.sh <path of dtv-middleware>
binary="$1"
dir=$(mktemp -d) || exit 2
trap 'rm -rf "$dir"' EXIT
failures=0

check() {   # check <description> <expected text in stderr> <arguments...>
    description="$1"; expected="$2"; shift 2
    "$binary" "$@" > "$dir/out" 2> "$dir/err"
    status=$?
    if [ "$status" -ne 1 ]; then
        echo "FAIL $description: exit code $status, expected 1"; failures=$((failures + 1))
    elif ! grep -qF -- "$expected" "$dir/err"; then
        echo "FAIL $description: stderr does not contain '$expected':"; cat "$dir/err"; failures=$((failures + 1))
    else
        echo "ok   $description"
    fi
}

printf '{"rtp_port": 5004,' > "$dir/broken.json"
printf '{"rtp_port": "abc"}' > "$dir/bad.json"
check "missing file" "$dir/missing.json" --config "$dir/missing.json"
check "invalid JSON" "$dir/broken.json" --config "$dir/broken.json"
check "invalid value" "rtp_port" --config "$dir/bad.json"
check "no arguments" "usage: dtv-middleware --config <file>"
check "unknown argument" "usage: dtv-middleware --config <file>" --conf x.json
exit "$failures"

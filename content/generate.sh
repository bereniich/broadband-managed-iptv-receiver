#!/usr/bin/env bash
# SRV-GEN — content generator (SPECIFICATION.md, Sections 4.7.2 and 5.2.1).
#
# Generates the clear content of the reference set and of the test set with
# FFmpeg. The protected files (svc-3.enc.ts, svc-4.enc.ts,
# test/svc-4.altpid.enc.ts) are made from the clear ones by the packaging tool
# build/bin/package_ts (SRV-PKG, Section 4.7.3), not by this script.
#
# Usage: content/generate.sh            (from any directory)
#
# Service names and program titles are read from content/services.json.
# Every file is written under a temporary name and renamed when complete, so an
# interrupted run never leaves a truncated content file.
set -euo pipefail

CONTENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICES="${CONTENT_DIR}/services.json"
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
DURATION=600   # D-09: 10-minute files

die() { echo "generate.sh: $*" >&2; exit 1; }

for tool in ffmpeg ffprobe python3; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ -f "$FONT" ]] || die "missing font $FONT (package fonts-dejavu-core)"
[[ -f "$SERVICES" ]] || die "missing $SERVICES"
mkdir -p "${CONTENT_DIR}/test"

# Prints number, name, program title 1 and program title 2 of a service, one per line.
service_fields() {
    python3 -c '
import json, sys
services = {s["service_id"]: s for s in json.load(open(sys.argv[1]))["services"]}
s = services[sys.argv[2]]
if len(s["programs"]) != 2:
    sys.exit("services.json: %s must have exactly 2 programs" % sys.argv[2])
print(s["number"]); print(s["name"])
for p in s["programs"]:
    print(p["title"])
' "$SERVICES" "$1"
}

# Texts go into an FFmpeg filter script inside single quotes: reject characters
# that would need escaping there.
check_text() {
    [[ "$1" =~ ^[A-Za-z0-9\ ._-]+$ ]] || die "unsupported characters in text: '$1'"
}

# Writes the video filter of Section 4.7.2 (service name, program title, time code).
write_filter() {  # <file> <name> <title 1> <title 2>
    local style="fontfile=${FONT}:fontsize=48:fontcolor=white:box=1:boxcolor=black:boxborderw=8:x=40"
    cat > "$1" <<EOF
drawtext=${style}:y=40:text='$2',
drawtext=${style}:y=110:text='$3':enable='lt(t,300)',
drawtext=${style}:y=110:text='$4':enable='gte(t,300)',
drawtext=${style}:y=180:text='%{eif\:floor(t/60)\:d\:2}\:%{eif\:mod(floor(t),60)\:d\:2}'
EOF
}

# Common settings of Section 4.7.2.
VIDEO_ENC=(-c:v libx264 -profile:v high -preset veryfast -b:v 2500k -maxrate 2500k -bufsize 2500k
           -g 25 -keyint_min 25 -sc_threshold 0 -flags +cgop)
AUDIO_ENC=(-c:a aac -b:a 128k -ar 48000 -ac 2)
MUX=(-f mpegts -muxrate 3500k -pat_period 0.09 -t "$DURATION")

# generate <service_id> <output relative to content/> <with video: 1|0> [extra mux options...]
generate() {
    local service_id=$1 out=$2 with_video=$3
    shift 3
    local fields number name title1 title2
    fields="$(service_fields "$service_id")" || die "cannot read $service_id from services.json"
    { read -r number; read -r name; read -r title1; read -r title2; } <<< "$fields"
    for t in "$name" "$title1" "$title2"; do check_text "$t"; done

    local tone=$(( 220 * (number + 1) ))   # Section 4.7.2: 220·(n+1) Hz
    local target="${CONTENT_DIR}/${out}"
    local tmp="${target%/*}/.tmp-${target##*/}"
    local filter
    filter="$(mktemp)"
    write_filter "$filter" "$name" "$title1" "$title2"

    local inputs=() maps=() venc=()
    if [[ "$with_video" == 1 ]]; then
        inputs+=(-f lavfi -i "testsrc2=size=1280x720:rate=25")
        maps+=(-map 0:v -map 1:a -filter_script:v "$filter")
        venc=("${VIDEO_ENC[@]}")
    else
        maps+=(-map 0:a)
    fi
    inputs+=(-f lavfi -i "sine=frequency=${tone}:sample_rate=48000")

    echo "generating ${out} (${service_id}, ${name}, ${tone} Hz$([[ $with_video == 1 ]] || echo ', no video'))"
    ffmpeg -hide_banner -loglevel error -nostdin -y "${inputs[@]}" "${maps[@]}" \
        "${venc[@]}" "${AUDIO_ENC[@]}" "${MUX[@]}" "$@" "$tmp"
    mv -f "$tmp" "$target"
    rm -f "$filter"
}

start=$SECONDS
# Reference set (Section 5.2.1): program number 1, PMT PID 4096, video 256, audio 257.
for n in 1 2 3 4; do
    generate "svc-$n" "svc-$n.ts" 1 -mpegts_service_id 1
done
# Test set (Section 5.2.1).
generate svc-1 test/novideo.ts 0 -mpegts_service_id 1
ALT_PIDS=(-mpegts_service_id 2 -mpegts_pmt_start_pid 0x0300 -mpegts_start_pid 0x0401)
generate svc-2 test/svc-2.altpid.ts 1 "${ALT_PIDS[@]}"
generate svc-4 test/svc-4.altpid.ts 1 "${ALT_PIDS[@]}"

echo "done in $(( SECONDS - start )) s. Next: package svc-3.ts, svc-4.ts and test/svc-4.altpid.ts" \
     "with build/bin/package_ts (Section 4.7.3)."

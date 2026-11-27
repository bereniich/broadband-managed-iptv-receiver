#!/usr/bin/env python3
"""RTP receiver of the integration tests (Section 5.4.2, CFG-OP; TST-502, TST-504, TST-622).

Receives on 127.0.0.1:<port> for a given time, checks every RTP header against IF-EXT-05
(Section 3.4), writes the TS payload to a file and prints a summary.

Usage (Python standard library only, NFR-11):
  rtp_capture.py --port 5004 --seconds 15 [--ssrc N] [--out capture.ts]

Checks per datagram: at least 12 bytes; V = 2, P = 0, X = 0, CC = 0, M = 0, PT = 33; the SSRC is the
given one (or, without --ssrc, the one of the first datagram); the payload is 1 - 7 whole TS packets.
A TS packet without the sync byte 0x47 is a TS-level error (REQ-15): it is counted in ts_sync_errors,
and its datagram still counts as a valid RTP datagram. Sequence numbers must increase by 1 (wrap 65535 -> 0); a
forward jump counts the missing numbers as gaps, a backward step as late or duplicate.

Summary lines (key=value): datagrams, invalid (with the reasons), sequence_gaps, late_or_duplicate,
first_datagram_ts_packets, first_datagram_pids, first_arrival_ms and last_arrival_ms (UNIX time in
milliseconds, to compare with the times of /start and /stop), bitrate_last_10s_mbps (TS payload),
ts_sync_errors, ts_bytes. The exit code is 1 if a datagram was invalid, 2 for a usage error, otherwise 0.
"""

import argparse
import collections
import socket
import struct
import sys
import time

TS = 188


def check_header(data, expected_ssrc):
    """Returns (problem or None, ssrc, sequence, packet_count)."""
    if len(data) < 12:
        return 'shorter than 12 bytes', None, None, 0
    b0, b1, seq, _timestamp, ssrc = struct.unpack('!BBHII', data[:12])
    if b0 >> 6 != 2:
        return 'version %d' % (b0 >> 6), ssrc, seq, 0
    if b0 & 0x20:
        return 'padding bit set', ssrc, seq, 0
    if b0 & 0x10:
        return 'extension bit set', ssrc, seq, 0
    if b0 & 0x0F:
        return 'CSRC count %d' % (b0 & 0x0F), ssrc, seq, 0
    if b1 & 0x80:
        return 'marker bit set', ssrc, seq, 0
    if b1 & 0x7F != 33:
        return 'payload type %d' % (b1 & 0x7F), ssrc, seq, 0
    if expected_ssrc is not None and ssrc != expected_ssrc:
        return 'SSRC %d instead of %d' % (ssrc, expected_ssrc), ssrc, seq, 0
    payload = len(data) - 12
    if payload == 0 or payload % TS != 0 or payload > 7 * TS:
        return 'payload of %d bytes' % payload, ssrc, seq, 0
    return None, ssrc, seq, payload // TS


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--seconds', type=float, required=True)
    parser.add_argument('--ssrc', type=int)
    parser.add_argument('--out')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.seconds <= 0:
        parser.error('invalid --port or --seconds')

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    try:
        sock.bind(('127.0.0.1', args.port))
    except OSError as e:
        print('rtp_capture: cannot open UDP port 127.0.0.1:%d: %s' % (args.port, e), file=sys.stderr)
        return 2
    out = open(args.out, 'wb') if args.out else None

    expected_ssrc = args.ssrc
    datagrams = invalid = gaps = late = ts_bytes = sync_errors = 0
    reasons = collections.Counter()
    last_seq = None
    first = None  # (packet count, PIDs) of the first valid datagram
    first_ms = last_ms = None
    recent = collections.deque()  # (arrival time, TS bytes) for the bitrate of the last 10 s
    end = time.monotonic() + args.seconds
    while True:
        left = end - time.monotonic()
        if left <= 0:
            break
        sock.settimeout(left)
        try:
            data = sock.recv(65536)
        except socket.timeout:
            break
        now = time.time()
        datagrams += 1
        if first_ms is None:
            first_ms = now * 1000
        last_ms = now * 1000
        problem, ssrc, seq, count = check_header(data, expected_ssrc)
        if problem:
            invalid += 1
            reasons[problem] += 1
            continue
        if expected_ssrc is None:
            expected_ssrc = ssrc
        if last_seq is not None:
            step = (seq - last_seq - 1) & 0xFFFF
            if step < 0x8000:
                gaps += step
                last_seq = seq
            else:
                late += 1
        else:
            last_seq = seq
        if first is None:
            pids = sorted({((data[i + 1] & 0x1F) << 8) | data[i + 2] for i in range(12, len(data), TS)})
            first = (count, pids)
        payload = data[12:]
        sync_errors += sum(1 for i in range(0, len(payload), TS) if payload[i] != 0x47)  # TS level (REQ-15)
        ts_bytes += len(payload)
        if out:
            out.write(payload)
        recent.append((now, len(payload)))
    if out:
        out.close()

    if recent:
        last_time = recent[-1][0]
        window = [n for (t, n) in recent if t > last_time - 10.0]
        span = min(10.0, last_time - recent[0][0])
        bitrate = sum(window) * 8 / span / 1e6 if span > 0 else 0.0
    else:
        bitrate = 0.0
    print('datagrams=%d' % datagrams)
    print('invalid=%d%s' % (invalid, ''.join(' [%s: %d]' % r for r in sorted(reasons.items()))))
    print('sequence_gaps=%d' % gaps)
    print('late_or_duplicate=%d' % late)
    print('first_datagram_ts_packets=%s' % (first[0] if first else '-'))
    print('first_datagram_pids=%s' % (','.join(map(str, first[1])) if first else '-'))
    print('first_arrival_ms=%s' % ('%.0f' % first_ms if first_ms else '-'))
    print('last_arrival_ms=%s' % ('%.0f' % last_ms if last_ms else '-'))
    print('bitrate_last_10s_mbps=%.3f' % bitrate)
    print('ts_sync_errors=%d' % sync_errors)
    print('ts_bytes=%d' % ts_bytes)
    return 1 if invalid else 0


if __name__ == '__main__':
    sys.exit(main())

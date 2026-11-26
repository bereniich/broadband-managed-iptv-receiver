#!/usr/bin/env python3
"""RTP test sender for the invalid inputs IN-08 - IN-10 (Section 5.2.5) and TST-709.

Sends to 127.0.0.1:<port> (the tuner of the middleware or a receiver of a test).

Usage (Python standard library only, NFR-11):
  rtp_send.py --mode invalid   [--port 5004] [--ssrc N]
      IN-08: V = 1; PT = 96; another SSRC; payloads of 100 bytes, 0 bytes and 8 x 188 bytes;
      1 000 datagrams of random bytes. Every datagram must be counted in rtp_invalid.
  rtp_send.py --mode random    [--port 5004] --rate 1000 --seconds 60
      TST-709: random datagrams (1 - 1 500 bytes) at the given rate.
  rtp_send.py --mode drop50    [--port 5004] [--ssrc N] [--file content/svc-1.ts] [--seconds 20]
      IN-09: a valid stream of the file in which every 50th RTP packet is not sent (its sequence
      number is skipped). rtp_lost must equal the printed number of omitted packets.
  rtp_send.py --mode ts-errors [--port 5004] [--ssrc N] [--file content/svc-1.ts] [--seconds 20]
      IN-10 inside valid RTP: once per second one TS packet with sync byte 0x46, one skipped
      continuity counter on the video PID, one PAT section with a changed CRC byte and one PMT
      section with a section_length larger than its data.

Valid streams send 7 TS packets per datagram at 3.5 Mbit/s (about 332 datagrams/s), with RTP
headers of Section 3.4 and a random first sequence number. A summary of what was sent is printed.
"""

import argparse
import os
import random
import socket
import struct
import sys
import time

TS = 188
RATE_BPS = 3_500_000


def header(seq, timestamp, ssrc, version=2, pt=33):
    return struct.pack('!BBHII', version << 6, pt & 0x7F, seq & 0xFFFF, timestamp & 0xFFFFFFFF, ssrc)


def ts_packets(path):
    with open(path, 'rb') as f:
        while True:
            p = f.read(TS)
            if len(p) < TS:
                return
            yield bytearray(p)


def pid_of(p):
    return ((p[1] & 0x1F) << 8) | p[2]


def section_start(p):
    """Offset of the first section in a packet with payload_unit_start_indicator = 1, or None."""
    afc = (p[3] >> 4) & 0x3
    if not p[1] & 0x40 or not afc & 0x1:
        return None
    offset = 4 + (1 + p[4] if afc & 0x2 else 0)
    if offset >= TS:
        return None
    offset += 1 + p[offset]  # pointer_field
    return offset if offset + 12 <= TS else None


def pmt_pid_of_pat(p):
    o = section_start(p)
    if o is None:
        return None
    end = min(TS, o + 3 + (((p[o + 1] & 0x0F) << 8) | p[o + 2]) - 4)  # without the CRC
    for i in range(o + 8, end - 3, 4):
        if (p[i] << 8) | p[i + 1] != 0:  # program_number 0 is the network PID
            return ((p[i + 2] & 0x1F) << 8) | p[i + 3]
    return None


def video_pid_of_pmt(p):
    o = section_start(p)
    if o is None:
        return None
    end = min(TS, o + 3 + (((p[o + 1] & 0x0F) << 8) | p[o + 2]) - 4)
    i = o + 12 + (((p[o + 10] & 0x0F) << 8) | p[o + 11])
    while i + 5 <= end:
        if p[i] == 0x1B:  # H.264
            return ((p[i + 1] & 0x1F) << 8) | p[i + 2]
        i += 5 + (((p[i + 3] & 0x0F) << 8) | p[i + 4])
    return None


def send_invalid(sock, dest, ssrc):
    sent = {}
    good = bytes([0x47] + [0xFF] * 187) * 7
    other = (ssrc + 1) & 0xFFFFFFFF
    cases = [('version 1', header(1, 0, ssrc, version=1) + good),
             ('payload type 96', header(2, 0, ssrc, pt=96) + good),
             ('other SSRC', header(3, 0, other) + good),
             ('payload of 100 bytes', header(4, 0, ssrc) + good[:100]),
             ('payload of 0 bytes', header(5, 0, ssrc)),
             ('payload of 8 x 188 bytes', header(6, 0, ssrc) + good + good[:TS])]
    for name, data in cases:
        sock.sendto(data, dest)
        sent[name] = 1
        time.sleep(0.002)
    rng = random.Random(8)
    for i in range(1000):
        sock.sendto(bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 1500))), dest)
        if i % 50 == 49:
            time.sleep(0.01)  # within the receive buffer
    sent['random datagrams'] = 1000
    return sent


def send_random(sock, dest, rate, seconds):
    rng = random.Random(9)
    start = time.monotonic()
    count = 0
    while time.monotonic() - start < seconds:
        sock.sendto(os.urandom(rng.randint(1, 1500)), dest)
        count += 1
        delay = start + count / rate - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    return {'random datagrams': count}


def send_stream(sock, dest, ssrc, path, seconds, mode):
    """A valid paced stream of `path`; mode 'drop50' or 'ts-errors' adds the faults of IN-09 / IN-10."""
    seq = random.randint(0, 0xFFFF)
    datagram_interval = 7 * TS * 8 / RATE_BPS
    start = time.monotonic()
    counts = {'datagrams sent': 0, 'datagrams omitted': 0, 'sync errors': 0, 'skipped continuity counters': 0,
              'PAT with a changed CRC byte': 0, 'PMT with a too large section_length': 0}
    video_pid = pmt_pid = None
    fault_second = -1
    pending = {'sync': False, 'cc': False, 'pat': False, 'pmt': False}
    block = []
    index = 0
    while time.monotonic() - start < seconds:
        for p in ts_packets(path):
            if time.monotonic() - start >= seconds:
                break
            pid = pid_of(p)
            if pid == 0 and pmt_pid is None:
                pmt_pid = pmt_pid_of_pat(p)
            if pid == pmt_pid and video_pid is None:
                video_pid = video_pid_of_pmt(p)
            if mode == 'ts-errors':
                second = int(time.monotonic() - start)
                if second != fault_second:
                    fault_second = second
                    pending = {'sync': True, 'cc': True, 'pat': True, 'pmt': True}
                if pending['sync'] and pid not in (0, pmt_pid):
                    p[0] = 0x46
                    pending['sync'] = False
                    counts['sync errors'] += 1
                elif pending['cc'] and pid == video_pid:
                    pending['cc'] = False  # this video packet is not sent: its counter is skipped
                    counts['skipped continuity counters'] += 1
                    continue
                elif pending['pat'] and pid == 0 and section_start(p) is not None:
                    o = section_start(p)
                    length = ((p[o + 1] & 0x0F) << 8) | p[o + 2]
                    p[min(TS - 1, o + 3 + length - 1)] ^= 0xFF  # last CRC byte
                    pending['pat'] = False
                    counts['PAT with a changed CRC byte'] += 1
                elif pending['pmt'] and pid == pmt_pid and section_start(p) is not None:
                    o = section_start(p)
                    p[o + 1] = (p[o + 1] & 0xF0) | 0x0F  # section_length 0x0Fxx: more than the data
                    pending['pmt'] = False
                    counts['PMT with a too large section_length'] += 1
            block.append(bytes(p))
            if len(block) < 7:
                continue
            index += 1
            timestamp = int((time.monotonic() - start) * 90000)
            if mode == 'drop50' and index % 50 == 0:
                counts['datagrams omitted'] += 1  # the sequence number is used, the datagram not sent
            else:
                sock.sendto(header(seq, timestamp, ssrc) + b''.join(block), dest)
                counts['datagrams sent'] += 1
            seq = (seq + 1) & 0xFFFF
            block = []
            delay = start + index * datagram_interval - time.monotonic()
            if delay > 0:
                time.sleep(delay)
    return {k: v for k, v in counts.items() if v or k == 'datagrams omitted' and mode == 'drop50'}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--mode', required=True, choices=['invalid', 'random', 'drop50', 'ts-errors'])
    parser.add_argument('--port', type=int, default=5004)
    parser.add_argument('--ssrc', type=int, default=1180331842)
    parser.add_argument('--file', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                                                       'content', 'svc-1.ts'))
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--rate', type=float, default=1000)
    args = parser.parse_args()
    if args.mode in ('drop50', 'ts-errors') and not os.path.exists(args.file):
        print('rtp_send: content missing: run content/generate.sh (%s)' % args.file, file=sys.stderr)
        return 2
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = ('127.0.0.1', args.port)
    if args.mode == 'invalid':
        result = send_invalid(sock, dest, args.ssrc)
    elif args.mode == 'random':
        result = send_random(sock, dest, args.rate, args.seconds)
    else:
        result = send_stream(sock, dest, args.ssrc, args.file, args.seconds, args.mode)
    for name, count in result.items():
        print('%s=%d' % (name.replace(' ', '_'), count))
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""HLS checker for IF-INT-02 (Section 5.4.2).

Loads index.m3u8 of a session every 500 ms, checks its syntax against Section 3.7, downloads every
newly listed segment, runs `ffprobe -show_programs -show_streams` and a decode check
`ffmpeg -v error -i <seg> -f null -` on it, and extracts the first video frame of a segment as PNG
on request.

Usage (Python standard library only, NFR-11; needs ffprobe and ffmpeg):
  hls_check.py (--url URL | --session N) [--seconds 30] [--out DIR] [--png SEQ]...
               [--png-after-discontinuity] [--hls-dir DIR] [--quiet]

  --url URL         playlist URL, e.g. http://127.0.0.1:8090/hls/3/index.m3u8
  --session N       the same for session N on --host / --port (default 127.0.0.1:8090)
  --seconds S       stop after S seconds (default 30); also stops when the playlist that was
                    already loaded answers 404 (the session ended, Section 3.7 "End")
  --out DIR         directory for the PNG files (seg-<k>.png); needed with --png options
  --png SEQ         extract the first video frame of segment SEQ (repeatable)
  --png-after-discontinuity
                    extract it for every segment preceded by #EXT-X-DISCONTINUITY
  --hls-dir DIR     also count, every 50 ms, the segment files in the HLS directory of the session
                    (every file whose name ends in seg-<k>.ts, so also a segment being written under
                    a temporary name) and report more than 8 as an error (REQ-23, B7)

Checks per playlist: #EXTM3U first; #EXT-X-VERSION:3; #EXT-X-TARGETDURATION:2;
#EXT-X-MEDIA-SEQUENCE equal to the sequence of the first listed segment seg-<k>.ts and consecutive
sequences; #EXTINF:<d.ddd>, with 3 decimals and at most 2.000; 1 - 6 segments; no #EXT-X-ENDLIST
and no other tag; HTTP Content-Type application/vnd.apple.mpegurl and Cache-Control no-cache.
Across reloads: the media sequence does not decrease, a listed segment keeps its duration and its
discontinuity mark, and #EXT-X-DISCONTINUITY-SEQUENCE (0 when absent) grows by the number of marked
segments that left the playlist (REQ-24).
Checks per segment: HTTP Content-Type video/mp2t; size a multiple of 188, sync byte 0x47 and
transport_scrambling_control '00' on every packet; ffprobe finds exactly one program; only PID 0,
the PMT PID and the PIDs of the streams found by ffprobe occur; the first three packets are PAT,
PMT and the video PID with payload_unit_start_indicator = 1 and random_access_indicator = 1
(Section 3.7, REQ-23); the decode check prints nothing.

Output: one line per checked segment and per error, then a summary. The exit code is 0 if no error
was found, 1 otherwise, 2 for a usage error (also when ffprobe or ffmpeg is missing).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

TS = 188
MAX_LISTED = 6            # Section 3.7
MAX_SEGMENT_FILES = 8     # REQ-23, B7
MAX_DURATION = 2.0        # Section 3.7: at most 2 s

EXTINF = re.compile(r'#EXTINF:([0-9]+\.[0-9]{3}),')
SEGMENT = re.compile(r'seg-([0-9]+)\.ts')
SEGMENT_FILE = re.compile(r'.*seg-[0-9]+\.ts')


class Playlist:
    def __init__(self):
        self.media_sequence = None
        self.discontinuity_sequence = 0
        self.segments = []      # [(sequence, duration text, discontinuity before it)]


def parse_playlist(text):
    """Returns (Playlist, errors) for the text of index.m3u8 (Section 3.7)."""
    errors = []
    p = Playlist()
    lines = [l.rstrip('\r') for l in text.split('\n')]
    if not lines or lines[0] != '#EXTM3U':
        errors.append('first line is not #EXTM3U')
    seen = set()
    duration = None
    discontinuity = False
    for n, line in enumerate(lines[1:], 2):
        if line == '':
            continue
        where = 'line %d' % n
        header = line.split(':', 1)[0]
        if header in ('#EXT-X-VERSION', '#EXT-X-TARGETDURATION', '#EXT-X-MEDIA-SEQUENCE',
                      '#EXT-X-DISCONTINUITY-SEQUENCE'):
            if header in seen:
                errors.append('%s: %s appears twice' % (where, header))
            if p.segments or duration is not None or discontinuity:
                errors.append('%s: %s after the first segment' % (where, header))
            seen.add(header)
            value = line[len(header) + 1:] if ':' in line else ''
            if header == '#EXT-X-VERSION' and value != '3':
                errors.append('%s: must be #EXT-X-VERSION:3' % where)
            elif header == '#EXT-X-TARGETDURATION' and value != '2':
                errors.append('%s: must be #EXT-X-TARGETDURATION:2' % where)
            elif header in ('#EXT-X-MEDIA-SEQUENCE', '#EXT-X-DISCONTINUITY-SEQUENCE'):
                if not value.isdigit():
                    errors.append('%s: %s needs a decimal integer' % (where, header))
                elif header == '#EXT-X-MEDIA-SEQUENCE':
                    p.media_sequence = int(value)
                else:
                    p.discontinuity_sequence = int(value)
        elif line == '#EXT-X-DISCONTINUITY':
            if discontinuity:
                errors.append('%s: two #EXT-X-DISCONTINUITY for one segment' % where)
            discontinuity = True
        elif line.startswith('#EXTINF:'):
            m = EXTINF.fullmatch(line)
            if duration is not None:
                errors.append('%s: #EXTINF without a segment URI before it' % where)
            if not m:
                errors.append('%s: must be #EXTINF:<duration with 3 decimals>, (is %s)' % (where, line))
                duration = '?'
            else:
                duration = m.group(1)
                if not 0 < float(duration) <= MAX_DURATION:
                    errors.append('%s: duration %s not in (0, 2.000]' % (where, duration))
        elif line.startswith('#'):
            errors.append('%s: tag not allowed by Section 3.7: %s' % (where, line))
        else:
            m = SEGMENT.fullmatch(line)
            if duration is None:
                errors.append('%s: segment URI without #EXTINF' % where)
            if not m:
                errors.append('%s: segment URI must be seg-<sequence>.ts (is %s)' % (where, line))
            else:
                p.segments.append((int(m.group(1)), duration, discontinuity))
            duration = None
            discontinuity = False
    if duration is not None or discontinuity:
        errors.append('the playlist ends before the URI of its last segment')
    for tag in ('#EXT-X-VERSION', '#EXT-X-TARGETDURATION', '#EXT-X-MEDIA-SEQUENCE'):
        if tag not in seen:
            errors.append('%s missing' % tag)
    if not 1 <= len(p.segments) <= MAX_LISTED:
        errors.append('%d segments listed (1 - %d allowed)' % (len(p.segments), MAX_LISTED))
    if p.segments and p.media_sequence is not None:
        expected = list(range(p.media_sequence, p.media_sequence + len(p.segments)))
        if [s[0] for s in p.segments] != expected:
            errors.append('segments %s do not follow #EXT-X-MEDIA-SEQUENCE:%d consecutively'
                          % ([s[0] for s in p.segments], p.media_sequence))
    return p, errors


def compare_playlists(old, new):
    """Errors between two consecutive loads of the playlist of one session."""
    errors = []
    if old.media_sequence is None or new.media_sequence is None:
        return errors
    if new.media_sequence < old.media_sequence:
        errors.append('#EXT-X-MEDIA-SEQUENCE decreased from %d to %d' % (old.media_sequence, new.media_sequence))
        return errors
    before = {s[0]: s for s in old.segments}
    for s in new.segments:
        o = before.get(s[0])
        if o is not None and o[1:] != s[1:]:
            errors.append('seg-%d.ts changed from duration %s discontinuity %s to %s %s'
                          % (s[0], o[1], o[2], s[1], s[2]))
    if old.segments and new.media_sequence <= old.segments[-1][0] + 1:
        left = sum(1 for s in old.segments if s[0] < new.media_sequence and s[2])
        expected = old.discontinuity_sequence + left
        if new.discontinuity_sequence != expected:
            errors.append('#EXT-X-DISCONTINUITY-SEQUENCE is %d, expected %d (%d marked segment(s) left)'
                          % (new.discontinuity_sequence, expected, left))
    elif new.discontinuity_sequence < old.discontinuity_sequence:
        errors.append('#EXT-X-DISCONTINUITY-SEQUENCE decreased')
    return errors


def scan_ts(data, allowed_pids, pmt_pid, video_pid):
    """Own packet scan of one segment; returns (set of PIDs, errors)."""
    errors = []
    pids = set()
    if len(data) == 0 or len(data) % TS:
        errors.append('size %d is not a positive multiple of 188' % len(data))
    first = []
    for i in range(0, len(data) - len(data) % TS, TS):
        p = data[i:i + TS]
        if p[0] != 0x47:
            errors.append('packet %d: sync byte 0x%02X' % (i // TS, p[0]))
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pids.add(pid)
        if p[3] & 0xC0:
            errors.append('packet %d (PID %d): transport_scrambling_control is not 00' % (i // TS, pid))
        if len(first) < 3:
            pusi = bool(p[1] & 0x40)
            rai = bool(p[3] & 0x20) and p[4] > 0 and bool(p[5] & 0x40)
            first.append((pid, pusi, rai))
    if allowed_pids is not None and not pids <= allowed_pids:
        errors.append('PIDs %s not in the program (allowed %s)' % (sorted(pids - allowed_pids), sorted(allowed_pids)))
    expected = [0, pmt_pid, video_pid]
    if len(first) < 3 or [f[0] for f in first] != expected:
        errors.append('first packets have PIDs %s, expected PAT, PMT, video %s' % ([f[0] for f in first], expected))
    elif not (first[2][1] and first[2][2]):
        errors.append('third packet: video without payload_unit_start_indicator = 1 and random_access_indicator = 1')
    return pids, errors


def count_segment_files(directory):
    try:
        names = os.listdir(directory)
    except FileNotFoundError:
        return 0, False
    return sum(1 for n in names if SEGMENT_FILE.fullmatch(n)), 'index.m3u8' in names


class DirWatcher(threading.Thread):
    def __init__(self, directory, interval_s=0.05):
        super().__init__(daemon=True)
        self.directory = directory
        self.interval_s = interval_s
        self.stop_event = threading.Event()
        self.samples = 0
        self.max_files = 0
        self.playlist_seen = False

    def run(self):
        while not self.stop_event.is_set():
            n, playlist = count_segment_files(self.directory)
            self.samples += 1
            self.max_files = max(self.max_files, n)
            self.playlist_seen = self.playlist_seen or playlist
            self.stop_event.wait(self.interval_s)


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # no proxy for 127.0.0.1


def http_get(url):
    """(status, headers, body); status 0 if there was no answer."""
    try:
        with OPENER.open(url, timeout=2) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, b''
    except (urllib.error.URLError, OSError):
        return 0, None, b''


def probe(path):
    """(allowed PIDs, PMT PID, video PID, description, errors) from ffprobe (Section 5.1.7 rule 5)."""
    r = subprocess.run(['ffprobe', '-v', 'error', '-show_programs', '-show_streams', '-of', 'json', path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, None, None, '', ['ffprobe failed: %s' % r.stderr.strip()[:200]]
    info = json.loads(r.stdout)
    programs = info.get('programs', [])
    if len(programs) != 1:
        return None, None, None, '', ['ffprobe finds %d programs, expected 1' % len(programs)]
    prog = programs[0]
    pmt_pid = prog.get('pmt_pid')
    allowed = {0, pmt_pid}
    video_pid = None
    parts = ['program %s pmt %s' % (prog.get('program_num'), pmt_pid)]
    for s in prog.get('streams', []):
        pid = int(s['id'], 16)
        allowed.add(pid)
        if s.get('codec_type') == 'video' and video_pid is None:
            video_pid = pid
            parts.append('video %d %s %s %sx%s' % (pid, s.get('codec_name'), s.get('profile'), s.get('width'), s.get('height')))
        elif s.get('codec_type') == 'audio':
            parts.append('audio %d %s %s %s Hz %s ch' % (pid, s.get('codec_name'), s.get('profile'),
                                                       s.get('sample_rate'), s.get('channels')))
    errors = [] if video_pid is not None else ['ffprobe finds no video stream']
    return allowed, pmt_pid, video_pid, ', '.join(parts), errors


def decode_errors(path):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'null', '-'], capture_output=True, text=True)
    text = r.stderr.strip()
    if r.returncode != 0 or text:
        return ['decode check: %s' % (text.splitlines()[0] if text else 'exit code %d' % r.returncode)]
    return []


def extract_png(path, png):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', path, '-frames:v', '1', png], capture_output=True, text=True)
    return [] if r.returncode == 0 else ['PNG extraction failed: %s' % r.stderr.strip()[:200]]


class Checker:
    def __init__(self, url, args, work):
        self.url = url
        self.base = url.rsplit('/', 1)[0] + '/'
        self.args = args
        self.work = work
        self.errors = []
        self.last = None
        self.checked = set()
        self.loads = 0
        self.max_listed = 0
        self.marked = set()
        self.max_dseq = 0

    def error(self, text):
        self.errors.append(text)
        print('ERROR %s' % text, flush=True)

    def poll(self):
        """False when the session has ended (404 after the playlist was loaded)."""
        status, headers, body = http_get(self.url)
        if status != 200:
            if self.last is not None and status == 404:
                print('playlist answers 404: the session has ended', flush=True)
                return False
            if self.last is not None:
                self.error('playlist: HTTP status %d' % status)
            return True
        self.loads += 1
        ctype = headers.get('Content-Type', '')
        if ctype.split(';')[0].strip() != 'application/vnd.apple.mpegurl':
            self.error('playlist: Content-Type %r' % ctype)
        if 'no-cache' not in headers.get('Cache-Control', ''):
            self.error('playlist: Cache-Control %r' % headers.get('Cache-Control'))
        playlist, errors = parse_playlist(body.decode('utf-8', 'replace'))
        if self.last is not None:
            errors += compare_playlists(self.last, playlist)
        for e in errors:
            self.error('playlist load %d: %s' % (self.loads, e))
        if errors and not self.args.quiet:
            print(body.decode('utf-8', 'replace'), flush=True)
        self.last = playlist
        self.max_listed = max(self.max_listed, len(playlist.segments))
        self.max_dseq = max(self.max_dseq, playlist.discontinuity_sequence)
        for seq, duration, disc in playlist.segments:
            if disc:
                self.marked.add(seq)
            if seq not in self.checked:
                self.checked.add(seq)
                self.check_segment(seq, duration, disc)
        return True

    def check_segment(self, seq, duration, disc):
        name = 'seg-%d.ts' % seq
        status, headers, data = http_get(self.base + name)
        errors = []
        if status != 200:
            errors.append('HTTP status %d' % status)
        else:
            ctype = headers.get('Content-Type', '')
            if ctype.split(';')[0].strip() != 'video/mp2t':
                errors.append('Content-Type %r' % ctype)
            path = os.path.join(self.work, name)
            with open(path, 'wb') as f:
                f.write(data)
            allowed, pmt_pid, video_pid, description, perrors = probe(path)
            errors += perrors
            pids, serrors = scan_ts(data, allowed, pmt_pid, video_pid)
            errors += serrors
            errors += decode_errors(path)
            if self.args.out and (seq in self.args.png or (disc and self.args.png_after_discontinuity)):
                png = os.path.join(self.args.out, 'seg-%d.png' % seq)
                errors += extract_png(path, png)
                description += ', frame %s' % png
            os.remove(path)
            if not self.args.quiet or errors:
                print('%d %s duration %s%s size %d PIDs %s; %s' % (int(time.time() * 1000), name, duration,
                      ' after #EXT-X-DISCONTINUITY' if disc else '', len(data),
                      ','.join(str(p) for p in sorted(pids)), description), flush=True)
        for e in errors:
            self.error('%s: %s' % (name, e))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    where = parser.add_mutually_exclusive_group(required=True)
    where.add_argument('--url')
    where.add_argument('--session', type=int)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8090)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--out')
    parser.add_argument('--png', type=int, action='append', default=[], metavar='SEQ')
    parser.add_argument('--png-after-discontinuity', action='store_true')
    parser.add_argument('--hls-dir')
    parser.add_argument('--quiet', action='store_true', help='print only errors and the summary')
    args = parser.parse_args(argv)
    if (args.png or args.png_after_discontinuity) and not args.out:
        parser.error('--png and --png-after-discontinuity need --out')
    for tool in ('ffprobe', 'ffmpeg'):
        if shutil.which(tool) is None:
            print('hls_check: %s not found' % tool, file=sys.stderr)
            return 2
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    url = args.url or 'http://%s:%d/hls/%d/index.m3u8' % (args.host, args.port, args.session)

    watcher = DirWatcher(args.hls_dir) if args.hls_dir else None
    if watcher:
        watcher.start()
    with tempfile.TemporaryDirectory(prefix='hls_check-') as work:
        checker = Checker(url, args, work)
        end = time.monotonic() + args.seconds
        next_poll = time.monotonic()
        while time.monotonic() < end:
            if not checker.poll():
                break
            next_poll = max(next_poll + 0.5, time.monotonic())   # no burst after a slow segment check
            time.sleep(max(0.0, min(next_poll, end) - time.monotonic()))
    if checker.last is None:
        checker.error('the playlist %s was never loaded' % url)
    if watcher:
        watcher.stop_event.set()
        watcher.join()
        if watcher.max_files > MAX_SEGMENT_FILES:
            checker.error('%s: up to %d segment files (at most %d allowed)' % (args.hls_dir, watcher.max_files,
                                                                               MAX_SEGMENT_FILES))
        print('directory %s: %d samples, at most %d segment files, playlist file %s' % (
            args.hls_dir, watcher.samples, watcher.max_files, 'seen' if watcher.playlist_seen else 'never seen'))
    print('%s: %d playlist loads, %d segments checked, at most %d listed, %d discontinuities, '
          'discontinuity sequence up to %d, %d error(s)' % (
              'FAIL' if checker.errors else 'PASS', checker.loads, len(checker.checked), checker.max_listed,
              len(checker.marked), checker.max_dseq, len(checker.errors)))
    return 1 if checker.errors else 0


if __name__ == '__main__':
    sys.exit(main())

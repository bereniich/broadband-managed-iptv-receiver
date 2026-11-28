#!/usr/bin/env python3
"""Runner of the WP-AL integration tests of CFG-MW (Section 5.4.3).

Starts P4 (mw_server), P3 (headend) and P2 (dtv-middleware) with the configurations of
scripts/start.sh (server/config.json, middleware/main/middleware.json), plays the role of the Web
application with the tools of tests/tools/ (the WebSocket client of ws_client.py, hls_check.py,
contract_check.py), checks the expected results of Section 5.4.3 and stops the processes it started
(only these PIDs).

Usage (Python standard library only, NFR-11), from any directory:
  tests/integration/run_mw.py [--tests 521,...,531,533] [--build build] [--evidence]

  --evidence  writes the evidence of the column "Evidence" of Section 5.4.3 to tests/evidence/<TST-id>/,
              named with the date and the commit (Section 5.1.6): TESTOUT for every test, the LOG
              excerpts of the three processes, FFPROBE and the PNG of the PHOTO checks. Refused for a
              modified working tree, because such a run is not evidence (Section 5.1.7).

Tests:
  TST-521  start without P4: mw_status false, MW_UNREACHABLE; P4 started: mw_status true within 5.5 s,
           the services of 5.2.3; P4 stopped: the same list, get_epg MW_UNREACHABLE
  TST-522  svc-1 and svc-2 live for 30 s each: events, stream_info against ffprobe of the content
           file, hls_check.py (playlist, segments, PIDs, at most 8 segment files), keepalives every
           5 s +/- 0.5 s in the head-end log, PNG of a segment (PHOTO: burned-in service name)
  TST-523  test set (server/config.test.json): svc-2 with PIDs 768 / 1025 / 1026, svc-4 with K4 (key_required,
           then playing), svc-1 without video (error INVALID_STREAM)
  TST-524  archive start of svc-2 at the previous event: start = start_time = T; PNG of the first frame of
           seg-0.ts (PHOTO: time code T mod 600 +/- 1 s)
  TST-525  protected rewind of svc-3 at T: no key, KX (rejected within 3 s of the first Locked of the attempt),
           K3 after 60 s (playing from T; PNG of seg-0.ts), live without the key dialog, K3 / K4 for svc-4;
           the HLS directory is listed every 50 ms
  TST-526  latest request wins: svc-4 then svc-3 after 50 ms, then 10 plays of svc-1 / svc-2 every 100 ms
  TST-527  set_key during a start: 20 ms after play, and as soon as validating_key is received
  TST-528  P3 stopped during playing: no_signal after 3 s, error at the third failed keepalive (15 s), a new
           play after the restart of P3; second run: P3 restarted within 5 s (SESSION_NOT_FOUND, no /stop)
  TST-529  P3 suspended for 5 s (SIGSTOP / SIGCONT): no_signal, playing again, #EXT-X-DISCONTINUITY, skip
           warning of the head-end, PNG of the first new segment (PHOTO: time code now mod 600 +/- 2 s)
  TST-530  SIGTERM to P2: /stop and exit code 0 within 3 s; SIGKILL: session expired 15 - 16 s after the
           last keepalive
  TST-531  loop boundary: live started at now mod 600 = 580 (60 s) and archive at T mod 600 = 590 (30 s);
           waits up to 10 min for the start point of the live run
  TST-533  contract_check.py on the JSON examples of Section 3, the response files of the fake HTTP
           client of WP-AL and the responses of P3 / P4 recorded by this run (the requests of
           TST-501 and TST-502)
A test with a PHOTO check ends with "passed pending the PHOTO inspection", which is not a status of
Section 5.1.5: the executing member checks the PNG, and only then the test is recorded as "passed".

Expected values come from the specification (Sections 3, 5.2.1, 5.2.3, 5.4.3) and from ffprobe,
never from the output of the code under test (Section 5.1.7, rule 5).

Every test that depends on the stored keys (TST-523, 525, 526, 527) restarts P2 first, so that no key
of an earlier test is stored (as PRE-2 of Section 5.5.1).

Needs: build/bin/dtv-middleware, headend, mw_server; ffmpeg / ffprobe; the content of Section 5.2.1
and its test set (with the encrypted files of package_ts); free ports 8080, 8081, 8090 (TCP) and 5004 (UDP). The exit
code is 0 if every executed test passed.
"""

import argparse
import datetime
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
TOOLS = os.path.join(REPO, 'tests', 'tools')
sys.path.insert(0, TOOLS)
import ws_client  # noqa: E402  (tests/tools/ws_client.py: the RFC 6455 client of Section 5.4.2)

SERVER_CONFIG = os.path.join('server', 'config.json')
TEST_CONFIG = os.path.join('server', 'config.test.json')         # test set, Section 5.2.1
MW_CONFIG = os.path.join('middleware', 'main', 'middleware.json')
MW_PORT, HE_PORT, UI_PORT, RTP_PORT = 8080, 8081, 8090, 5004    # Section 2.2.5
HLS_DIR = '/tmp/m15-hls'                                        # Section 4.2.5
LOGS = {'P2': 'logs/middleware.log', 'P3': 'logs/headend.log', 'P4': 'logs/mw_server.log'}  # NFR-13

# Section 5.2.3: service_id, number, name, protected.
SERVICES = [('svc-1', 1, 'Demo One', False), ('svc-2', 2, 'Demo Two', False),
            ('svc-3', 3, 'Premium One', True), ('svc-4', 4, 'Premium Two', True)]
CONTENT = {'svc-1': 'content/svc-1.ts', 'svc-2': 'content/svc-2.ts'}   # Section 5.2.1, reference set
K3 = '3A7F1C9E5B2D8046A1E3C5079B4D6F28'   # keys of Section 5.2.2 (test code only, NFR-08)
K4 = 'C04E92B7165FD83A2E7B0C9145D6A3F1'
KX = '00112233445566778899AABBCCDDEEFF'
LOG_TIME = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}) ')       # NFR-13


def now_ms():
    return int(time.time() * 1000)


def log_time_ms(line):
    """UNIX time in ms of an NFR-13 log line (ISO local time with ms), or None."""
    m = LOG_TIME.match(line)
    if not m:
        return None
    return int(datetime.datetime.strptime(m.group(1), '%Y-%m-%dT%H:%M:%S.%f').timestamp() * 1000)


class Result:
    def __init__(self, tst):
        self.tst, self.lines, self.failures, self.status = tst, [], 0, None
        self.files = {}     # extra TESTOUT sections
        self.ffprobe = ''   # FFPROBE evidence
        self.photos = []    # PHOTO evidence: PNG files for the inspection
        self.log = ''       # LOG evidence: what the processes logged during the test

    def check(self, ok, text):
        self.lines.append('%s %s' % ('PASS' if ok else 'FAIL', text))
        if not ok:
            self.failures += 1
        return ok

    def note(self, text):
        self.lines.append('     ' + text)

    def measured(self, text):
        self.lines.append('     TST-%d measured: %s' % (self.tst, text))


# ---------------------------------------------------------------- processes

def port_free(port, kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)   # connections in TIME_WAIT do not count
        try:
            s.bind(('127.0.0.1', port))
            return True
        except OSError:
            return False


def wait_port(port, timeout=10.0):
    """Time in ms at which the TCP port accepts a connection, or None."""
    end = time.time() + timeout
    while time.time() < end:
        with socket.socket() as s:
            if s.connect_ex(('127.0.0.1', port)) == 0:
                return now_ms()
        time.sleep(0.02)
    return None


class Processes:
    """P2, P3, P4 started by this runner; only these PIDs are ever signalled."""
    COMMANDS = {'P4': ('mw_server', SERVER_CONFIG, MW_PORT), 'P3': ('headend', SERVER_CONFIG, HE_PORT),
                'P2': ('dtv-middleware', MW_CONFIG, UI_PORT)}

    def __init__(self, build, tmp):
        self.build, self.tmp, self.procs, self.configs = build, tmp, {}, {}

    def start(self, name, config=None):
        exe, default_config, port = self.COMMANDS[name]
        config = config or default_config
        self.configs[name] = config
        out = open(os.path.join(self.tmp, '%s-%d.out' % (exe, now_ms())), 'w')
        p = subprocess.Popen([os.path.join(self.build, 'bin', exe), '--config', config], cwd=REPO,
                             stdout=out, stderr=subprocess.STDOUT)
        self.procs[name] = p
        opened = wait_port(port)
        if opened is None or p.poll() is not None:
            raise RuntimeError('%s did not open TCP port %d (exit code %s, output %s)' % (exe, port, p.poll(), out.name))
        return opened

    def running(self, name):
        p = self.procs.get(name)
        return p is not None and p.poll() is None

    def ensure(self, *names):
        """Running with the reference configuration."""
        for name in names:
            if self.running(name) and self.configs.get(name) != self.COMMANDS[name][1]:
                self.stop(name)
            if not self.running(name):
                self.start(name)

    def restart(self, name, config=None):
        self.stop(name)
        return self.start(name, config)

    def send(self, name, sig):
        self.procs[name].send_signal(sig)

    def stop(self, name, sig=signal.SIGTERM, timeout=5.0):
        """Exit code, or None if the process had to be killed."""
        p = self.procs.get(name)
        if p is None or p.poll() is not None:
            return p.returncode if p else None
        p.send_signal(sig)
        try:
            return p.wait(timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            return None

    def stop_all(self):
        for name in ('P2', 'P3', 'P4'):   # P2 first: its orderly stop sends POST /stop to P3 (REQ-70)
            self.stop(name)


class Logs:
    """Excerpts of the logs of the three processes, from a mark on."""

    def __init__(self):
        self.offsets = {}

    def mark(self):
        self.offsets = {k: (os.path.getsize(os.path.join(REPO, p)) if os.path.exists(os.path.join(REPO, p)) else 0)
                        for k, p in LOGS.items()}

    def lines(self, name):
        path = os.path.join(REPO, LOGS[name])
        if not os.path.exists(path):
            return []
        with open(path, errors='replace') as f:
            f.seek(self.offsets.get(name, 0))
            return f.read().splitlines()

    def excerpt(self):
        return ''.join('--- %s ---\n%s\n' % (LOGS[k], '\n'.join(self.lines(k))) for k in ('P2', 'P3', 'P4'))


# ---------------------------------------------------------------- IF-INT-01 (the role of the APP)

class Ui:
    """One connection to the control WebSocket (Section 3.6); every frame is recorded in the format of
    ws_client.py --record (input of contract_check.py --kind ws), the key of set_key masked (REQ-55)."""

    def __init__(self, record_path):
        self.conn = ws_client.Connection('ws://127.0.0.1:%d/ws/control' % UI_PORT)
        self.record = open(record_path, 'a', encoding='utf-8')
        self.messages = []   # (t_ms, message or None)
        self.next_id = 1

    def _log(self, direction, text):
        self.record.write(json.dumps({'t_ms': now_ms(), 'dir': direction, 'kind': 'text', 'text': text},
                                     ensure_ascii=False) + '\n')
        self.record.flush()

    def command(self, name, params=None):
        cid = self.next_id
        self.next_id += 1
        text = json.dumps({'type': 'command', 'id': cid, 'name': name, 'params': params or {}})
        self.conn.send(ws_client.OP_TEXT, text.encode())
        self._log('sent', ws_client.masked(text))
        return cid

    def pump(self, seconds):
        """Receives for the whole `seconds` (a step delay of the test)."""
        end = time.time() + seconds
        while True:
            self._receive(max(0.0, end - time.time()))
            if time.time() >= end:
                return

    def _receive(self, seconds):
        """Receives until the first messages arrive or `seconds` have passed."""
        for opcode, payload in self.conn.receive(seconds):
            if opcode != ws_client.OP_TEXT:
                continue
            text = payload.decode('utf-8')
            self._log('recv', text)
            try:
                m = json.loads(text)
            except ValueError:
                m = None
            self.messages.append((now_ms(), m if isinstance(m, dict) else None))

    def wait(self, pred, timeout_s, since=0):
        """(t_ms, message) of the first received message from index `since` on that satisfies `pred`."""
        end = time.time() + timeout_s
        while True:
            for t, m in self.messages[since:]:
                if m is not None and pred(m):
                    return t, m
            if time.time() >= end:
                return None, None
            self._receive(min(0.05, max(0.0, end - time.time())))

    def call(self, name, params=None, timeout_s=2.0):
        """Sends a command and waits for its response (Section 3.6.1: within 1 s)."""
        since = len(self.messages)
        cid = self.command(name, params)
        t0 = now_ms()
        t, m = self.wait(lambda m: m.get('type') == 'response' and m.get('id') == cid, timeout_s, since)
        return m, (t - t0 if t else None)

    def events(self, name, session=None):
        return [(t, m) for t, m in self.messages if m and m.get('type') == 'event' and m.get('name') == name
                and (session is None or m.get('data', {}).get('session') == session)]

    def states(self, session):
        return [m['data'].get('state') for _, m in self.events('session_state', session)]

    def wait_state(self, session, state, timeout_s, since=0):
        return self.wait(lambda m: m.get('type') == 'event' and m.get('name') == 'session_state'
                         and m['data'].get('session') == session and m['data'].get('state') == state,
                         timeout_s, since)

    def close(self):
        self.conn.close()
        self.record.close()


# ---------------------------------------------------------------- tools

def ffprobe_json(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_programs', '-show_streams', '-of', 'json', path],
                         capture_output=True, text=True)
    try:
        return json.loads(out.stdout), out.stdout
    except ValueError:
        return {}, out.stdout + out.stderr


def stream_type(stream):
    tag = stream.get('codec_tag', '0x0000')
    return int(tag, 16) & 0xFF


def start_hls_check(ctx, session, seconds, png=(), after_discontinuity=False):
    out_dir = os.path.join(ctx.tmp, 'hls-%d' % session)
    os.makedirs(out_dir, exist_ok=True)
    cmd = [sys.executable, '-I', os.path.join(TOOLS, 'hls_check.py'), '--session', str(session), '--seconds',
           str(seconds), '--hls-dir', os.path.join(HLS_DIR, str(session)), '--out', out_dir]
    for seq in png:
        cmd += ['--png', str(seq)]
    if after_discontinuity:
        cmd.append('--png-after-discontinuity')
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True), out_dir


def finish_hls_check(proc, res=None):
    out, _ = proc.communicate()
    if res is not None:   # the errors found, at most 10
        for line in [l for l in out.splitlines() if l.startswith('ERROR')][:10]:
            res.note('hls_check: ' + line)
    return proc.returncode, out


def contract(kind_args):
    r = subprocess.run([sys.executable, '-I', os.path.join(TOOLS, 'contract_check.py')] + kind_args,
                       capture_output=True, text=True)
    lines = (r.stdout + r.stderr).strip().splitlines()
    return r.returncode, lines


def check_contract(res, label, kind_args):
    code, lines = contract(kind_args)
    res.check(code == 0, 'contract_check.py %s: %s' % (label, lines[-1] if lines else 'no output, exit code %d' % code))
    for line in lines[:-1][:10]:
        res.note(line)


# ---------------------------------------------------------------- TST-521

def tst521(ctx):
    res = Result(521)
    ctx.procs.stop('P4')
    ctx.procs.stop('P2')
    ctx.procs.ensure('P3')
    ctx.procs.start('P2')   # 1. start P2 while P4 is not running
    ui = Ui(ctx.record(521))
    try:
        t, m = ui.wait(lambda m: m.get('name') == 'mw_status', 3.0)
        res.check(m is not None and m['data'].get('reachable') is False, '1: mw_status reachable false (%s)'
                  % (m['data'] if m else 'none within 3 s'))
        r, _ = ui.call('get_services')   # 2.
        res.check(r is not None and r.get('ok') is False and r.get('error', {}).get('code') == 'MW_UNREACHABLE',
                  '2: get_services -> MW_UNREACHABLE (%s)' % (r.get('error') if r else 'no response'))

        since = len(ui.messages)
        opened = ctx.procs.start('P4')   # 3. start P4; wait 7 s
        t, m = ui.wait(lambda m: m.get('name') == 'mw_status' and m['data'].get('reachable') is True, 7.0, since)
        delay = (t - opened) if t else None
        if delay is not None:
            res.measured('mw_status true %d ms after P4 opened its port (limit 5 500 ms)' % delay)
        res.check(delay is not None and delay <= 5500, '3: mw_status true at most 5.5 s after P4 opened its port')
        ui.pump(max(0.0, (opened + 7000 - now_ms()) / 1000.0))

        r, _ = ui.call('get_services')   # 4.
        listed = [(e.get('service_id'), e.get('number'), e.get('name'), e.get('protected'))
                  for e in (r or {}).get('result', {}).get('services', [])]
        res.check(listed == SERVICES, '4: get_services -> the 4 services of Section 5.2.3 (%s)' % listed)

        code = ctx.procs.stop('P4')   # 5. stop P4
        res.note('P4 stopped with SIGTERM, exit code %s' % code)
        r, _ = ui.call('get_services')
        listed = [(e.get('service_id'), e.get('number'), e.get('name'), e.get('protected'))
                  for e in (r or {}).get('result', {}).get('services', [])]
        res.check(listed == SERVICES, '5: get_services after the stop of P4 -> the same 4 services (REQ-05)')
        now = int(time.time())
        r, ms = ui.call('get_epg', {'from': now - 7200, 'to': now + 7200})
        res.check(r is not None and r.get('ok') is False and r.get('error', {}).get('code') == 'MW_UNREACHABLE',
                  '5: get_epg -> MW_UNREACHABLE (%s, response after %s ms)' % (r.get('error') if r else 'no response', ms))
    finally:
        ui.close()
    ctx.procs.ensure('P4')
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(521)])
    res.files['recording'] = open(ctx.record(521), encoding='utf-8').read()
    return res


# ---------------------------------------------------------------- TST-522

def keepalive_intervals(lines, headend_session):
    times = [log_time_ms(l) for l in lines if '[SRV-HEC] keepalive session_id=%s' % headend_session in l]
    times = [t for t in times if t is not None]
    return [b - a for a, b in zip(times, times[1:])]


def tst522(ctx):
    res = Result(522)
    ctx.procs.ensure('P4', 'P3', 'P2')
    ui = Ui(ctx.record(522))
    try:
        for service in ('svc-1', 'svc-2'):
            since = len(ui.messages)
            r, ms = ui.call('play', {'service_id': service, 'start': 'live'})
            n = (r or {}).get('result', {}).get('session')
            if not res.check(isinstance(n, int), '%s: response {"session": n} (%s, after %s ms)' % (service, r, ms)):
                continue
            first_event = next((i for i in range(since, len(ui.messages))
                                if ui.messages[i][1] and ui.messages[i][1].get('type') == 'event'
                                and ui.messages[i][1].get('data', {}).get('session') == n), None)
            response_index = next(i for i in range(since, len(ui.messages)) if ui.messages[i][1] is r)
            res.check(first_event is None or first_event > response_index,
                      '%s: the response comes before the first event of session %d' % (service, n))
            t_play, _ = ui.wait_state(n, 'playing', 10.0, since)
            res.check(t_play is not None and ui.states(n) == ['starting', 'waiting_stream', 'playing'],
                      '%s: session_state starting, waiting_stream, playing (%s)' % (service, ui.states(n)))
            hls, out_dir = start_hls_check(ctx, n, 30, png=(2,))

            # stream_info against ffprobe of the content file (HLR-12).
            info = [m['data'] for _, m in ui.events('stream_info', n)]
            probe, raw = ffprobe_json(os.path.join(REPO, CONTENT[service]))
            res.ffprobe += '--- %s ---\n%s\n' % (CONTENT[service], raw)
            prog = (probe.get('programs') or [{}])[0]
            streams = {int(s['id'], 16): s for s in probe.get('streams', []) if 'id' in s}
            video = next((pid for pid, s in streams.items() if s.get('codec_type') == 'video'), None)
            audio = next((pid for pid, s in streams.items() if s.get('codec_type') == 'audio'), None)
            expected = {'program_number': prog.get('program_num'), 'pmt_pid': prog.get('pmt_pid'),
                        'pcr_pid': prog.get('pcr_pid'),
                        'video': {'pid': video, 'stream_type': stream_type(streams.get(video, {}))},
                        'audio': {'pid': audio, 'stream_type': stream_type(streams.get(audio, {}))}}
            got = {}
            if len(info) == 1:
                d = info[0]
                got = {'program_number': d.get('program_number'), 'pmt_pid': d.get('pmt_pid'), 'pcr_pid': d.get('pcr_pid'),
                       'video': {k: (d.get('video') or {}).get(k) for k in ('pid', 'stream_type')},
                       'audio': {k: (d.get('audio') or {}).get(k) for k in ('pid', 'stream_type')}}
            res.check(len(info) == 1 and got == expected, '%s: one stream_info, equal to ffprobe of %s (%s)'
                      % (service, CONTENT[service], got or info))

            ui.pump(30.0)   # run 30 s
            code, out = finish_hls_check(hls, res)
            res.files['hls_check session %d' % n] = out
            summary = [l for l in out.splitlines() if 'playlist loads' in l or l.startswith('directory ')]
            for line in summary:
                res.note(line)
            pid_sets = set(re.findall(r' PIDs ([0-9,]+);', out))   # hls_check.py: "PIDs 0,256,257,4096;"
            res.check(code == 0, '%s: hls_check.py: playlist valid, at most 6 listed, every segment probes and '
                      'decodes, at most 8 segment files (exit code %d)' % (service, code))
            res.check(pid_sets == {'0,256,257,4096'}, '%s: every segment contains PIDs 0 / 4096 / 256 / 257 only (%s)'
                      % (service, sorted(pid_sets)))
            png = os.path.join(out_dir, 'seg-2.png')
            if res.check(os.path.exists(png), '%s: PNG of seg-2.ts written' % service):
                photo = os.path.join(ctx.tmp, 'tst522-%s.png' % service)
                shutil.copyfile(png, photo)
                res.photos.append(photo)
                res.note('PHOTO to inspect: %s must show the burned-in name "%s"'
                         % (photo, dict((s[0], s[2]) for s in SERVICES)[service]))

            # keepalives every 5 s +/- 0.5 s in the head-end log (REQ-67).
            start_ok = [l for l in ctx.logs.lines('P2') if 'start ok session=%d ' % n in l]
            hs = re.search(r'headend_session=(\S+)', start_ok[-1]).group(1) if start_ok else None
            intervals = keepalive_intervals(ctx.logs.lines('P3'), hs) if hs else []
            if intervals:
                res.measured('%s: %d keepalive intervals of %s, %d - %d ms (limit 4 500 - 5 500 ms)'
                             % (service, len(intervals), hs, min(intervals), max(intervals)))
            res.check(len(intervals) >= 4 and all(4500 <= i <= 5500 for i in intervals),
                      '%s: head-end log shows a keepalive every 5 s +/- 0.5 s' % service)
    finally:
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(522)])
    if not res.failures and res.photos:
        res.status = 'passed pending the PHOTO inspection'
    return res


# ---------------------------------------------------------------- helpers of 13b

def previous_event_start(now):
    """Start of the event before the running one (Section 5.5.1): now - (now mod 300) - 300."""
    return now - now % 300 - 300


def play(res, ui, service, start='live', label=None):
    """Sends play; returns the session number (or None) and the index of the first message after it."""
    since = len(ui.messages)
    r, _ = ui.call('play', {'service_id': service, 'start': start})
    n = (r or {}).get('result', {}).get('session')
    res.check(isinstance(n, int), '%s: play %s %s -> {"session": n} (%s)' % (label or service, service, start, r))
    return n, since


def set_key(res, ui, service, key, label):
    r, _ = ui.call('set_key', {'service_id': service, 'key': key})
    res.check(r is not None and r.get('ok') is True, '%s: set_key %s %s… -> ok' % (label, service, key[:4]))


def last_state(ui, session):
    states = ui.events('session_state', session)
    return states[-1][1]['data'] if states else {}


def png_of(ctx, res, hls_out_dir, seq, name, what):
    png = os.path.join(hls_out_dir, 'seg-%d.png' % seq)
    if res.check(os.path.exists(png), 'PNG of seg-%d.ts written' % seq):
        photo = os.path.join(ctx.tmp, name)
        shutil.copyfile(png, photo)
        res.photos.append(photo)
        res.note('PHOTO to inspect: %s must show %s' % (photo, what))


class DirWatch:
    """Lists a directory every 50 ms (TST-525) and keeps every file name it ever saw."""

    def __init__(self, path):
        self.path, self.seen, self.samples, self.running = path, set(), 0, True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while self.running:
            try:
                self.seen.update(os.listdir(self.path))
            except OSError:
                pass
            self.samples += 1
            time.sleep(0.05)

    def stop(self):
        self.running = False
        self.thread.join()


def time_code(t):
    return '%02d:%02d' % ((t % 600) // 60, t % 60)


# ---------------------------------------------------------------- TST-523

def tst523(ctx):
    res = Result(523)
    ctx.procs.stop('P2')
    ctx.procs.restart('P4', TEST_CONFIG)
    ctx.procs.restart('P3', TEST_CONFIG)
    ctx.procs.start('P2')
    ui = Ui(ctx.record(523))
    try:
        n, since = play(res, ui, 'svc-2', label='1')   # 1.
        if n:
            t, _ = ui.wait_state(n, 'playing', 10.0, since)
            hls, _ = start_hls_check(ctx, n, 6)
            info = [m['data'] for _, m in ui.events('stream_info', n)]
            got = info[0] if len(info) == 1 else {}
            ok = (got.get('program_number') == 2 and got.get('pmt_pid') == 768 and got.get('pcr_pid') == 1025
                  and (got.get('video') or {}).get('pid') == 1025 and (got.get('audio') or {}).get('pid') == 1026)
            res.check(ok, '1: stream_info program 2, PMT 768, video 1025, audio 1026 (Section 5.2.1, test set) (%s)'
                      % (info or 'none'))
            res.check(t is not None, '1: playing (%s)' % ui.states(n))
            code, out = finish_hls_check(hls, res)
            res.files['hls_check session %d' % n] = out
            pid_sets = set(re.findall(r' PIDs ([0-9,]+);', out))
            res.check(code == 0 and pid_sets == {'0,768,1025,1026'},
                      '1: segments contain PIDs 0 / 768 / 1025 / 1026 only (%s, hls_check exit code %d)'
                      % (sorted(pid_sets), code))

        n, since = play(res, ui, 'svc-4', label='2')   # 2.
        if n:
            t, _ = ui.wait_state(n, 'key_required', 10.0, since)
            res.check(t is not None, '2: key_required (%s)' % ui.states(n))
            set_key(res, ui, 'svc-4', K4, '2')
            t, _ = ui.wait_state(n, 'playing', 10.0, since)
            res.check(t is not None and last_state(ui, n).get('protected') is True,
                      '2: then playing with protected true (%s)' % ui.states(n))

        n, since = play(res, ui, 'svc-1', label='3')   # 3.
        if n:
            t, m = ui.wait_state(n, 'error', 10.0, since)
            res.check(t is not None and (m['data'].get('error') or {}).get('code') == 'INVALID_STREAM',
                      '3: error INVALID_STREAM (%s)' % (m['data'] if m else ui.states(n)))
    finally:
        ui.close()
        ctx.procs.stop('P2')
        ctx.procs.ensure('P4', 'P3')   # back to the reference configuration
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(523)])
    return res


# ---------------------------------------------------------------- TST-524

def tst524(ctx):
    res = Result(524)
    ctx.procs.ensure('P4', 'P3', 'P2')
    ui = Ui(ctx.record(524))
    try:
        T = previous_event_start(int(time.time()))
        n, since = play(res, ui, 'svc-2', T, label='1')
        if n:
            hls, out_dir = start_hls_check(ctx, n, 8, png=(0,))
            t, m = ui.wait_state(n, 'playing', 10.0, since)
            d = m['data'] if m else {}
            res.check(d.get('start') == T and d.get('start_time') == T,
                      '1: session_state start = start_time = T = %d (%s / %s)' % (T, d.get('start'), d.get('start_time')))
            code, out = finish_hls_check(hls, res)
            res.files['hls_check session %d' % n] = out
            res.check(code == 0, 'hls_check.py: 0 errors (exit code %d)' % code)
            png_of(ctx, res, out_dir, 0, 'tst524-svc-2.png',
                   'the time code %s (T mod 600) +/- 1 s and "Demo Two" (step 2, NFR-04)' % time_code(T))
    finally:
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(524)])
    if not res.failures and res.photos:
        res.status = 'passed pending the PHOTO inspection'
    return res


# ---------------------------------------------------------------- TST-525

def tst525(ctx):
    res = Result(525)
    ctx.procs.ensure('P4', 'P3')
    ctx.procs.restart('P2')   # no key stored
    ui = Ui(ctx.record(525))
    watch = None
    try:
        T = previous_event_start(int(time.time()))
        n, since = play(res, ui, 'svc-3', T, label='1')   # 1.
        if not n:
            return res
        watch = DirWatch(os.path.join(HLS_DIR, str(n)))
        t, m = ui.wait_state(n, 'key_required', 10.0, since)
        d = m['data'] if m else {}
        res.check(d.get('reason') == 'no_key' and d.get('start') == T,
                  '1: key_required no_key, start = T = %d (%s)' % (T, d or ui.states(n)))
        ui.pump(0.5)
        start_ok = [l for l in ctx.logs.lines('P2') if 'start ok session=%d ' % n in l]
        hs1 = re.search(r'headend_session=(\S+)', start_ok[-1]).group(1) if start_ok else None
        res.check(hs1 is not None and any('[SRV-HEC] stop session_id=%s' % hs1 in l for l in ctx.logs.lines('P3')),
                  '1: head-end log shows /stop for %s' % hs1)

        since = len(ui.messages)   # 2.
        set_key(res, ui, 'svc-3', KX, '2')
        t, m = ui.wait(lambda m: m.get('name') == 'session_state' and m['data'].get('session') == n
                       and m['data'].get('state') == 'key_required', 10.0, since)
        d = m['data'] if m else {}
        res.check(d.get('reason') == 'key_rejected' and 'validating_key' in ui.states(n),
                  '2: validating_key, then key_required key_rejected (%s)' % ui.states(n))
        ui.pump(0.3)
        lines = ctx.logs.lines('P2')
        rejected = next((log_time_ms(l) for l in lines if '[AL-API] send event session_state session=%d '
                         'state=key_required reason=key_rejected' % n in l), None)
        locked = [log_time_ms(l) for l in lines if '[AL-SES] tuner Locked session=%d ' % n in l]
        locked = [x for x in locked if rejected is not None and x is not None and x <= rejected]
        if rejected is not None and locked:
            res.measured('key_rejected %d ms after the first Locked of the attempt (limit 3 000 ms, NFR-03)'
                         % (rejected - locked[-1]))
        res.check(rejected is not None and locked and rejected - locked[-1] <= 3000,
                  '2: key_required key_rejected at most 3 s after the first RTP data of the attempt (middleware log)')
        watch.stop()
        segments = sorted(f for f in watch.seen if f.endswith('.ts') or f.endswith('.m3u8'))
        res.check(not segments, '1, 2: no segment file and no playlist at any of %d listings every 50 ms (%s)'
                  % (watch.samples, segments))
        watch = None

        res.note('waiting 60 s (step 3)')   # 3.
        ui.pump(60.0)
        since = len(ui.messages)
        set_key(res, ui, 'svc-3', K3, '3')
        hls, out_dir = start_hls_check(ctx, n, 10, png=(0,))
        t, m = ui.wait_state(n, 'playing', 10.0, since)
        d = m['data'] if m else {}
        res.check(t is not None and d.get('start') == T and d.get('start_time') == T,
                  '3: starting ... playing with start = start_time = T (%s, %s / %s)'
                  % (ui.states(n), d.get('start'), d.get('start_time')))
        code, out = finish_hls_check(hls, res)
        res.files['hls_check session %d' % n] = out
        res.check(code == 0, '3: hls_check.py: 0 errors (exit code %d)' % code)
        png_of(ctx, res, out_dir, 0, 'tst525-svc-3.png',
               'the time code %s (T mod 600) +/- 1 s, not %s (T + 60), and "Premium One"'
               % (time_code(T), time_code(T + 60)))

        n4, since = play(res, ui, 'svc-3', label='4')   # 4.
        t, _ = ui.wait_state(n4, 'playing', 10.0, since)
        res.check(t is not None and 'key_required' not in ui.states(n4),
                  '4: svc-3 live plays without key_required (%s)' % ui.states(n4))

        n5, since = play(res, ui, 'svc-4', label='5')   # 5.
        ui.wait_state(n5, 'key_required', 10.0, since)
        since = len(ui.messages)
        set_key(res, ui, 'svc-4', K3, '5')
        t, m = ui.wait_state(n5, 'key_required', 10.0, since)
        res.check(t is not None and m['data'].get('reason') == 'key_rejected', '5: K3 for svc-4 rejected (%s)'
                  % ui.states(n5))
        since = len(ui.messages)
        set_key(res, ui, 'svc-4', K4, '5')
        t, _ = ui.wait_state(n5, 'playing', 10.0, since)
        res.check(t is not None, '5: K4 for svc-4 accepted, playing (%s)' % ui.states(n5))
    finally:
        if watch:
            watch.stop()
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(525)])
    if not res.failures and res.photos:
        res.status = 'passed pending the PHOTO inspection'
    return res


# ---------------------------------------------------------------- TST-526

def tst526(ctx):
    res = Result(526)
    ctx.procs.ensure('P4', 'P3')
    ctx.procs.restart('P2')
    ui = Ui(ctx.record(526))
    sessions = []
    try:
        for service, delay in [('svc-4', 0.05), ('svc-3', 0.1)] + [('svc-1' if k % 2 == 0 else 'svc-2', 0.1)
                                                                    for k in range(10)]:
            cid = ui.command('play', {'service_id': service, 'start': 'live'})
            sessions.append((service, cid))
            ui.pump(delay)
        res.check(sessions[-1][0] == 'svc-2', 'the last play is svc-2')
        t, r = ui.wait(lambda m: m.get('type') == 'response' and m.get('id') == sessions[-1][1], 2.0)
        last = (r or {}).get('result', {}).get('session')
        t, _ = ui.wait_state(last, 'playing', 10.0)
        ui.pump(1.0)
        res.check(t is not None and last_state(ui, last).get('service_id') == 'svc-2',
                  'the last session %s reaches playing with service_id svc-2 (%s)' % (last, ui.states(last)))
        # After every stopped of a session no further event of it.
        late = []
        for i, (_, m) in enumerate(ui.messages):
            if m and m.get('name') == 'session_state' and m['data'].get('state') == 'stopped':
                s = m['data']['session']
                late += [m2['data'] for _, m2 in ui.messages[i + 1:] if m2 and m2.get('type') == 'event'
                         and m2.get('data', {}).get('session') == s]
        res.check(not late, 'after the stopped of a session no further event of it (%d late)' % len(late))
        numbers = sorted({m['data']['session'] for _, m in ui.messages if m and m.get('name') == 'session_state'})
        res.note('sessions %s; stopped: %s' % (numbers, sorted({m['data']['session'] for _, m in ui.messages if m
                                                               and m.get('name') == 'session_state'
                                                               and m['data'].get('state') == 'stopped'})))
        he = ctx.logs.lines('P3')
        started = [re.search(r'start session_id=(\S+)', l).group(1) for l in he if '[SRV-HEC] start session_id=' in l]
        ended = {re.search(r'(?:stop|session expired) session_id=(\S+)', l).group(1) for l in he
                 if re.search(r'\[SRV-HEC\] (stop|session expired) session_id=', l)}
        alive = [s for s in started if s not in ended]
        res.check(len(alive) == 1, 'the head-end has exactly one session at the end (started %d, ended %d, alive %s)'
                  % (len(started), len(ended), alive))
        dirs = sorted(os.listdir(HLS_DIR)) if os.path.isdir(HLS_DIR) else []
        res.check(dirs == [str(last)], '/tmp/m15-hls contains only the directory of the last session (%s)' % dirs)
        mw = ctx.logs.lines('P2')
        errors = [l for l in mw if ' ERROR ' in l]
        drops = [l for l in mw if re.search(r'discard|dropped', l, re.I) and ' DEBUG ' not in l]
        res.check(not errors, 'middleware log: no ERROR line (%d)' % len(errors))
        res.check(not drops, 'middleware log: discarded / dropped tasks of old generations only at DEBUG (%d other)'
                  % len(drops))
        for line in (errors + drops)[:10]:
            res.note(line)
    finally:
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(526)])
    return res


# ---------------------------------------------------------------- TST-527

def tst527(ctx):
    res = Result(527)
    ctx.procs.ensure('P4', 'P3')
    ctx.procs.restart('P2')
    ui = Ui(ctx.record(527))
    try:
        set_key(res, ui, 'svc-3', KX, 'precondition (KX stored for svc-3)')
        since = len(ui.messages)   # 1.
        cid = ui.command('play', {'service_id': 'svc-3', 'start': 'live'})
        ui.pump(0.02)
        set_key(res, ui, 'svc-3', K3, '1 (20 ms after play)')
        t, r = ui.wait(lambda m: m.get('type') == 'response' and m.get('id') == cid, 2.0, since)
        n1 = (r or {}).get('result', {}).get('session')
        t, _ = ui.wait_state(n1, 'playing', 10.0, since)
        res.check(t is not None and 'key_required' not in ui.states(n1),
                  '1: the session ends in playing, key_rejected never reported (%s)' % ui.states(n1))

        set_key(res, ui, 'svc-3', KX, '2 (KX stored again)')   # 2.
        n2, since = play(res, ui, 'svc-3', label='2')
        t, _ = ui.wait_state(n2, 'validating_key', 10.0, since)
        set_key(res, ui, 'svc-3', K3, '2 (as soon as validating_key is received)')
        t, _ = ui.wait_state(n2, 'playing', 10.0, since)
        res.check(t is not None and 'key_required' not in ui.states(n2),
                  '2: the session ends in playing, key_rejected never reported (%s)' % ui.states(n2))

        n3, since = play(res, ui, 'svc-3', label='K3 stored')
        t, _ = ui.wait_state(n3, 'playing', 10.0, since)
        res.check(t is not None and 'key_required' not in ui.states(n3),
                  'K3 is the stored key afterwards: a new play svc-3 plays without key_required (%s)' % ui.states(n3))
        res.check(not any('key rejected' in l for l in ctx.logs.lines('P2')),
                  'middleware log: no "key rejected" line')
    finally:
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(527)])
    return res

# ---------------------------------------------------------------- helpers of 13c

def log_lines_with(lines, text):
    return [(log_time_ms(l), l) for l in lines if text in l]


def headend_session_of(ctx, session):
    start_ok = [l for l in ctx.logs.lines('P2') if 'start ok session=%d ' % session in l]
    m = re.search(r'headend_session=(\S+)', start_ok[-1]) if start_ok else None
    return m.group(1) if m else None


def play_until_playing(res, ui, service, label, start='live'):
    n, since = play(res, ui, service, start, label=label)
    t, _ = ui.wait_state(n, 'playing', 10.0, since) if n else (None, None)
    res.check(t is not None, '%s: %s playing (%s)' % (label, service, ui.states(n) if n else '-'))
    return n


# ---------------------------------------------------------------- TST-528

def tst528(ctx):
    res = Result(528)
    ctx.procs.ensure('P4', 'P3', 'P2')
    p2 = ctx.procs.procs['P2'].pid
    ui = Ui(ctx.record(528))
    try:
        n = play_until_playing(res, ui, 'svc-1', 'precondition')
        ui.pump(6.0)   # at least one successful keepalive
        since = len(ui.messages)
        t_stop = now_ms()   # 1. the head-end sends until it is stopped: the time of the last datagram
        code = ctx.procs.stop('P3')
        res.note('P3 stopped with SIGTERM, exit code %s' % code)
        t, m = ui.wait_state(n, 'no_signal', 6.0, since)
        if t:
            res.measured('no_signal %d ms after the stop of P3 (limit 3 000 +/- 500 ms)' % (t - t_stop))
        res.check(t is not None and m['data'].get('reason') == 'data_lost' and 2500 <= t - t_stop <= 3500,
                  '1: no_signal data_lost 3 s +/- 0.5 s after the last datagram')
        t_err, m = ui.wait_state(n, 'error', 25.0, since)   # 2. wait (25 s in all)
        ui.pump(max(0.0, (t_stop + 25000 - now_ms()) / 1000.0))
        code_err = (m['data'].get('error') or {}).get('code') if m else None
        hs = headend_session_of(ctx, n)
        oks = [t for t, _ in log_lines_with(ctx.logs.lines('P2'), 'keepalive ok headend_session=%s' % hs) if t]
        sent = log_lines_with(ctx.logs.lines('P2'), '[AL-API] send event session_state session=%d state=error' % n)
        if oks and sent:
            res.measured('error %d ms after the last successful keepalive (limit 15 000 +/- 500 ms)' % (sent[-1][0] - oks[-1]))
        res.check(code_err == 'HEADEND_UNREACHABLE' and oks and sent and 14500 <= sent[-1][0] - oks[-1] <= 15500,
                  '2: error HEADEND_UNREACHABLE at the third failed keepalive, 15 s +/- 0.5 s after the last '
                  'successful one (%s, %s)' % (code_err, ui.states(n)))
        failed = log_lines_with(ctx.logs.lines('P2'), 'keepalive failed (')
        res.note('failed keepalives logged: %d' % len(failed))

        ctx.procs.start('P3')   # 3.
        n2 = play_until_playing(res, ui, 'svc-2', '3')
        res.check(ctx.procs.running('P2') and ctx.procs.procs['P2'].pid == p2, '3: without restarting P2 (pid %d)' % p2)

        # 4. second run: P3 stopped and started again within 5 s.
        ui.pump(6.0)
        hs2 = headend_session_of(ctx, n2)
        since = len(ui.messages)
        t0 = now_ms()
        ctx.procs.stop('P3')
        opened = ctx.procs.start('P3')
        res.measured('P3 restarted in %d ms (limit 5 000 ms)' % (opened - t0))
        t, m = ui.wait_state(n2, 'error', 12.0, since)
        code_err = (m['data'].get('error') or {}).get('code') if m else None
        nf = log_lines_with(ctx.logs.lines('P2'), 'keepalive')
        not_found = [l for _, l in nf if 'SESSION_NOT_FOUND' in l]
        res.check(code_err == 'HEADEND_UNREACHABLE' and not_found and opened - t0 <= 5000,
                  '4: error HEADEND_UNREACHABLE at the first keepalive after the restart (SESSION_NOT_FOUND) (%s)'
                  % ui.states(n2))
        ui.pump(1.0)
        stops = [l for t, l in log_lines_with(ctx.logs.lines('P2'), '[AL-HEC] stop headend_session=%s' % hs2)
                 if t is not None and t >= t0]   # P3 restarted: the same id may exist from an earlier run
        res.check(not stops, '4: no /stop sent for %s (T-28)' % hs2)
    finally:
        ui.close()
    ctx.procs.ensure('P3')
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(528)])
    return res


# ---------------------------------------------------------------- TST-529

def tst529(ctx):
    res = Result(529)
    ctx.procs.ensure('P4', 'P3', 'P2')
    ui = Ui(ctx.record(529))
    try:
        n = play_until_playing(res, ui, 'svc-1', 'precondition')
        hls, out_dir = start_hls_check(ctx, n, 25, after_discontinuity=True)
        ui.pump(4.0)
        since = len(ui.messages)
        ctx.procs.send('P3', signal.SIGSTOP)   # 1.
        t_stop = now_ms()
        ui.pump(5.0)
        ctx.procs.send('P3', signal.SIGCONT)
        t_cont = now_ms()
        res.note('P3 suspended for %d ms' % (t_cont - t_stop))
        t_ns, m = ui.wait_state(n, 'no_signal', 2.0, since)
        res.check(t_ns is not None and m['data'].get('reason') == 'data_lost', '1: no_signal data_lost (%s)' % ui.states(n))
        after = next((i for i, (_, mm) in enumerate(ui.messages) if mm is m), len(ui.messages)) if m else len(ui.messages)
        t_pl, _ = ui.wait_state(n, 'playing', 5.0, after)
        if t_pl:
            res.measured('playing again %d ms after SIGCONT (the first new datagram comes after it; limit 1 000 ms)'
                         % (t_pl - t_cont))
        res.check(t_pl is not None and t_pl - t_cont <= 1000,
                  '1: playing of the same session within 1 s after the first new datagram')
        code, out = finish_hls_check(hls)
        res.files['hls_check session %d' % n] = out
        res.check('error' not in ui.states(n), '1: no error (%s)' % ui.states(n))
        disc = [l for l in out.splitlines() if 'after #EXT-X-DISCONTINUITY' in l]
        res.check(bool(disc), '1: new segments listed, the first one after #EXT-X-DISCONTINUITY (%s)'
                  % (disc[0].split(' duration')[0] if disc else 'none'))
        # Not an expected result of TST-529: what hls_check.py finds besides (IF-INT-02, Section 3.7) is
        # reported as a finding; the segment closed at the forward PCR jump of the head-end skip is tracked
        # by the issue on REQ-24.
        errors = [l for l in out.splitlines() if l.startswith('ERROR')]
        if errors:
            res.note('FINDING outside the expected result (hls_check.py exit code %d, %d error line(s)):' % (code, len(errors)))
            for line in errors[:10]:
                res.note('  ' + line)
        late = [l for l in ctx.logs.lines('P3') if '[SRV-RTP] late by' in l]
        res.check(bool(late), '1: head-end log: skip warning (%s)' % (late[0][24:] if late else 'none'))
        seq = int(re.search(r'seg-(\d+)\.ts', disc[0]).group(1)) if disc else None
        if seq is not None:   # 2. the first video frame of the first new segment
            png_of(ctx, res, out_dir, seq, 'tst529-svc-1.png',
                   'the time code %s (now mod 600 at the resume) +/- 2 s and "Demo One"' % time_code(t_cont // 1000))
    finally:
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(529)])
    if not res.failures and res.photos:
        res.status = 'passed pending the PHOTO inspection'
    return res


# ---------------------------------------------------------------- TST-530

def tst530(ctx):
    res = Result(530)
    ctx.procs.ensure('P4', 'P3', 'P2')
    ui = Ui(ctx.record(530))
    try:   # 1. SIGTERM to P2
        n = play_until_playing(res, ui, 'svc-1', '1')
        hs = headend_session_of(ctx, n)
    finally:
        ui.close()
    t0 = now_ms()
    code = ctx.procs.stop('P2', signal.SIGTERM, timeout=10.0)
    took = now_ms() - t0
    res.measured('P2 exited %d ms after SIGTERM (limit 3 000 ms)' % took)
    res.check(code == 0 and took <= 3000, '1: P2 exits with code 0 within 3 s (exit code %s)' % code)
    time.sleep(0.3)
    stop_lines = [l for l in ctx.logs.lines('P3') if '[SRV-HEC] stop session_id=%s' % hs in l]
    res.check(bool(stop_lines) and 'reason=' not in stop_lines[-1], '1: head-end log /stop for %s (%s)'
              % (hs, stop_lines[-1][24:] if stop_lines else 'none'))

    ctx.procs.start('P2')   # 2. new run: SIGKILL to P2
    ui = Ui(ctx.record(530, 'b'))
    try:
        n = play_until_playing(res, ui, 'svc-1', '2')
        hs = headend_session_of(ctx, n)
        ui.pump(6.0)
    finally:
        ui.close()
    ctx.procs.stop('P2', signal.SIGKILL)
    time.sleep(18.0)
    he = ctx.logs.lines('P3')
    keepalives = [t for t, _ in log_lines_with(he, '[SRV-HEC] keepalive session_id=%s' % hs) if t]
    expired = [t for t, _ in log_lines_with(he, 'session expired session_id=%s' % hs) if t]
    if keepalives and expired:
        res.measured('session expired %d ms after the last keepalive (limit 15 000 - 16 000 ms)'
                     % (expired[0] - keepalives[-1]))
    res.check(keepalives and expired and 15000 <= expired[0] - keepalives[-1] <= 16000,
              '2: head-end log "session expired" for %s 15 - 16 s after the last keepalive' % hs)
    ctx.procs.ensure('P2')
    for part in ('', 'b'):   # one recording per connection
        check_contract(res, '--kind ws (recording of run %s)' % (part or 'a'), ['--kind', 'ws', ctx.record(530, part)])
    return res


# ---------------------------------------------------------------- TST-531

def loop_run(ctx, res, ui, label, start, seconds):
    n, since = play(res, ui, 'svc-1', start, label=label)
    if not n:
        return
    hls, _ = start_hls_check(ctx, n, seconds)
    ui.wait_state(n, 'playing', 10.0, since)
    ui.pump(seconds)
    code, out = finish_hls_check(hls, res)
    res.files['hls_check %s session %d' % (label, n)] = out
    summary = next((l for l in out.splitlines() if 'playlist loads' in l), '')
    res.note('%s: %s' % (label, summary))
    m = re.search(r'(\d+) discontinuities, discontinuity sequence up to (\d+)', summary)
    disc = [l for l in out.splitlines() if 'after #EXT-X-DISCONTINUITY' in l]
    res.check(bool(disc), '%s: #EXT-X-DISCONTINUITY before the first segment after the loop boundary (%s)'
              % (label, disc[0].split(' duration')[0] if disc else 'none'))
    res.check(m is not None and int(m.group(2)) >= 1, '%s: later #EXT-X-DISCONTINUITY-SEQUENCE:1 (%s)'
              % (label, m.group(0) if m else summary))
    res.check(code == 0, '%s: all segments probe and decode (hls_check exit code %d)' % (label, code))
    stats = [l for l in ctx.logs.lines('P2') if '[AL-SES] stats ' in l and ' session=%d ' % n in l + ' ']
    lost = [int(x) for x in re.findall(r'rtp_lost=(\d+)', '\n'.join(stats))]
    res.check(bool(lost) and max(lost) == 0, '%s: middleware statistics rtp_lost = 0 (%d stats lines, %s)'
              % (label, len(stats), lost))


def tst531(ctx):
    res = Result(531)
    ctx.procs.ensure('P4', 'P3', 'P2')
    ui = Ui(ctx.record(531))
    try:
        now = int(time.time())
        wait = (580 - now % 600) % 600
        if wait > 60:   # step 2 first: it fits into the waiting time of step 1
            T = now - ((now - 590) % 600) - (600 if (now - 590) % 600 == 0 else 0)
            loop_run(ctx, res, ui, '2 (T mod 600 = 590)', T, 30)
            now = int(time.time())
            wait = (580 - now % 600) % 600
            res.note('waiting %d s for now mod 600 = 580 (step 1)' % wait)
            ui.pump(wait)
            loop_run(ctx, res, ui, '1 (live at now mod 600 = 580)', 'live', 60)
        else:
            ui.pump(wait)
            loop_run(ctx, res, ui, '1 (live at now mod 600 = 580)', 'live', 60)
            now = int(time.time())
            T = now - ((now - 590) % 600) - (600 if (now - 590) % 600 == 0 else 0)
            loop_run(ctx, res, ui, '2 (T mod 600 = 590)', T, 30)
    finally:
        ui.close()
    check_contract(res, '--kind ws (recording of this test)', ['--kind', 'ws', ctx.record(531)])
    return res

# ---------------------------------------------------------------- TST-533

def tst533(ctx):
    res = Result(533)
    check_contract(res, '--spec-examples (the JSON examples of Section 3)', ['--spec-examples'])
    fakes = os.path.join(REPO, 'middleware', 'core', 'tests', 'fakes', 'responses')
    kinds = {}
    for name in sorted(os.listdir(fakes)):
        kind = ('mw_services' if name == 'mw_services.json' else 'mw_service_info' if name.startswith('mw_service_')
                else 'mw_epg' if name == 'mw_epg.json' else 'he_start' if name == 'he_start.json'
                else 'he_keepalive' if name == 'he_keepalive.json' else 'he_stop' if name == 'he_stop.json'
                else None)
        if name.startswith('error_'):
            # An error body: checked for every interface whose codes of Section 3.10 contain it.
            code = name[len('error_'):-len('.json')].upper()
            for k, codes in (('mw_error', {'INVALID_REQUEST', 'INVALID_RANGE', 'SERVICE_NOT_FOUND'}),
                             ('he_error', {'INVALID_REQUEST', 'OUT_OF_ARCHIVE', 'FUTURE_TIME', 'SERVICE_NOT_FOUND',
                                           'SESSION_NOT_FOUND'})):
                if code in codes:
                    kinds.setdefault(k, []).append(os.path.join(fakes, name))
            continue
        kinds.setdefault(kind, []).append(os.path.join(fakes, name))
    if None in kinds:
        res.check(False, 'fake responses of unknown kind: %s' % kinds[None])
    for kind, files in sorted((k, v) for k, v in kinds.items() if k):
        check_contract(res, '--kind %s on %d response file(s) of the fake HTTP client' % (kind, len(files)),
                       ['--kind', kind] + files)

    # Responses of P3 / P4 recorded now: the requests of TST-501 step 1 and TST-502 steps 1 and 3.
    ctx.procs.ensure('P4', 'P3')
    recorded = {}
    folder = os.path.join(ctx.tmp, 'tst533')
    os.makedirs(folder, exist_ok=True)

    def keep(kind, name, status, text):
        path = os.path.join(folder, name + '.json')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
        recorded.setdefault(kind, []).append(path)
        res.note('recorded %s: HTTP %d' % (name, status))

    mw = 'http://127.0.0.1:%d/api/v1' % MW_PORT
    keep('mw_services', 'services', *http(mw + '/services'))
    for sid, *_ in SERVICES:
        keep('mw_service_info', 'service-' + sid, *http(mw + '/services/' + sid))
    now = int(time.time())
    keep('mw_epg', 'epg', *http(mw + '/epg?from=%d&to=%d' % (now - 7200, now + 7200)))
    he = 'http://127.0.0.1:%d/api/v1/stream' % HE_PORT
    status, text = http(he + '/start', {'service_id': 'svc-1', 'start': 'live', 'client_host': '127.0.0.1',
                                         'client_port': RTP_PORT, 'ssrc': 1180331842})
    keep('he_start', 'start', status, text)
    sid = json.loads(text).get('session_id') if status == 200 else None
    keep('he_keepalive', 'keepalive', *http(he + '/keepalive', {'session_id': sid}))
    keep('he_stop', 'stop', *http(he + '/stop', {'session_id': sid}))
    keep('he_error', 'stop-again', *http(he + '/stop', {'session_id': sid}))   # SESSION_NOT_FOUND
    for kind, files in sorted(recorded.items()):
        check_contract(res, '--kind %s on %d recorded response(s)' % (kind, len(files)), ['--kind', kind] + files)
    return res


def http(url, body=None, timeout=5):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method='POST' if body is not None else 'GET',
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, (e.read() or b'').decode('utf-8', 'replace')


TESTS = {521: tst521, 522: tst522, 523: tst523, 524: tst524, 525: tst525, 526: tst526, 527: tst527, 528: tst528,
         529: tst529, 530: tst530, 531: tst531, 533: tst533}


class Context:
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--tests', default=','.join(map(str, TESTS)))
    parser.add_argument('--build', default=os.path.join(REPO, 'build'))
    parser.add_argument('--evidence', action='store_true')
    args = parser.parse_args()
    ids = [int(x) for x in args.tests.split(',') if x]
    if any(i not in TESTS for i in ids):
        parser.error('tests available: %s' % ','.join(map(str, TESTS)))

    commit = subprocess.run(['git', 'rev-parse', '--short=8', 'HEAD'], cwd=REPO, capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=REPO,
                           capture_output=True, text=True).stdout.strip()
    if args.evidence and dirty:
        print('run_mw: --evidence needs an unmodified working tree (Section 5.1.7)', file=sys.stderr)
        return 2
    for exe in ('dtv-middleware', 'headend', 'mw_server'):
        if not os.path.exists(os.path.join(args.build, 'bin', exe)):
            print('run_mw: %s missing: build first (README)' % os.path.join(args.build, 'bin', exe), file=sys.stderr)
            return 2
    for port, kind in ((MW_PORT, socket.SOCK_STREAM), (HE_PORT, socket.SOCK_STREAM), (UI_PORT, socket.SOCK_STREAM),
                       (RTP_PORT, socket.SOCK_DGRAM)):
        if not port_free(port, kind):
            print('run_mw: port %d is in use: stop the system first (scripts/stop.sh)' % port, file=sys.stderr)
            return 2

    ctx = Context()
    ctx.tmp = tempfile.mkdtemp(prefix='run_mw-')
    ctx.procs = Processes(args.build, ctx.tmp)
    ctx.logs = Logs()
    ctx.record = lambda tst, part='': os.path.join(ctx.tmp, 'tst%d%s-ws.jsonl' % (tst, part))
    os.makedirs(os.path.join(REPO, 'logs'), exist_ok=True)
    results = []
    try:
        for i in ids:
            print('== TST-%d' % i, flush=True)
            ctx.logs.mark()
            try:
                res = TESTS[i](ctx)
            except Exception as e:   # a broken run is reported, never hidden
                res = Result(i)
                res.check(False, 'test aborted: %s: %s' % (type(e).__name__, e))
            res.log = ctx.logs.excerpt()
            if res.status is None:
                res.status = 'passed' if not res.failures else 'failed'
            for line in res.lines:
                print('  ' + line)
            print('  -> %s' % res.status, flush=True)
            results.append(res)
    finally:
        ctx.procs.stop_all()

    if args.evidence:
        date = datetime.date.today().isoformat()
        env = '%s; %s; %s' % (os.uname().sysname, platform_name(), first_line(['ffprobe', '-version']))
        for res in results:
            folder = os.path.join(REPO, 'tests', 'evidence', 'TST-%d' % res.tst)
            os.makedirs(folder, exist_ok=True)
            base = os.path.join(folder, '%s_%s' % (date, commit))
            with open(base + '_testout.txt', 'w', encoding='utf-8') as f:
                f.write('TESTOUT of TST-%d (Section 5.1.6)\nCommand: tests/integration/run_mw.py --tests %d --evidence\n'
                        'Commit: %s  Date: %s  Environment: %s\n\n%s\nStatus: %s\n'
                        % (res.tst, res.tst, commit, date, env, '\n'.join(res.lines), res.status))
                for name, text in res.files.items():
                    f.write('\n--- %s ---\n%s' % (name, text))
            with open(base + '_log.txt', 'w', encoding='utf-8') as f:
                f.write(res.log)
            if res.ffprobe:
                with open(base + '_ffprobe.json', 'w', encoding='utf-8') as f:
                    f.write(res.ffprobe)
            for k, photo in enumerate(res.photos, 1):
                shutil.copyfile(photo, '%s_photo%d.png' % (base, k))
        print('evidence written below tests/evidence/ (commit %s)' % commit)
    print('summary: ' + ', '.join('TST-%d %s' % (r.tst, r.status) for r in results))
    print('temporary files: %s' % ctx.tmp)
    return 0 if all(r.status.startswith('passed') for r in results) else 1


def first_line(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        return '?'


def platform_name():
    try:
        with open('/etc/os-release', encoding='utf-8') as f:
            for line in f:
                if line.startswith('PRETTY_NAME='):
                    return line.split('=', 1)[1].strip().strip('"')
    except OSError:
        pass
    return '?'


if __name__ == '__main__':
    sys.exit(main())

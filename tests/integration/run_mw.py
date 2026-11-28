#!/usr/bin/env python3
"""Runner of the WP-AL integration tests of CFG-MW (Section 5.4.3).

Starts P4 (mw_server), P3 (headend) and P2 (dtv-middleware) with the configurations of
scripts/start.sh (server/config.json, middleware/main/middleware.json), plays the role of the Web
application with the tools of tests/tools/ (the WebSocket client of ws_client.py, hls_check.py,
contract_check.py), checks the expected results of Section 5.4.3 and stops the processes it started
(only these PIDs).

Usage (Python standard library only, NFR-11), from any directory:
  tests/integration/run_mw.py [--tests 521,522,533] [--build build] [--evidence]

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
  TST-533  contract_check.py on the JSON examples of Section 3, the response files of the fake HTTP
           client of WP-AL and the responses of P3 / P4 recorded by this run (the requests of
           TST-501 and TST-502)
A test with a PHOTO check ends with "passed pending the PHOTO inspection", which is not a status of
Section 5.1.5: the executing member checks the PNG, and only then the test is recorded as "passed".

Expected values come from the specification (Sections 3, 5.2.1, 5.2.3, 5.4.3) and from ffprobe,
never from the output of the code under test (Section 5.1.7, rule 5).

Needs: build/bin/dtv-middleware, headend, mw_server; ffmpeg / ffprobe; the content of Section 5.2.1
(with the encrypted files of package_ts); free ports 8080, 8081, 8090 (TCP) and 5004 (UDP). The exit
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
import time
import urllib.error
import urllib.request

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
TOOLS = os.path.join(REPO, 'tests', 'tools')
sys.path.insert(0, TOOLS)
import ws_client  # noqa: E402  (tests/tools/ws_client.py: the RFC 6455 client of Section 5.4.2)

SERVER_CONFIG = os.path.join('server', 'config.json')
MW_CONFIG = os.path.join('middleware', 'main', 'middleware.json')
MW_PORT, HE_PORT, UI_PORT, RTP_PORT = 8080, 8081, 8090, 5004    # Section 2.2.5
HLS_DIR = '/tmp/m15-hls'                                        # Section 4.2.5
LOGS = {'P2': 'logs/middleware.log', 'P3': 'logs/headend.log', 'P4': 'logs/mw_server.log'}  # NFR-13

# Section 5.2.3: service_id, number, name, protected.
SERVICES = [('svc-1', 1, 'Demo One', False), ('svc-2', 2, 'Demo Two', False),
            ('svc-3', 3, 'Premium One', True), ('svc-4', 4, 'Premium Two', True)]
CONTENT = {'svc-1': 'content/svc-1.ts', 'svc-2': 'content/svc-2.ts'}   # Section 5.2.1, reference set
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
        self.build, self.tmp, self.procs = build, tmp, {}

    def start(self, name):
        exe, config, port = self.COMMANDS[name]
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
        for name in names:
            if not self.running(name):
                self.start(name)

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
            self.pump(min(0.05, max(0.0, end - time.time())))

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


def start_hls_check(ctx, session, seconds, png=()):
    out_dir = os.path.join(ctx.tmp, 'hls-%d' % session)
    os.makedirs(out_dir, exist_ok=True)
    cmd = [sys.executable, '-I', os.path.join(TOOLS, 'hls_check.py'), '--session', str(session), '--seconds',
           str(seconds), '--hls-dir', os.path.join(HLS_DIR, str(session)), '--out', out_dir]
    for seq in png:
        cmd += ['--png', str(seq)]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True), out_dir


def finish_hls_check(proc):
    out, _ = proc.communicate()
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
            code, out = finish_hls_check(hls)
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


TESTS = {521: tst521, 522: tst522, 533: tst533}


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
    ctx.record = lambda tst: os.path.join(ctx.tmp, 'tst%d-ws.jsonl' % tst)
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

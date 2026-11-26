#!/usr/bin/env python3
"""Runner of the WP-PAL integration tests of CFG-OP and CFG-RXP (Section 5.4.3).

Starts P3 (headend) and P4 (mw_server) with the configuration of scripts/start.sh
(server/config.json), executes the steps of each test with the tools of tests/tools/ and pal_dump,
checks the expected results of Section 5.4.3 and stops both processes with SIGTERM.

Usage (Python standard library only, NFR-11), from any directory:
  tests/integration/run_pal.py [--tests 501,502,504,511,512] [--config server/config.json]
                               [--build build] [--evidence]

  --evidence  writes TESTOUT, FFPROBE and LOG files to tests/evidence/<TST-id>/ with the date and the
              commit (Section 5.1.6). Refused for a modified working tree or a configuration other than
              server/config.json, because such a run is not evidence (Section 5.1.7).

Tests:
  TST-501  services and EPG against 5.2.3 / 5.2.4 (the contract check needs tests/tools/contract_check.py)
  TST-502  /start svc-1 live, 15 s RTP, /stop, 2 s more; headers, gaps, first datagram, bitrate, ffprobe
  TST-504  expiry without keepalive (20 s) and 60 s with a keepalive every 5 s
  TST-511  pal_dump 10 s with svc-1 live: PAT / PMT, statistics, ffprobe, decode check
  TST-512  pal_dump with K3, KX and no key on svc-3 (needs svc-3.enc.ts from package_ts)
TST-503 needs a person to read the burned-in time code (PHOTO) and is not run here.

Needs: build/bin/headend, build/bin/mw_server, build/bin/pal_dump, ffmpeg / ffprobe, the content of
Section 5.2.1, free ports 8080, 8081 and 5004. The exit code is 0 if every executed test passed.
"""

import argparse
import datetime
import json
import os
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
SSRC = 1180331842
RTP_PORT = 5004
K3 = '3A7F1C9E5B2D8046A1E3C5079B4D6F28'  # Section 5.2.2
KX = '00112233445566778899AABBCCDDEEFF'


def now_ms():
    return int(time.time() * 1000)


class Result:
    def __init__(self, tst):
        self.tst, self.lines, self.failures, self.status, self.files = tst, [], 0, None, {}

    def check(self, ok, text):
        self.lines.append('%s %s' % ('PASS' if ok else 'FAIL', text))
        if not ok:
            self.failures += 1

    def note(self, text):
        self.lines.append('     ' + text)


def http_json(method, url, body=None, timeout=5):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'null')


def wait_port(port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        with socket.socket() as s:
            if s.connect_ex(('127.0.0.1', port)) == 0:
                return True
        time.sleep(0.1)
    return False


def port_free(port, kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # connections in TIME_WAIT do not count
        try:
            s.bind(('127.0.0.1', port))
            return True
        except OSError:
            return False


def parse_kv(text):
    out = {}
    for line in text.splitlines():
        key, sep, value = line.partition('=')
        if sep:
            out[key.strip()] = value.strip().split(' ')[0]
    return out


def ffprobe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_programs', '-show_streams', '-of', 'json', path],
                         capture_output=True, text=True)
    try:
        return json.loads(out.stdout), out.stdout
    except ValueError:
        return {}, out.stdout + out.stderr


def check_probe(res, path, expect_pids=(256, 257)):
    info, raw = ffprobe(path)
    res.files['ffprobe'] = raw
    programs = info.get('programs', [])
    streams = {int(s.get('id', '0'), 16): s for s in info.get('streams', [])}
    res.check(len(programs) == 1, 'ffprobe: one program (%d)' % len(programs))
    if programs:
        res.check(programs[0].get('pmt_pid') == 4096, 'ffprobe: PMT PID 4096 (%s)' % programs[0].get('pmt_pid'))
    v, a = streams.get(expect_pids[0], {}), streams.get(expect_pids[1], {})
    res.check(v.get('codec_name') == 'h264' and v.get('width') == 1280 and v.get('height') == 720,
              'ffprobe: H.264 1280x720 on PID %d' % expect_pids[0])
    res.check(a.get('codec_name') == 'aac', 'ffprobe: AAC on PID %d' % expect_pids[1])


class Session:
    def __init__(self, base):
        self.base = base

    def start(self, service, start='live', ssrc=SSRC, port=RTP_PORT):
        return http_json('POST', self.base + '/start', {'service_id': service, 'start': start,
                                                         'client_host': '127.0.0.1', 'client_port': port, 'ssrc': ssrc})

    def keepalive(self, sid):
        return http_json('POST', self.base + '/keepalive', {'session_id': sid})

    def stop(self, sid):
        return http_json('POST', self.base + '/stop', {'session_id': sid})


def tst501(ctx):
    res = Result(501)
    cfg = json.load(open(os.path.join(REPO, 'content', 'services.json')))
    expected = sorted(cfg['services'], key=lambda s: s['number'])
    status, body = http_json('GET', ctx.mw + '/services')
    listed = body.get('services', []) if isinstance(body, dict) else []
    res.check(status == 200 and [s.get('service_id') for s in listed] == [s['service_id'] for s in expected],
              'GET /services: the services of 5.2.3 in number order')
    for s in expected:
        status, info = http_json('GET', ctx.mw + '/services/' + s['service_id'])
        ok = status == 200 and all(info.get(k) == s[k] for k in ('service_id', 'number', 'name', 'protected'))
        res.check(ok, 'GET /services/%s: number, name, protected of 5.2.3' % s['service_id'])
    t0 = int(time.time())
    status, epg = http_json('GET', ctx.mw + '/epg?from=%d&to=%d' % (t0 - 7200, t0 + 7200))
    t = epg.get('server_time', 0) if isinstance(epg, dict) else 0
    for svc in (epg.get('services', []) if isinstance(epg, dict) else []):
        s = next((x for x in cfg['services'] if x['service_id'] == svc.get('service_id')), None)
        events = svc.get('events', [])
        starts = [e['start'] for e in events]
        expected_count = 48 if t0 % 300 == 0 else 49
        ok = s is not None and len(events) == expected_count and all(b - a == 300 for a, b in zip(starts, starts[1:]))
        ok = ok and all(e['title'] == s['programs'][0 if e['start'] % 600 < 300 else 1]['title'] for e in events)
        ok = ok and all(e['in_archive'] == (t - 7200 <= e['start'] <= t) for e in events)
        res.check(ok, 'EPG %s: %d events (expected %d), titles and in_archive of 5.2.4 at server_time %d'
                  % (svc.get('service_id'), len(events), expected_count, t))
    contract = os.path.join(TOOLS, 'contract_check.py')
    if not os.path.exists(contract):
        res.note('step 2 (contract checker) not executed: tests/tools/contract_check.py is not available')
        res.status = 'not executed' if not res.failures else 'failed'
    return res


def capture(seconds, out=None, ssrc=SSRC):
    args = [sys.executable, '-I', os.path.join(TOOLS, 'rtp_capture.py'), '--port', str(RTP_PORT),
            '--seconds', str(seconds), '--ssrc', str(ssrc)]
    if out:
        args += ['--out', out]
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def tst502(ctx):
    res = Result(502)
    ts = os.path.join(ctx.tmp, 'tst502.ts')
    cap = capture(17.5, ts)
    time.sleep(0.3)
    status, body = ctx.session.start('svc-1')
    res.check(status == 200 and body.get('ssrc') == SSRC and body.get('payload_type') == 33
              and str(body.get('session_id', '')).startswith('hs-'), 'POST /start svc-1 live: response of 3.3.1')
    time.sleep(15)
    status, _ = ctx.session.stop(body.get('session_id'))
    stop_ms = now_ms()
    res.check(status == 200, 'POST /stop: 200')
    out, _ = cap.communicate()
    res.files['rtp_capture'] = out
    kv = parse_kv(out)
    res.check(kv.get('invalid') == '0', 'all RTP headers valid (invalid=%s)' % kv.get('invalid'))
    res.check(kv.get('sequence_gaps') == '0', 'no sequence gap (%s)' % kv.get('sequence_gaps'))
    res.check(kv.get('first_datagram_ts_packets') == '2' and kv.get('first_datagram_pids') == '0,4096',
              'first datagram: 2 TS packets, PAT and PMT (%s on %s)' % (kv.get('first_datagram_ts_packets'),
                                                                     kv.get('first_datagram_pids')))
    rate = float(kv.get('bitrate_last_10s_mbps', '0'))
    res.check(abs(rate - 3.5) <= 0.175, 'bitrate of the last 10 s %.3f Mbit/s (3.5 +/- 5 %%)' % rate)
    late = int(kv.get('last_arrival_ms', '0')) - stop_ms
    res.check(late <= 100, 'last datagram %d ms after the /stop response (<= 100 ms)' % late)
    check_probe(res, ts)
    return res


def tst504(ctx):
    res = Result(504)
    cap = capture(20.5)
    time.sleep(0.3)
    status, body = ctx.session.start('svc-1')
    start_ms = now_ms()
    sid = body.get('session_id')
    out, _ = cap.communicate()
    kv = parse_kv(out)
    after = (int(kv.get('last_arrival_ms', '0')) - start_ms) / 1000.0
    res.check(14.9 <= after <= 16.1, '1: last datagram %.2f s after the /start response (14.9 - 16.1 s)' % after)
    log = open(ctx.headend_log, errors='replace').read() if os.path.exists(ctx.headend_log) else ''
    res.check(('session expired session_id=%s' % sid) in log, '1: head-end log "session expired session_id=%s"' % sid)
    res.files['rtp_capture_1'] = out
    cap = capture(61)
    time.sleep(0.3)
    status, body = ctx.session.start('svc-1')
    start_ms = now_ms()
    sid = body.get('session_id')
    for _ in range(12):
        time.sleep(5)
        ctx.session.keepalive(sid)
    out, _ = cap.communicate()
    ctx.session.stop(sid)
    kv = parse_kv(out)
    span = (int(kv.get('last_arrival_ms', '0')) - start_ms) / 1000.0
    res.check(span >= 60.0 and kv.get('sequence_gaps') == '0',
              '2: datagrams for %.1f s with a keepalive every 5 s, %s gaps' % (span, kv.get('sequence_gaps')))
    res.files['rtp_capture_2'] = out
    return res


def pal_dump(ctx, name, seconds, key=None):
    out_ts = os.path.join(ctx.tmp, name + '.ts')
    args = [ctx.bin('pal_dump'), '--port', str(RTP_PORT), '--ssrc', str(SSRC), '--seconds', str(seconds), '--out', out_ts]
    if key:
        args.append('--key-stdin')
    # The log lines of the platform go to logs/pal_dump.log in the temporary directory.
    p = subprocess.Popen(args, stdin=subprocess.PIPE if key else subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, cwd=ctx.tmp)
    if key:  # read by pal_dump before it tunes
        p.stdin.write(key + '\n')
        p.stdin.close()
    return p, out_ts


def finish(p):
    out = p.stdout.read()
    p.wait()
    return out


def decode_errors(path):
    out = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'null', '-'], capture_output=True, text=True)
    return [l for l in out.stderr.splitlines() if l.strip()]


def tst511(ctx):
    res = Result(511)
    p, ts = pal_dump(ctx, 'tst511', 10)
    time.sleep(0.5)
    status, body = ctx.session.start('svc-1')
    out = finish(p)
    ctx.session.stop(body.get('session_id'))
    res.files['pal_dump'] = out
    lines = out.splitlines()
    res.check(any(l.startswith('PAT ') and 'CRC ok' in l and 'program 1 -> PMT PID 4096' in l for l in lines),
              'PAT section with program 1 -> 4096 printed (CRC ok)')
    res.check(any(l.startswith('PMT ') and 'CRC ok' in l for l in lines), 'PMT section printed (CRC ok)')
    kv = {}
    for l in lines:
        if l.startswith('tuner ') or l.startswith('demux '):
            kv.update(parse_kv(l.split(' ', 1)[1].replace(' ', '\n')))
    for key in ('rtp_invalid', 'rtp_lost', 'sync_errors'):
        res.check(kv.get(key) == '0', '%s = 0 (%s)' % (key, kv.get(key)))
    res.check(kv.get('cc_errors', 'x').isdigit() and int(kv['cc_errors']) <= 2, 'cc_errors <= 2 (%s)' % kv.get('cc_errors'))
    check_probe(res, ts)
    errors = decode_errors(ts)
    res.check(not errors, 'decode check without error (%d error lines)' % len(errors))
    return res


def tst512(ctx):
    res = Result(512)
    enc = os.path.join(REPO, 'content', 'svc-3.enc.ts')
    if not os.path.exists(enc):
        res.note('content missing: run content/generate.sh and package svc-3.enc.ts with package_ts (%s)' % enc)
        res.status = 'not executed'
        return res
    for name, key in (('K3', K3), ('KX', KX), ('no key', None)):
        p, ts = pal_dump(ctx, 'tst512-' + name.replace(' ', ''), 10, key)
        time.sleep(0.5)
        status, body = ctx.session.start('svc-3')
        out = finish(p)
        ctx.session.stop(body.get('session_id'))
        time.sleep(0.3)
        res.files['pal_dump ' + name] = out
        line = next((l for l in out.splitlines() if l.startswith('video PES starts=')), '')
        total = int(line.split('=')[1].split(' ')[0]) if line else -1
        good = int(line.rsplit(':', 1)[1]) if line else -1
        if key == K3:
            res.check(total > 0 and good == total, 'K3: %d of %d video PES starts begin with 00 00 01' % (good, total))
            errors = decode_errors(ts)
            res.check(not errors, 'K3: decode check without error (%d error lines)' % len(errors))
            png = os.path.join(ctx.tmp, 'tst512-first-key-frame.png')
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', ts, '-frames:v', '1', png])
            res.note('PNG of the first key frame for inspection ("Premium One"): %s' % png)
            ctx.photos[512] = png
        else:
            res.check(total > 0 and good == 0, '%s: 0 of %d video PES starts begin with 00 00 01 (%d)' % (name, total, good))
    if not res.failures:
        res.status = 'passed pending the PHOTO inspection'
    return res


TESTS = {501: tst501, 502: tst502, 504: tst504, 511: tst511, 512: tst512}


class Context:
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--tests', default='501,502,504,511,512')
    parser.add_argument('--config', default=os.path.join(REPO, 'server', 'config.json'))
    parser.add_argument('--build', default=os.path.join(REPO, 'build'))
    parser.add_argument('--evidence', action='store_true')
    args = parser.parse_args()
    ids = [int(x) for x in args.tests.split(',') if x]
    if any(i not in TESTS for i in ids):
        parser.error('tests available: %s' % ','.join(map(str, TESTS)))

    reference = os.path.abspath(args.config) == os.path.join(REPO, 'server', 'config.json')
    commit = subprocess.run(['git', 'rev-parse', '--short=8', 'HEAD'], cwd=REPO, capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=REPO,
                           capture_output=True, text=True).stdout.strip()
    if args.evidence and (dirty or not reference):
        print('run_pal: --evidence needs an unmodified working tree and server/config.json (Section 5.1.7)', file=sys.stderr)
        return 2
    if not reference:
        print('NOTE: configuration %s is not the reference configuration: the results are not evidence' % args.config)

    ctx = Context()
    ctx.bin = lambda name: os.path.join(args.build, 'bin', name)
    for name in ('headend', 'mw_server', 'pal_dump'):
        if not os.path.exists(ctx.bin(name)):
            print('run_pal: %s missing: build first (README)' % ctx.bin(name), file=sys.stderr)
            return 2
    config = json.load(open(args.config))
    mw_port, he_port = config.get('mw_port', 8080), config.get('headend_port', 8081)
    for port, kind in ((mw_port, socket.SOCK_STREAM), (he_port, socket.SOCK_STREAM), (RTP_PORT, socket.SOCK_DGRAM)):
        if not port_free(port, kind):
            print('run_pal: port %d is in use: stop the system first (scripts/stop.sh)' % port, file=sys.stderr)
            return 2
    ctx.tmp = tempfile.mkdtemp(prefix='run_pal-')
    log_dir = config.get('log_dir', 'logs')
    log_dir = log_dir if os.path.isabs(log_dir) else os.path.join(REPO, log_dir)
    ctx.headend_log = os.path.join(log_dir, 'headend.log')
    ctx.mw = 'http://127.0.0.1:%d/api/v1' % mw_port
    ctx.session = Session('http://127.0.0.1:%d/api/v1/stream' % he_port)
    ctx.photos = {}

    procs = {}
    for name in ('headend', 'mw_server'):
        out = open(os.path.join(ctx.tmp, name + '.out'), 'w')
        procs[name] = subprocess.Popen([ctx.bin(name), '--config', args.config], cwd=REPO, stdout=out, stderr=out)
    started = wait_port(he_port) and wait_port(mw_port)
    results = []
    try:
        if not started:
            for name, p in procs.items():
                if p.poll() is not None:
                    print('run_pal: %s exited with code %d:' % (name, p.returncode))
                    print(open(os.path.join(ctx.tmp, name + '.out')).read().strip())
            print('run_pal: P3 / P4 did not start; for the reference set the encrypted files of package_ts are needed')
            return 1
        for i in ids:
            print('== TST-%d' % i, flush=True)
            res = TESTS[i](ctx)
            if res.status is None:
                res.status = 'passed' if not res.failures else 'failed'
            for line in res.lines:
                print('  ' + line)
            print('  -> %s' % res.status, flush=True)
            results.append(res)
    finally:
        for name, p in procs.items():
            p.send_signal(signal.SIGTERM)
            try:
                code = p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                code = 'killed'
            print('%s stopped, exit code %s' % (name, code))

    if args.evidence:
        date = datetime.date.today().isoformat()
        for res in results:
            folder = os.path.join(REPO, 'tests', 'evidence', 'TST-%d' % res.tst)
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, '%s_%s_testout.txt' % (date, commit)), 'w') as f:
                f.write('TESTOUT of TST-%d (Section 5.1.6)\nCommand: tests/integration/run_pal.py --tests %d --evidence\n'
                        'Commit: %s  Date: %s\n\n%s\nStatus: %s\n' % (res.tst, res.tst, commit, date, '\n'.join(res.lines), res.status))
                for name, text in res.files.items():
                    if name != 'ffprobe':
                        f.write('\n--- %s ---\n%s' % (name, text))
            if 'ffprobe' in res.files:
                with open(os.path.join(folder, '%s_%s_ffprobe.json' % (date, commit)), 'w') as f:
                    f.write(res.files['ffprobe'])
        print('evidence written below tests/evidence/ (commit %s)' % commit)
    print('summary: ' + ', '.join('TST-%d %s' % (r.tst, r.status) for r in results))
    print('temporary files: %s' % ctx.tmp)
    return 0 if all(r.status.startswith('passed') for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())

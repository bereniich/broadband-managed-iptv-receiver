#!/usr/bin/env python3
"""Mock middleware of WP-APP (SPECIFICATION.md, Section 2.2.7; HANDOVER-WP-APP, Section 6).

Stand-in for P2 during the development of the Web application (CFG-MOCK, TST-541, TST-532):
  * serves the files of webui/ (IF-INT-03) and /hls/<n>/... (IF-INT-02),
  * answers every IF-INT-01 command on ws://127.0.0.1:8090/ws/control with the JSON of
    Section 3.6 (services of 3.2.1 / 5.2.3, EPG with the formula of Section 4.7.7),
  * sends the event sequences of the state machine of Section 4.3 for the scripted cases
    (clear service, protected service without a key, incorrect key, correct key),
  * plays an HLS live stream made with FFmpeg from a clear content file.

Python 3 standard library only (Section 2.2.8). Run from the repository root:

    python3 webui/mock/mock_middleware.py [--content content/svc-1.ts] [--mw-down]

States on request (curl from another terminal):

    curl -X POST http://127.0.0.1:8090/_mock/no_signal      # playing -> no_signal (data_lost)
    curl -X POST http://127.0.0.1:8090/_mock/signal         # no_signal -> playing
    curl -X POST 'http://127.0.0.1:8090/_mock/error?code=HEADEND_UNREACHABLE'
    curl -X POST 'http://127.0.0.1:8090/_mock/mw?reachable=false'   # or true
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HLS_DIR = '/tmp/m15-mock-hls'
WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'   # RFC 6455, Section 1.3
ARCHIVE_WINDOW_S = 7200                             # D-08
CYCLE_S = 600                                       # D-09: 2 programs of 300 s

# Services of Sections 3.2.1 and 5.2.3.
SERVICES = [
    {'service_id': 'svc-1', 'number': 1, 'name': 'Demo One', 'protected': False},
    {'service_id': 'svc-2', 'number': 2, 'name': 'Demo Two', 'protected': False},
    {'service_id': 'svc-3', 'number': 3, 'name': 'Premium One', 'protected': True},
    {'service_id': 'svc-4', 'number': 4, 'name': 'Premium Two', 'protected': True},
]
PROGRAMS = {
    'svc-1': ('News at Nine', 'Weather Today', 'clear demo service'),
    'svc-2': ('Science Hour', 'Nature Walk', 'clear demo service'),
    'svc-3': ('Premium Morning', 'Premium Evening', 'protected demo service'),
    'svc-4': ('Cinema Classic', 'Late Movie', 'protected demo service'),
}
# Correct keys of the protected services (test data of Section 5.2.2).
CORRECT_KEYS = {
    'svc-3': '3A7F1C9E5B2D8046A1E3C5079B4D6F28',
    'svc-4': 'C04E92B7165FD83A2E7B0C9145D6A3F1',
}
HEX = set('0123456789ABCDEFabcdef')

# Delays of the simulated stream start (the real AL needs about the same, Section 4.6).
START_DELAY_S = 0.2       # starting -> waiting_stream (/start response)
PSI_DELAY_S = 0.3         # waiting_stream -> stream_info (PAT/PMT parsed)
DECISION_DELAY_S = 0.2    # stream_info -> protection decision / key decision
PLAYLIST_TIMEOUT_S = 15   # FFmpeg must list the first segment within this time

CONTENT_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.json': 'application/json',
    '.m3u8': 'application/vnd.apple.mpegurl',
    '.ts': 'video/mp2t',
    '.ico': 'image/x-icon',
}


def log(message):
    """NFR-13 style line on standard error."""
    stamp = datetime.datetime.now().isoformat(timespec='milliseconds')
    print(f'{stamp} INFO [MOCK] {message}', file=sys.stderr, flush=True)


def masked(key):
    """A key in a log line: at most its first 4 characters (REQ-55)."""
    return f'{key[:4]}…' if isinstance(key, str) else '?'


def epg_events(service_id, frm, to, server_time):
    """Events of Section 4.7.7 that overlap [frm, to)."""
    first, second, description = PROGRAMS[service_id]
    events = []
    for k in range(frm // CYCLE_S - 1, to // CYCLE_S + 1):
        for offset, title, which in ((0, first, 'First'), (300, second, 'Second')):
            start = k * CYCLE_S + offset
            if start < to and start + 300 > frm:
                events.append({
                    'event_id': f'{service_id}-{start}',
                    'start': start,
                    'duration': 300,
                    'title': title,
                    'description': f'{which} program of the {description}.',
                    'in_archive': server_time - ARCHIVE_WINDOW_S <= start <= server_time,
                })
    return sorted(events, key=lambda e: e['start'])


class WebSocketClient:
    """Server side of one RFC 6455 connection: text frames only, unmasked from the server."""

    def __init__(self, rfile, wfile):
        self.rfile = rfile
        self.wfile = wfile
        self.lock = threading.Lock()
        self.open = True

    def send_text(self, text):
        data = text.encode('utf-8')
        header = bytearray([0x81])
        if len(data) < 126:
            header.append(len(data))
        elif len(data) < 65536:
            header.append(126)
            header += struct.pack('!H', len(data))
        else:
            header.append(127)
            header += struct.pack('!Q', len(data))
        self._write(bytes(header) + data)

    def _write(self, raw):
        with self.lock:
            if not self.open:
                return
            try:
                self.wfile.write(raw)
                self.wfile.flush()
            except OSError:
                self.open = False

    def _read_exact(self, n):
        data = self.rfile.read(n)
        if data is None or len(data) < n:
            raise ConnectionError('closed')
        return data

    def read_message(self):
        """Returns the next text message, or None for a binary message; raises when closed."""
        parts = []
        opcode_first = None
        while True:
            b0, b1 = self._read_exact(2)
            fin, opcode = b0 & 0x80, b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack('!H', self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack('!Q', self._read_exact(8))[0]
            mask = self._read_exact(4) if b1 & 0x80 else b'\0\0\0\0'
            payload = bytes(c ^ mask[i % 4] for i, c in enumerate(self._read_exact(length)))
            if opcode == 0x8:                       # close
                self._write(b'\x88\x00')
                raise ConnectionError('close frame')
            if opcode == 0x9:                       # ping -> pong
                self._write(bytes([0x8A, len(payload)]) + payload if len(payload) < 126 else b'')
                continue
            if opcode == 0xA:                       # pong
                continue
            if opcode in (0x1, 0x2):
                opcode_first = opcode
                parts = [payload]
            else:                                   # continuation
                parts.append(payload)
            if fin:
                data = b''.join(parts)
                return data.decode('utf-8', 'replace') if opcode_first == 0x1 else None


class Stream:
    """FFmpeg HLS output of one session: /tmp/m15-mock-hls/<n>/index.m3u8 (Section 2.2.7)."""

    def __init__(self, content, duration, session, start_point):
        self.dir = os.path.join(HLS_DIR, str(session))
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        # The content position of the requested time on the D-08 timeline.
        t = int(time.time()) if start_point == 'live' else start_point
        offset = t % duration if duration else 0
        self.playlist = os.path.join(self.dir, 'index.m3u8')
        self.process = subprocess.Popen(
            ['ffmpeg', '-loglevel', 'error', '-re', '-stream_loop', '-1', '-ss', str(offset),
             '-i', content, '-c', 'copy', '-f', 'hls', '-hls_time', '1', '-hls_list_size', '6',
             '-hls_flags', 'delete_segments', self.playlist],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        log(f'stream session={session} start={start_point} content offset={offset}')

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


class Mock:
    """IF-INT-01 semantics of the mock: one current session, events of Section 4.3."""

    ACTIVE = ('starting', 'waiting_stream', 'validating_key', 'playing', 'no_signal')

    def __init__(self, content, duration, mw_down):
        self.content = content
        self.duration = duration
        self.lock = threading.RLock()
        self.clients = set()
        self.reachable = not mw_down
        self.services_loaded = not mw_down
        self.keys = {}               # service_id -> key (upper case), in memory only
        self.last_number = 0
        self.generation = 0
        self.session = None          # data of the last session_state (Section 3.6.3)
        self.stream = None
        self.stream_info_sent = False
        self.pmt_parsed = False      # flags of the current attempt (Section 4.2.3)
        self.protection = 'unknown'  # 'unknown', 'clear' or 'protected'
        self.key_accepted = False    # key_result = accepted

    # ------------------------------------------------------------------ clients and events
    def add_client(self, client):
        with self.lock:
            self.clients.add(client)
        client.send_text(json.dumps({'type': 'event', 'name': 'mw_status', 'data': {'reachable': self.reachable}}))

    def remove_client(self, client):
        with self.lock:
            self.clients.discard(client)

    def broadcast(self, name, data):
        text = json.dumps({'type': 'event', 'name': name, 'data': data})
        with self.lock:
            clients = list(self.clients)
        for client in clients:
            client.send_text(text)

    def set_state(self, state, **changes):
        self.session.update(changes)
        self.session['state'] = state
        if state not in ('key_required', 'no_signal'):
            self.session['reason'] = changes.get('reason')
        if state != 'error':
            self.session['error'] = None
        log(f'send event session_state session={self.session["session"]} state={state} '
            f'reason={self.session["reason"]} start={self.session["start"]}')
        self.broadcast('session_state', dict(self.session))

    # ------------------------------------------------------------------ commands
    def handle(self, client, text):
        try:
            message = json.loads(text)
        except ValueError:
            log('invalid JSON ignored')
            return
        if not isinstance(message, dict):
            return
        command_id = message.get('id')
        if not isinstance(command_id, int) or isinstance(command_id, bool):
            log('message without a readable id ignored')
            return
        name = message.get('name')
        params = message.get('params') if isinstance(message.get('params'), dict) else None
        shown = {k: (masked(v) if k == 'key' else v) for k, v in (params or {}).items()}
        log(f'recv command id={command_id} name={name} ' + ' '.join(f'{k}={v}' for k, v in shown.items()))

        def ok(result):
            client.send_text(json.dumps({'type': 'response', 'id': command_id, 'ok': True, 'result': result}))

        def error(code, text_):
            client.send_text(json.dumps({'type': 'response', 'id': command_id, 'ok': False,
                                         'error': {'code': code, 'message': text_}}))

        if message.get('type') != 'command' or params is None:
            return error('INVALID_COMMAND', 'malformed command')
        handler = getattr(self, f'cmd_{name}', None) if isinstance(name, str) else None
        if handler is None:
            return error('INVALID_COMMAND', f'unknown command {name}')
        with self.lock:
            handler(params, ok, error)

    def cmd_get_services(self, params, ok, error):
        if not self.services_loaded:
            return error('MW_UNREACHABLE', 'service list not loaded')
        ok({'services': SERVICES})

    def cmd_get_epg(self, params, ok, error):
        frm, to = params.get('from'), params.get('to')
        if not all(isinstance(v, int) and not isinstance(v, bool) for v in (frm, to)) \
                or frm >= to or to - frm > 86400:
            return error('INVALID_RANGE', 'invalid range')
        if not self.reachable:
            return error('MW_UNREACHABLE', 'OTT MW server not reachable')
        now = int(time.time())
        ok({'server_time': now,
            'services': [{'service_id': s['service_id'], 'events': epg_events(s['service_id'], frm, to, now)}
                         for s in SERVICES]})

    def cmd_get_status(self, params, ok, error):
        ok(dict(self.session) if self.session else {'state': 'idle'})

    def cmd_stop(self, params, ok, error):
        # T-02; Section 3.6.2 of 1.3 (#27): session_state stopped before the response, as for play (T-01).
        if self.session and self.session['state'] in self.ACTIVE + ('key_required', 'error'):
            self.release()
            self.set_state('stopped')
        ok({})

    def cmd_play(self, params, ok, error):
        service_id, start = params.get('service_id'), params.get('start')
        if not isinstance(service_id, str) or (start != 'live' and (not isinstance(start, int) or isinstance(start, bool))):
            return error('INVALID_COMMAND', 'service_id must be a string, start "live" or an integer')
        if service_id not in PROGRAMS:
            return error('SERVICE_NOT_FOUND', f'unknown service {service_id}')
        now = int(time.time())
        if start != 'live':                                  # REQ-41
            start = 'live' if start >= now else max(start, now - ARCHIVE_WINDOW_S + 5)
        if self.session and self.session['state'] in self.ACTIVE + ('key_required', 'error'):
            self.release()
            self.set_state('stopped')                        # T-01
        self.last_number += 1
        self.session = {'session': self.last_number, 'service_id': service_id, 'state': 'starting',
                        'reason': None, 'start': start, 'start_time': None, 'playing_since': None,
                        'protected': None, 'error': None}
        self.stream_info_sent = False
        ok({'session': self.last_number})                    # the response before the first event
        self.attempt()

    def cmd_set_key(self, params, ok, error):
        service_id, key = params.get('service_id'), params.get('key')
        if not isinstance(service_id, str):
            return error('INVALID_COMMAND', 'service_id must be a string')
        if not isinstance(key, str) or len(key) != 32 or not set(key) <= HEX:
            return error('INVALID_KEY_FORMAT', 'key must be 32 hexadecimal characters')
        if service_id not in PROGRAMS:
            return error('SERVICE_NOT_FOUND', f'unknown service {service_id}')
        self.keys[service_id] = key.upper()
        ok({})
        s = self.session
        if not s or s['service_id'] != service_id:
            return                                           # T-25: used at the next PMT of the service
        if s['state'] == 'key_required':
            restart = True                                   # T-24
        else:                                                # T-33 with the guard of 1.3 (#22); otherwise T-25
            restart = (s['state'] in ('starting', 'waiting_stream', 'validating_key')
                       and self.pmt_parsed and self.protection != 'clear' and not self.key_accepted)
        if restart:
            self.release()
            s.update({'start_time': None, 'playing_since': None, 'protected': None})
            self.attempt()                                   # same session, kept start point (D-14)

    # ------------------------------------------------------------------ stream attempt (S1 - S12)
    def attempt(self):
        """Starts a stream attempt of the current session: starting, then the timed steps."""
        self.generation += 1
        g = self.generation
        self.pmt_parsed = False
        self.protection = 'unknown'
        self.key_accepted = False
        self.set_state('starting')
        self.later(START_DELAY_S, g, self.on_start_ok)

    def later(self, delay, g, step):
        def run():
            with self.lock:
                if g == self.generation:                     # latest request wins (rule 2)
                    step(g)
        timer = threading.Timer(delay, run)
        timer.daemon = True
        timer.start()

    def on_start_ok(self, g):
        s = self.session
        start_time = int(time.time()) if s['start'] == 'live' else s['start']
        self.set_state('waiting_stream', start_time=start_time)
        self.later(PSI_DELAY_S, g, self.on_psi)

    def on_psi(self, g):
        s = self.session
        self.pmt_parsed = True                               # the key, if stored, is applied here (T-12)
        if not self.stream_info_sent:                        # REQ-18: once per session
            self.stream_info_sent = True
            self.broadcast('stream_info', {
                'session': s['session'], 'service_id': s['service_id'], 'program_number': 1,
                'pmt_pid': 4096, 'pcr_pid': 256,
                'video': {'pid': 256, 'stream_type': 27, 'codec': 'H.264'},
                'audio': {'pid': 257, 'stream_type': 15, 'codec': 'AAC'}})
        self.later(DECISION_DELAY_S, g, self.on_protection)

    def on_protection(self, g):
        service_id = self.session['service_id']
        if service_id not in CORRECT_KEYS:                   # clear stream (T-19)
            self.protection = 'clear'
            self.session['protected'] = False
            self.start_stream(g)
        elif service_id not in self.keys:                    # T-14
            self.protection = 'protected'
            self.set_state('key_required', reason='no_key', protected=True)
        else:                                                # T-15
            self.protection = 'protected'
            self.set_state('validating_key', protected=True)
            self.later(DECISION_DELAY_S, g, self.on_key_decision)

    def on_key_decision(self, g):
        service_id = self.session['service_id']
        if self.keys[service_id] == CORRECT_KEYS[service_id]:   # T-16
            log(f'key accepted service={service_id} key={masked(self.keys[service_id])}')
            self.key_accepted = True
            self.start_stream(g)
        else:                                                    # T-17
            log(f'key rejected service={service_id} key={masked(self.keys[service_id])}')
            del self.keys[service_id]
            self.set_state('key_required', reason='key_rejected')

    def start_stream(self, g):
        try:
            self.stream = Stream(self.content, self.duration, self.session['session'], self.session['start'])
        except OSError as e:
            log(f'cannot start FFmpeg: {e}')
            return self.fail('RECEIVER_ERROR')
        stream = self.stream
        deadline = time.monotonic() + PLAYLIST_TIMEOUT_S

        def wait_for_playlist():
            while time.monotonic() < deadline:
                if os.path.exists(stream.playlist):
                    with self.lock:
                        if g == self.generation and self.stream is stream:
                            self.set_state('playing', playing_since=int(time.time()))   # T-18
                    return
                if stream.process.poll() is not None:
                    break
                time.sleep(0.1)
            with self.lock:
                if g == self.generation and self.stream is stream:
                    log('FFmpeg wrote no playlist')
                    self.fail('RECEIVER_ERROR')
        threading.Thread(target=wait_for_playlist, daemon=True).start()

    def release(self):
        """RELEASE (Section 4.3.1, rule 4): end the attempt and remove its HLS directory."""
        self.generation += 1
        if self.stream:
            self.stream.stop()
            self.stream = None

    def fail(self, code):
        self.release()
        self.set_state('error', error={'code': code, 'message': 'mock'})

    # ------------------------------------------------------------------ states on request
    def control(self, action, query):
        with self.lock:
            s = self.session
            state = s['state'] if s else 'idle'
            if action == 'no_signal' and state == 'playing':
                self.set_state('no_signal', reason='data_lost')                 # T-22
            elif action == 'signal' and state == 'no_signal':
                self.set_state('playing')                                       # T-23
            elif action == 'error' and state in self.ACTIVE:
                self.fail(query.get('code', ['HEADEND_UNREACHABLE'])[0])
            elif action == 'mw':
                self.reachable = query.get('reachable', ['true'])[0] == 'true'
                if self.reachable:
                    self.services_loaded = True
                log(f'mw_status reachable={self.reachable}')
                self.broadcast('mw_status', {'reachable': self.reachable})
            else:
                return False
            return True

    def shutdown(self):
        with self.lock:
            self.release()


class Handler(BaseHTTPRequestHandler):
    mock = None
    webui = None

    def log_message(self, fmt, *args):
        pass                                                  # requests are not logged

    def do_GET(self):
        path = urlparse(self.path).path
        if path == '/ws/control' and self.headers.get('Upgrade', '').lower() == 'websocket':
            return self.websocket()
        if path.startswith('/hls/'):
            return self.serve(HLS_DIR, path[len('/hls/'):], hls=True)
        return self.serve(self.webui, 'index.html' if path == '/' else path.lstrip('/'))

    def do_POST(self):
        url = urlparse(self.path)
        if url.path.startswith('/_mock/') and self.mock.control(url.path[len('/_mock/'):], parse_qs(url.query)):
            return self.reply(200, b'ok\n', 'text/plain')
        self.reply(409, b'not possible in the current state\n', 'text/plain')

    def reply(self, status, body, content_type, extra=None):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def serve(self, root, relative, hls=False):
        full = os.path.realpath(os.path.join(root, relative))
        if not full.startswith(os.path.realpath(root) + os.sep) or not os.path.isfile(full):
            return self.reply(404, b'not found\n', 'text/plain')
        try:
            with open(full, 'rb') as f:
                body = f.read()
        except OSError:
            return self.reply(404, b'not found\n', 'text/plain')
        ext = os.path.splitext(full)[1]
        extra = {'Cache-Control': 'no-cache'} if hls and ext == '.m3u8' else None   # Section 3.7
        self.reply(200, body, CONTENT_TYPES.get(ext, 'application/octet-stream'), extra)

    def websocket(self):
        key = self.headers.get('Sec-WebSocket-Key', '')
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.wfile.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                          f'Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n').encode())
        self.wfile.flush()
        client = WebSocketClient(self.rfile, self.wfile)
        log('UI client connected')
        self.mock.add_client(client)
        try:
            while True:
                text = client.read_message()
                if text is not None:
                    self.mock.handle(client, text)
                else:
                    log('binary frame ignored')
        except (ConnectionError, OSError):
            pass
        finally:
            client.open = False
            self.mock.remove_client(client)
            self.close_connection = True
            log('UI client disconnected')


def content_duration(path):
    """Duration of the content file in whole seconds, from ffprobe (0 if unknown)."""
    try:
        out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of',
                              'default=noprint_wrappers=1:nokey=1', path],
                             capture_output=True, text=True, timeout=30).stdout.strip()
        return int(float(out))
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return 0


def main():
    parser = argparse.ArgumentParser(description='Mock middleware of WP-APP (CFG-MOCK).')
    parser.add_argument('--port', type=int, default=8090)
    parser.add_argument('--webui', default='webui', help='directory of the Web application')
    parser.add_argument('--content', default='content/svc-1.ts', help='clear TS file played for every service')
    parser.add_argument('--mw-down', action='store_true', help='start with the OTT MW server unreachable')
    args = parser.parse_args()

    if not os.path.isfile(args.content):
        sys.exit(f'content missing: run content/generate.sh ({args.content})')
    if not os.path.isfile(os.path.join(args.webui, 'index.html')):
        sys.exit(f'Web application files missing: {args.webui}/index.html')
    if shutil.which('ffmpeg') is None:
        sys.exit('ffmpeg missing')

    shutil.rmtree(HLS_DIR, ignore_errors=True)
    os.makedirs(HLS_DIR)
    duration = content_duration(args.content)
    Handler.mock = Mock(args.content, duration, args.mw_down)
    Handler.webui = args.webui
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.daemon_threads = True
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    log(f'listening on http://127.0.0.1:{args.port}/ content={args.content} duration={duration} s')
    try:
        server.serve_forever()
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        Handler.mock.shutdown()
        shutil.rmtree(HLS_DIR, ignore_errors=True)
        log('stopped')


if __name__ == '__main__':
    main()

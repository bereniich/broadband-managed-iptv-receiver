#!/usr/bin/env python3
"""Head-end stub: replaces the head-end P3 (Section 5.4.2; "no data after the start", US-05 AC2).

Answers POST /api/v1/stream/start, /keepalive and /stop as in Section 3.3, but sends no RTP.
Sessions are numbered hs-000001, hs-000002, ...; a new /start for the same client_host:client_port
replaces the old session; a session without /start or /keepalive for 15 s is forgotten (as the
head-end's watchdog does). Listens on 127.0.0.1 only.

Usage (Python standard library only, NFR-11):
  headend_stub.py [--port 8081] [--services svc-1,svc-2,svc-3,svc-4]
Every request is printed on standard output with its status, as an NFR-13 line
(`2026-12-03T10:15:42.317 INFO [HE-STUB] ...`), so that its time can be compared with the logs of the
system (TST-608).
"""

import argparse
import datetime
import http.server
import json
import sys
import threading
import time

EXPIRY_S = 15
ARCHIVE_WINDOW_S = 7200


def log_line(module, message):
    """One line in the format of NFR-13: <ISO local time with ms> INFO [<module>] <message>."""
    now = datetime.datetime.now()
    print('%s.%03d INFO [%s] %s' % (now.strftime('%Y-%m-%dT%H:%M:%S'), now.microsecond // 1000, module, message),
          flush=True)


def make_handler(services):
    lock = threading.Lock()
    state = {'next': 1, 'sessions': {}}  # session_id -> (endpoint, last contact)

    def expire(now):
        for sid, (_, last) in list(state['sessions'].items()):
            if now - last > EXPIRY_S:
                del state['sessions'][sid]

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, fmt, *args):  # one NFR-13 line per request
            log_line('HE-STUB', fmt % args)

        def send(self, status, obj):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def error(self, status, code, message):
            self.send(status, {'error': {'code': code, 'message': message}})

        def do_POST(self):
            length = int(self.headers.get('Content-Length') or 0)
            raw = self.rfile.read(length) if length else b''
            try:
                body = json.loads(raw)
                if not isinstance(body, dict):
                    raise ValueError
            except ValueError:
                return self.error(400, 'INVALID_REQUEST', 'body is not a JSON object')
            now = time.time()
            with lock:
                expire(now)
                if self.path == '/api/v1/stream/start':
                    return self.start(body, now)
                if self.path in ('/api/v1/stream/keepalive', '/api/v1/stream/stop'):
                    sid = body.get('session_id')
                    if not isinstance(sid, str):
                        return self.error(400, 'INVALID_REQUEST', 'session_id missing')
                    if sid not in state['sessions']:
                        return self.error(404, 'SESSION_NOT_FOUND', 'unknown session')
                    if self.path.endswith('/stop'):
                        del state['sessions'][sid]
                        return self.send(200, {'stopped': True})
                    state['sessions'][sid] = (state['sessions'][sid][0], now)
                    return self.send(200, {'expires_in_s': EXPIRY_S})
            return self.error(404, 'INVALID_REQUEST', 'unknown path')

        def do_other(self):
            length = int(self.headers.get('Content-Length') or 0)
            if length:
                self.rfile.read(length)
            self.error(400, 'INVALID_REQUEST', 'method not allowed, only POST')

        do_GET = do_PUT = do_DELETE = do_PATCH = do_other

        def start(self, body, now):
            ok = isinstance(body.get('service_id'), str) and body['service_id'] != ''
            start = body.get('start')
            ok = ok and (start == 'live' or (isinstance(start, int) and not isinstance(start, bool)))
            ok = ok and isinstance(body.get('client_host'), str)
            port, ssrc = body.get('client_port'), body.get('ssrc')
            ok = ok and isinstance(port, int) and not isinstance(port, bool) and 1 <= port <= 65535
            ok = ok and isinstance(ssrc, int) and not isinstance(ssrc, bool) and 0 <= ssrc <= 0xFFFFFFFF
            if not ok:
                return self.error(400, 'INVALID_REQUEST', 'missing or malformed field')
            if body['service_id'] not in services:
                return self.error(404, 'SERVICE_NOT_FOUND', 'unknown service')
            now_s = int(now)  # whole seconds, as T
            if start != 'live' and start < now_s - ARCHIVE_WINDOW_S:
                return self.error(400, 'OUT_OF_ARCHIVE', 'before the archive window')
            if start != 'live' and start >= now_s:
                return self.error(400, 'FUTURE_TIME', 'use "live"')
            endpoint = '%s:%d' % (body['client_host'], port)
            for sid, (ep, _) in list(state['sessions'].items()):
                if ep == endpoint:
                    del state['sessions'][sid]  # replaced by the new session
            sid = 'hs-%06d' % state['next']
            state['next'] += 1
            state['sessions'][sid] = (endpoint, now)
            self.send(200, {'session_id': sid, 'service_id': body['service_id'],
                            'start_time': int(now) if start == 'live' else start,
                            'ssrc': ssrc, 'payload_type': 33})
            log_line('HE-STUB', 'start accepted session_id=%s service=%s client=%s ssrc=%d (no RTP is sent)'
                     % (sid, body['service_id'], endpoint, ssrc))
            return None

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--services', default='svc-1,svc-2,svc-3,svc-4')
    args = parser.parse_args()
    services = set(s for s in args.services.split(',') if s)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(services))
    print('headend_stub listening on http://127.0.0.1:%d/api/v1/stream (no RTP)' % server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())

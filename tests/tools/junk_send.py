#!/usr/bin/env python3
"""JSON / random-byte sender for the invalid inputs IN-01 - IN-04 and IN-07 (Section 5.2.5).

Sends every input of the chosen interface, checks the expected handling of Section 5.2.5 and that
the server still answers afterwards (REQ-58).

Usage (Python standard library only, NFR-11):
  junk_send.py --target mw      [--port 8080]   IN-01 and IN-04 against the OTT MW server
  junk_send.py --target headend [--port 8081]   IN-01, IN-02 and IN-03 against the head-end
  junk_send.py --target ws      [--port 8090]   IN-07 against the control WebSocket of the middleware

One line per input: PASS or FAIL, the input, the answer. The exit code is 1 if an input got an
unexpected answer or the server stopped answering, otherwise 0.
"""

import argparse
import base64
import http.client
import json
import os
import random
import socket
import struct
import sys
import time

RESULTS = []


def report(ok, name, detail):
    RESULTS.append(ok)
    print('%s %s: %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def http_call(port, method, path, body=None, content_type='application/json'):
    """Returns (status, parsed JSON or None, raw text); status 0 if there was no answer."""
    try:
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        headers = {'Content-Type': content_type} if body is not None else {}
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        conn.close()
    except (OSError, http.client.HTTPException) as e:
        return 0, None, 'no answer (%s)' % e
    text = raw.decode('utf-8', 'replace')
    try:
        return resp.status, json.loads(text), text
    except ValueError:
        return resp.status, None, text


def expect_error(port, name, method, path, body, statuses, code=None):
    status, obj, text = http_call(port, method, path, body)
    err = obj.get('error') if isinstance(obj, dict) else None
    has_object = isinstance(err, dict) and isinstance(err.get('code'), str) and isinstance(err.get('message'), str)
    ok = status in statuses and has_object and (code is None or err.get('code') == code)
    report(ok, name, '%d %s' % (status, text.strip()[:120]))


def in01(port, paths):
    rng = random.Random(1)
    post_path, get_path = paths
    expect_error(port, 'IN-01 truncated JSON body', 'POST', post_path, b'{"service_id": ', (400, 404))
    expect_error(port, 'IN-01 4 096 random bytes', 'POST', post_path, bytes(rng.getrandbits(8) for _ in range(4096)),
                 (400, 404))
    expect_error(port, 'IN-01 unknown path /api/v1/xyz', 'GET', '/api/v1/xyz', None, (400, 404))
    expect_error(port, 'IN-01 method DELETE', 'DELETE', get_path, None, (400, 404))


def target_mw(port):
    in01(port, ('/api/v1/services', '/api/v1/services'))
    now = int(time.time())
    epg = '/api/v1/epg?service_id=svc-1'
    expect_error(port, 'IN-04 /epg without to', 'GET', '%s&from=%d' % (epg, now), None, (400,), 'INVALID_RANGE')
    expect_error(port, 'IN-04 from = to', 'GET', '%s&from=%d&to=%d' % (epg, now, now), None, (400,), 'INVALID_RANGE')
    expect_error(port, 'IN-04 to - from = 86 401', 'GET', '%s&from=%d&to=%d' % (epg, now, now + 86401), None, (400,),
                 'INVALID_RANGE')
    expect_error(port, 'IN-04 from = abc', 'GET', '%s&from=abc&to=%d' % (epg, now), None, (400,), 'INVALID_RANGE')
    expect_error(port, 'IN-04 service_id = svc-9', 'GET', '/api/v1/epg?service_id=svc-9&from=%d&to=%d' % (now, now + 60),
                 None, (404,), 'SERVICE_NOT_FOUND')
    status, obj, _ = http_call(port, 'GET', '/api/v1/services')
    report(status == 200 and isinstance(obj, dict), 'server still answers', 'GET /api/v1/services -> %d' % status)


def target_headend(port):
    start = '/api/v1/stream/start'
    in01(port, (start, start))
    valid = {'service_id': 'svc-1', 'start': 'live', 'client_host': '127.0.0.1', 'client_port': 5999, 'ssrc': 1}
    def variant(**changes):
        body = dict(valid)
        for key, value in changes.items():
            if value is None:
                body.pop(key)
            else:
                body[key] = value
        return json.dumps(body).encode()
    for name, body in [('without ssrc', variant(ssrc=None)), ('ssrc = -1', variant(ssrc=-1)),
                       ('ssrc = 4294967296', variant(ssrc=4294967296)), ('ssrc = "abc"', variant(ssrc='abc')),
                       ('start = "yesterday"', variant(start='yesterday')),
                       ('client_port = 70000', variant(client_port=70000))]:
        expect_error(port, 'IN-02 ' + name, 'POST', start, body, (400,), 'INVALID_REQUEST')
    now = int(time.time())
    expect_error(port, 'IN-03 T = now - 7201', 'POST', start, variant(start=now - 7201), (400,), 'OUT_OF_ARCHIVE')
    expect_error(port, 'IN-03 T = now', 'POST', start, variant(start=now), (400,), 'FUTURE_TIME')
    expect_error(port, 'IN-03 service_id = svc-9', 'POST', start, variant(service_id='svc-9'), (404,),
                 'SERVICE_NOT_FOUND')
    status, _, text = http_call(port, 'POST', '/api/v1/stream/keepalive', json.dumps({'session_id': 'hs-999999'}).encode())
    report(status == 404, 'server still answers', 'POST /keepalive hs-999999 -> %d %s' % (status, text.strip()[:60]))


class WebSocket:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(('GET /ws/control HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n'
                           'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % key).encode())
        self.buffer = b''
        while b'\r\n\r\n' not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError('closed during the handshake')
            self.buffer += chunk
        head, self.buffer = self.buffer.split(b'\r\n\r\n', 1)
        if b' 101 ' not in head.split(b'\r\n')[0]:
            raise OSError('no WebSocket upgrade: %r' % head.split(b'\r\n')[0])

    def send(self, payload, opcode=1):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('!H', n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def receive(self, timeout):
        """Text messages received within `timeout` seconds."""
        messages = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.sock.settimeout(max(0.01, end - time.monotonic()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buffer += chunk
            while len(self.buffer) >= 2:
                n = self.buffer[1] & 0x7F
                offset = 2
                if n == 126:
                    if len(self.buffer) < 4:
                        break
                    n = struct.unpack('!H', self.buffer[2:4])[0]
                    offset = 4
                elif n == 127:
                    if len(self.buffer) < 10:
                        break
                    n = struct.unpack('!Q', self.buffer[2:10])[0]
                    offset = 10
                if len(self.buffer) < offset + n:
                    break
                if self.buffer[0] & 0x0F == 1:
                    messages.append(self.buffer[offset:offset + n].decode('utf-8', 'replace'))
                self.buffer = self.buffer[offset + n:]
        return messages


def responses(messages):
    out = {}
    for m in messages:
        try:
            obj = json.loads(m)
        except ValueError:
            continue
        if isinstance(obj, dict) and obj.get('type') == 'response':
            out[obj.get('id')] = obj
    return out


def target_ws(port):
    try:
        ws = WebSocket(port)
    except OSError as e:
        report(False, 'WebSocket connection', str(e))
        return
    ws.send(b'not json')
    ws.send(json.dumps({'type': 'command', 'id': 5, 'name': 'fly', 'params': {}}).encode())
    ws.send(json.dumps({'type': 'command', 'id': 6, 'name': 'play', 'params': {'start': 'live'}}).encode())
    ws.send(json.dumps({'type': 'command', 'id': 7, 'name': 'play', 'params': {'service_id': 'svc-1', 'start': -5.5}}).encode())
    ws.send(random.Random(7).randbytes(1000), opcode=2)
    ws.send(json.dumps({'type': 'command', 'id': 8, 'name': 'get_status', 'params': {}}).encode())
    got = responses(ws.receive(2.0))
    for rid, name in [(5, 'unknown command "fly"'), (6, 'play without service_id'), (7, 'play with start = -5.5')]:
        r = got.get(rid)
        ok = isinstance(r, dict) and r.get('ok') is False and isinstance(r.get('error'), dict) \
            and r['error'].get('code') == 'INVALID_COMMAND'
        report(ok, 'IN-07 ' + name, json.dumps(r)[:120] if r else 'no response')
    extra = sorted(k for k in got if k not in (5, 6, 7, 8))
    report(not extra, 'IN-07 "not json" and the binary frame ignored', 'responses to other ids: %s' % (extra or 'none'))
    r = got.get(8)
    report(isinstance(r, dict) and r.get('ok') is True, 'middleware still answers', 'get_status -> %s' % (json.dumps(r)[:80] if r else 'no response'))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--target', required=True, choices=['mw', 'headend', 'ws'])
    parser.add_argument('--port', type=int)
    args = parser.parse_args()
    port = args.port or {'mw': 8080, 'headend': 8081, 'ws': 8090}[args.target]
    {'mw': target_mw, 'headend': target_headend, 'ws': target_ws}[args.target](port)
    print('%d of %d inputs handled as expected' % (sum(RESULTS), len(RESULTS)))
    return 0 if all(RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())

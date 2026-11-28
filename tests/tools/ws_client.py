#!/usr/bin/env python3
"""WebSocket test client for IF-INT-01 (Section 5.4.2).

A minimal RFC 6455 client over a TCP socket: runs a script of commands with delays, records every
received message with its arrival time in milliseconds and checks the expected sequence of events
given by the test.

Usage (Python standard library only, NFR-11):
  ws_client.py --script FILE [--url ws://127.0.0.1:8090/ws/control] [--record FILE]
               [--linger-ms 1000] [--quiet]

The script is a JSON array of steps, executed in order; messages are received during every step:
  {"command": "<name>", "params": {...}, "as": "<label>"}   send a command (Section 3.6.1); the id
                                                           is assigned by the client (1, 2, ...,
                                                           skipping the ids of "raw" steps)
  {"raw": "<text>"}                                        send a text frame as it is (IN-07)
  {"binary_random": <n>}                                   send a binary frame of n random bytes
  {"sleep_ms": <n>}                                        wait n ms
  {"expect": <pattern>, "within_ms": <n>, "optional": false}   wait for a message (default 5000 ms)
  {"expect_all": [<pattern>, ...], "within_ms": <n>}       wait for all of them, in any order (for
                                                           messages whose order is not specified, e.g.
                                                           the response to stop and session_state stopped)

A pattern is matched against a received message: every key of the pattern must be present with a
matching value; an object matches recursively, an array means "one of these values", the string
"$<label>" stands for the session number of the response to the command named <label>, and the
key "response_to": "<label>" matches the response to that command. Patterns are matched in order,
each after the message that matched the previous one; other messages are skipped. For a
`session_state` pattern that names its session, every other `session_state` of that session in
between is a failure, so the sequence of states of a session is checked exactly. An optional
pattern is skipped when the next `session_state` of its session does not match it, or at the end
of its time.

The recording (--record) has one JSON object per frame: {"t_ms": <UNIX time in ms>, "dir": "sent" |
"recv", "kind": "text" | "binary", "text": "<frame text, base64 for binary>"}; it is the input of
`contract_check.py --kind ws`. The key of a sent `set_key` is recorded and printed masked
("3A7F…", REQ-55), so that no file contains a test key (Section 5.2.2, NFR-08).

The exit code is 0 if every expectation was met, 1 if one failed (the script stops there; messages
are still received for --linger-ms), 2 for a usage, script or connection error.
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.parse

GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'   # RFC 6455, Section 1.3
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


def now_ms():
    return int(time.time() * 1000)


def accept_key(key):
    """Sec-WebSocket-Accept for a Sec-WebSocket-Key (RFC 6455, Section 4.2.2)."""
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


def encode_frame(opcode, payload, mask):
    """One final frame; `mask` is 4 bytes (client to server) or None (RFC 6455, Section 5.2)."""
    n = len(payload)
    mask_bit = 0x80 if mask is not None else 0
    if n < 126:
        head = struct.pack('!BB', 0x80 | opcode, mask_bit | n)
    elif n < 65536:
        head = struct.pack('!BBH', 0x80 | opcode, mask_bit | 126, n)
    else:
        head = struct.pack('!BBQ', 0x80 | opcode, mask_bit | 127, n)
    if mask is None:
        return head + payload
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


class ProtocolError(Exception):
    pass


class FrameParser:
    """Turns received bytes into messages: (opcode, payload); fragments are joined (Section 5.4)."""

    def __init__(self):
        self.buffer = b''
        self.fragments = None   # (opcode, [payloads]) of a fragmented message

    def feed(self, data):
        self.buffer += data
        out = []
        while True:
            frame = self._frame()
            if frame is None:
                return out
            fin, opcode, payload = frame
            if opcode >= 0x8:                       # control frames may come between fragments
                if opcode not in (OP_CLOSE, OP_PING, OP_PONG):
                    raise ProtocolError('reserved opcode %d' % opcode)   # Section 5.2: fail the connection
                if not fin or len(payload) > 125:
                    raise ProtocolError('invalid control frame')
                out.append((opcode, payload))
            elif opcode == OP_CONT:
                if self.fragments is None:
                    raise ProtocolError('continuation frame without a message')
                self.fragments[1].append(payload)
                if fin:
                    out.append(self._message(self.fragments[0], b''.join(self.fragments[1])))
                    self.fragments = None
            elif opcode in (OP_TEXT, OP_BINARY):
                if self.fragments is not None:
                    raise ProtocolError('new message inside a fragmented one')
                if fin:
                    out.append(self._message(opcode, payload))
                else:
                    self.fragments = (opcode, [payload])
            else:
                raise ProtocolError('reserved opcode %d' % opcode)

    @staticmethod
    def _message(opcode, payload):
        if opcode == OP_TEXT:
            try:
                payload.decode('utf-8')
            except UnicodeDecodeError:
                raise ProtocolError('text message is not valid UTF-8') from None   # Section 8.1
        return opcode, payload

    def _frame(self):
        b = self.buffer
        if len(b) < 2:
            return None
        if b[0] & 0x70:
            raise ProtocolError('reserved bits set')
        if b[1] & 0x80:
            raise ProtocolError('masked frame from the server')   # Section 5.1
        n, offset = b[1] & 0x7F, 2
        if n == 126:
            if len(b) < 4:
                return None
            n, offset = struct.unpack('!H', b[2:4])[0], 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, offset = struct.unpack('!Q', b[2:10])[0], 10
        if len(b) < offset + n:
            return None
        self.buffer = b[offset + n:]
        return bool(b[0] & 0x80), b[0] & 0x0F, b[offset:offset + n]


class Connection:
    def __init__(self, url, timeout=5.0):
        u = urllib.parse.urlsplit(url)
        if u.scheme != 'ws' or not u.hostname:
            raise ValueError('not a ws:// URL: %s' % url)
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = u.path or '/'
        if u.query:
            path += '?' + u.query
        self.sock.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n'
                           % (path, u.hostname, u.port or 80, key)).encode())
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError('connection closed during the handshake')
            data += chunk
        head, rest = data.split(b'\r\n\r\n', 1)
        lines = head.decode('iso-8859-1').split('\r\n')
        if len(lines[0].split()) < 2 or lines[0].split()[1] != '101':
            raise ConnectionError('no WebSocket upgrade: %s' % lines[0])
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(':')
            headers[name.strip().lower()] = value.strip()
        if headers.get('upgrade', '').lower() != 'websocket':
            raise ConnectionError('handshake without "Upgrade: websocket"')
        if headers.get('sec-websocket-accept') != accept_key(key):
            raise ConnectionError('wrong Sec-WebSocket-Accept')
        self.parser = FrameParser()
        self.pending = self.parser.feed(rest)
        self.closed = False

    def send(self, opcode, payload):
        self.sock.sendall(encode_frame(opcode, payload, os.urandom(4)))

    def receive(self, timeout_s):
        """Data messages (opcode, payload) received within `timeout_s`; answers ping and close."""
        out = []
        end = time.monotonic() + timeout_s
        while True:
            for opcode, payload in self.pending:
                if opcode == OP_PING:
                    self.send(OP_PONG, payload)
                elif opcode == OP_CLOSE:
                    if not self.closed:
                        self.send(OP_CLOSE, payload[:2])
                    self.closed = True
                elif opcode != OP_PONG:
                    out.append((opcode, payload))
            self.pending = []
            left = end - time.monotonic()
            if out or self.closed or left <= 0:
                return out
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return out
            if not chunk:
                self.closed = True
                return out
            self.pending = self.parser.feed(chunk)

    def close(self):
        try:
            if not self.closed:
                self.send(OP_CLOSE, struct.pack('!H', 1000))
                self.receive(0.5)
        except OSError:
            pass
        self.sock.close()


def masked(text):
    """The text of a sent frame with the key of a set_key command masked (REQ-55)."""
    try:
        m = json.loads(text)
    except ValueError:
        return text
    if isinstance(m, dict) and isinstance(m.get('params'), dict) and isinstance(m['params'].get('key'), str):
        m['params']['key'] = m['params']['key'][:4] + '…'
        return json.dumps(m, ensure_ascii=False)
    return text


def matches(pattern, value, labels):
    """`labels` maps a label to {"id": command id, "session": session number or None}."""
    if isinstance(pattern, dict):
        if not isinstance(value, dict):
            return False
        for key, p in pattern.items():
            if key == 'response_to':
                label = labels.get(p)
                if label is None or value.get('type') != 'response' or value.get('id') != label['id']:
                    return False
            elif key not in value or not matches(p, value[key], labels):
                return False
        return True
    if isinstance(pattern, list):
        return any(matches(p, value, labels) for p in pattern)
    if isinstance(pattern, str) and pattern.startswith('$'):
        label = labels.get(pattern[1:])
        return label is not None and label['session'] is not None and value == label['session']
    return type(pattern) is type(value) and pattern == value


def pattern_session(pattern, labels):
    """(True, session) if the pattern is a session_state pattern that names its session."""
    if not isinstance(pattern, dict) or pattern.get('name') != 'session_state':
        return False, None
    s = pattern.get('data', {}).get('session') if isinstance(pattern.get('data'), dict) else None
    if isinstance(s, str) and s.startswith('$'):
        label = labels.get(s[1:])
        s = label['session'] if label else None
    return isinstance(s, int), s


class Runner:
    def __init__(self, conn, record, quiet):
        self.conn = conn
        self.record = record
        self.quiet = quiet
        self.received = []      # parsed messages (None for frames that are not JSON objects)
        self.cursor = 0         # index after the message that matched the last expectation
        self.labels = {}
        self.next_id = 1
        self.raw_ids = set()    # ids used by raw frames of the script; not assigned to commands

    def log(self, direction, kind, text):
        line = {'t_ms': now_ms(), 'dir': direction, 'kind': kind, 'text': text}
        if self.record:
            self.record.write(json.dumps(line, ensure_ascii=False) + '\n')
            self.record.flush()
        if not self.quiet:
            shown = text if kind == 'text' else '<binary %d bytes>' % len(base64.b64decode(text))
            print('%d %s %s' % (line['t_ms'], direction, shown), flush=True)

    def send_text(self, text):
        self.conn.send(OP_TEXT, text.encode('utf-8'))
        self.log('sent', 'text', masked(text))

    def pump(self, timeout_s):
        for opcode, payload in self.conn.receive(timeout_s):
            if opcode == OP_TEXT:
                text = payload.decode('utf-8')   # checked by FrameParser (Section 8.1)
                self.log('recv', 'text', text)
                try:
                    m = json.loads(text)
                except ValueError:
                    m = None
            else:
                self.log('recv', 'binary', base64.b64encode(payload).decode())
                m = None
            m = m if isinstance(m, dict) else None
            self.received.append(m)
            if m and m.get('type') == 'response':
                for label in self.labels.values():
                    if label['id'] == m.get('id') and isinstance(m.get('result'), dict):
                        s = m['result'].get('session')
                        label['session'] = s if isinstance(s, int) and not isinstance(s, bool) else None

    def wait(self, ms):
        end = time.monotonic() + ms / 1000.0
        while not self.conn.closed:
            left = end - time.monotonic()
            if left <= 0:
                return
            self.pump(left)

    def expect(self, pattern, within_ms, optional):
        """Returns 'PASS', 'SKIP' (optional pattern skipped) or 'FAIL: <reason>'."""
        end = time.monotonic() + within_ms / 1000.0
        while True:
            while self.cursor < len(self.received):
                m = self.received[self.cursor]
                if m is not None and matches(pattern, m, self.labels):
                    self.cursor += 1
                    return 'PASS'
                strict, session = pattern_session(pattern, self.labels)
                if (strict and m is not None and m.get('type') == 'event' and m.get('name') == 'session_state'
                        and isinstance(m.get('data'), dict) and m['data'].get('session') == session):
                    if optional:
                        return 'SKIP'
                    return 'FAIL: session %d went to %s before %s' % (session, json.dumps(m['data'].get('state')),
                                                                       json.dumps(pattern))
                self.cursor += 1
            left = end - time.monotonic()
            if left <= 0 or self.conn.closed:
                if optional:
                    return 'SKIP'
                return 'FAIL: %s not received within %d ms%s' % (json.dumps(pattern), within_ms,
                                                           ' (connection closed)' if self.conn.closed else '')
            self.pump(left)

    def expect_all(self, patterns, within_ms):
        """All patterns after the cursor, in any order; the cursor moves after the last of them."""
        end = time.monotonic() + within_ms / 1000.0
        while True:
            found, used = {}, set()
            for k, p in enumerate(patterns):
                for i in range(self.cursor, len(self.received)):
                    m = self.received[i]
                    if i not in used and m is not None and matches(p, m, self.labels):
                        found[k] = i
                        used.add(i)
                        break
            if len(found) == len(patterns):
                self.cursor = max(found.values()) + 1
                return 'PASS'
            left = end - time.monotonic()
            if left <= 0 or self.conn.closed:
                missing = [json.dumps(p) for k, p in enumerate(patterns) if k not in found]
                return 'FAIL: %s not received within %d ms' % (', '.join(missing), within_ms)
            self.pump(left)

    def run(self, steps):
        """Returns the number of failed expectations (0 or 1: the script stops at a failure)."""
        self.pump(0)   # messages received together with the handshake
        for step in steps:
            try:
                m = json.loads(step.get('raw', ''))
            except ValueError:
                continue
            if isinstance(m, dict) and isinstance(m.get('id'), int):
                self.raw_ids.add(m['id'])
        for n, step in enumerate(steps, 1):
            if 'command' in step:
                while self.next_id in self.raw_ids:
                    self.next_id += 1
                cid = self.next_id
                self.next_id += 1
                if 'as' in step:
                    self.labels[step['as']] = {'id': cid, 'session': None}
                self.send_text(json.dumps({'type': 'command', 'id': cid, 'name': step['command'],
                                           'params': step.get('params', {})}, ensure_ascii=False))
            elif 'raw' in step:
                self.send_text(step['raw'])
            elif 'binary_random' in step:
                payload = os.urandom(step['binary_random'])
                self.conn.send(OP_BINARY, payload)
                self.log('sent', 'binary', base64.b64encode(payload).decode())
            elif 'sleep_ms' in step:
                self.wait(step['sleep_ms'])
            elif 'expect' in step or 'expect_all' in step:
                if 'expect' in step:
                    what = 'expect'
                    result = self.expect(step['expect'], step.get('within_ms', 5000), step.get('optional', False))
                else:
                    what = 'expect_all'
                    result = self.expect_all(step['expect_all'], step.get('within_ms', 5000))
                status, _, reason = result.partition(': ')
                print('%s step %d: %s %s%s' % (status, n, what, json.dumps(step[what]), ': ' + reason if reason else ''),
                      flush=True)
                if status == 'FAIL':
                    return 1
            self.pump(0)
        return 0


def load_script(path):
    with open(path, encoding='utf-8') as f:
        steps = json.load(f)
    known = ('command', 'raw', 'binary_random', 'sleep_ms', 'expect', 'expect_all')
    if not isinstance(steps, list):
        raise ValueError('the script must be a JSON array of steps')
    for n, step in enumerate(steps, 1):
        if not isinstance(step, dict) or sum(k in step for k in known) != 1:
            raise ValueError('step %d needs exactly one of %s' % (n, ', '.join(known)))
    return steps


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--url', default='ws://127.0.0.1:8090/ws/control')
    parser.add_argument('--script', required=True, help='JSON array of steps')
    parser.add_argument('--record', help='JSON Lines file of all frames (input of contract_check.py --kind ws)')
    parser.add_argument('--linger-ms', type=int, default=1000, help='receive this long after the last step')
    parser.add_argument('--quiet', action='store_true', help='print only the results of the expectations')
    args = parser.parse_args(argv)
    try:
        steps = load_script(args.script)
        record = open(args.record, 'w', encoding='utf-8') if args.record else None
        conn = Connection(args.url)
    except (OSError, ValueError) as e:
        print('ws_client: %s' % e, file=sys.stderr)
        return 2
    runner = Runner(conn, record, args.quiet)
    try:
        failed = runner.run(steps)
        runner.wait(args.linger_ms)
    except (OSError, ProtocolError) as e:
        print('ws_client: %s' % e, file=sys.stderr)
        return 2
    finally:
        conn.close()
        if record:
            record.close()
    expects = sum('expect' in s or 'expect_all' in s for s in steps)
    print('%s: %d message(s) received, %d expectation(s), %d failed'
          % ('FAIL' if failed else 'PASS', len(runner.received), expects, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Self-tests of the integration test tools of tests/tools (Section 5.4.2).

Run with: python3 -m unittest discover -s tests/tools   (Python standard library only, NFR-11)

The test classes carry the ID of the first test of Section 5.4 that relies on the tool:
contract_check.py -> TST-501, ws_client.py -> TST-521, hls_check.py -> TST-522. Expected values come
from Section 3 of SPECIFICATION.md (its JSON examples, including the playlist example of Section 3.7,
must pass, and each variant below breaks exactly one rule of Section 3), from RFC 6455 (the
handshake example of Section 1.3 and the frames of Section 5.7) and from ISO/IEC 13818-1 (TS
packet fields of the hand-built segments).
"""

import base64
import contextlib
import copy
import hashlib
import io
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import contract_check  # noqa: E402
import hls_check  # noqa: E402
import ws_client  # noqa: E402

# Examples of Section 3 (3.2.1, 3.2.2, 3.2.3, 3.3.1, 3.6.3).
SERVICES = {'services': [
    {'service_id': 'svc-1', 'number': 1, 'name': 'Demo One', 'protected': False},
    {'service_id': 'svc-2', 'number': 2, 'name': 'Demo Two', 'protected': False},
    {'service_id': 'svc-3', 'number': 3, 'name': 'Premium One', 'protected': True},
    {'service_id': 'svc-4', 'number': 4, 'name': 'Premium Two', 'protected': True}]}
SERVICE_INFO = {'service_id': 'svc-3', 'number': 3, 'name': 'Premium One', 'protected': True,
                'description': 'Protected demo service',
                'stream_control_url': 'http://127.0.0.1:8081/api/v1/stream', 'archive_window_s': 7200}
EVENT = {'event_id': 'svc-3-1794999600', 'start': 1794999600, 'duration': 300, 'title': 'Premium Morning',
         'description': 'First program of the protected demo service.', 'in_archive': True}
EPG = {'server_time': 1795000000, 'from': 1794993000, 'to': 1795003000,
       'services': [{'service_id': 'svc-3', 'events': [EVENT]}]}
START_REQUEST = {'service_id': 'svc-3', 'start': 1794999600, 'client_host': '127.0.0.1',
                 'client_port': 5004, 'ssrc': 3054235519}
START = {'session_id': 'hs-000017', 'service_id': 'svc-3', 'start_time': 1794999600,
         'ssrc': 3054235519, 'payload_type': 33}
SESSION_STATE = {'session': 12, 'service_id': 'svc-3', 'state': 'playing', 'reason': None,
                 'start': 1794999600, 'start_time': 1794999600, 'playing_since': 1795000002,
                 'protected': True, 'error': None}
STREAM_INFO = {'session': 12, 'service_id': 'svc-3', 'program_number': 1, 'pmt_pid': 4096, 'pcr_pid': 256,
               'video': {'pid': 256, 'stream_type': 27, 'codec': 'H.264'},
               'audio': {'pid': 257, 'stream_type': 15, 'codec': 'AAC'}}


def errors(kind, obj):
    text = obj if isinstance(obj, str) else json.dumps(obj)
    return contract_check.check_text(kind, text, 'test')


def event(name, data):
    return {'type': 'event', 'name': name, 'data': data}


def changed(base, path, value):
    """Deep copy of `base` with the element at `path` (list of keys / indexes) set or deleted."""
    obj = copy.deepcopy(base)
    target = obj
    for key in path[:-1]:
        target = target[key]
    if value is DELETE:
        del target[path[-1]]
    else:
        target[path[-1]] = value
    return obj


DELETE = object()


class Tst501ContractCheckValid(unittest.TestCase):
    def test_spec_examples_pass(self):
        with open(contract_check.DEFAULT_SPEC, encoding='utf-8') as f:
            examples = contract_check.spec_examples(f.read())
        checked = [e for e in examples if e[0] is not None]
        # 3.2.1, 3.2.2, 3.2.3, 3.3.1 (two requests, one response), 3.3.2 and 3.3.3 (inline request
        # and response each), 3.6.3 (session_state, stream_info, mw_status); 3.5 is skipped.
        self.assertEqual(len(checked), 13)
        self.assertEqual(len(examples) - len(checked), 1)
        for kind, text, source in checked:
            self.assertEqual(contract_check.check_text(kind, text, source), [], source)

    def test_inline_copies_pass(self):
        for kind, obj in (('mw_services', SERVICES), ('mw_service_info', SERVICE_INFO), ('mw_epg', EPG),
                          ('he_start_request', START_REQUEST),
                          ('he_start_request', dict(START_REQUEST, start='live')), ('he_start', START),
                          ('ws_message', event('session_state', SESSION_STATE)),
                          ('ws_message', event('stream_info', STREAM_INFO)),
                          ('ws_message', event('stream_info', dict(STREAM_INFO, audio=None))),
                          ('ws_message', event('mw_status', {'reachable': False}))):
            self.assertEqual(errors(kind, obj), [], kind)

    def test_unknown_fields_are_ignored(self):
        self.assertEqual(errors('mw_services', changed(SERVICES, ['services', 0, 'logo'], 'x.png')), [])
        self.assertEqual(errors('he_start', dict(START, extra=1)), [])

    def test_session_state_variants_of_section_3_6_3(self):
        for changes in ({'state': 'key_required', 'reason': 'no_key', 'playing_since': None},
                        {'state': 'key_required', 'reason': 'key_rejected', 'playing_since': None},
                        {'state': 'no_signal', 'reason': 'data_lost'},
                        {'state': 'no_signal', 'reason': 'no_data_at_start', 'playing_since': None},
                        {'state': 'starting', 'start_time': None, 'playing_since': None, 'protected': None},
                        {'state': 'stopped', 'start': 'live'},
                        {'state': 'error', 'error': {'code': 'INVALID_REQUEST', 'message': 'x'}},   # ER-13
                        {'state': 'error', 'error': {'code': 'RECEIVER_ERROR', 'message': 'x'}}):
            data = dict(SESSION_STATE, **changes)
            self.assertEqual(errors('ws_message', event('session_state', data)), [], changes)


class Tst501ContractCheckInvalid(unittest.TestCase):
    def assertRejected(self, kind, obj, fragment):
        found = errors(kind, obj)
        self.assertTrue(found, 'no error for %s' % fragment)
        self.assertTrue(any(fragment in e for e in found), '%r not in %s' % (fragment, found))

    def test_service_list(self):   # Section 3.1, 3.2.1, REQ-02
        cases = ((['services', 0, 'protected'], DELETE, 'services[0].protected: missing'),
                 (['services', 0, 'protected'], 'yes', 'must be a boolean'),
                 (['services', 0, 'number'], 0, 'integer 1 - 999'),
                 (['services', 0, 'number'], 1000, 'integer 1 - 999'),
                 (['services', 0, 'number'], 1.0, 'integer 1 - 999'),
                 (['services', 1, 'number'], 1, 'duplicate number'),
                 (['services', 1, 'number'], 9, 'not sorted'),
                 (['services', 0, 'service_id'], '', 'svc-<n>'),
                 (['services', 1, 'service_id'], 'svc-1', 'duplicate svc-1'),
                 (['services', 0, 'name'], '', 'non-empty string'),
                 (['services'], {}, 'must be an array'))
        for path, value, fragment in cases:
            self.assertRejected('mw_services', changed(SERVICES, path, value), fragment)

    def test_service_info(self):   # Section 3.2.2
        self.assertRejected('mw_service_info', changed(SERVICE_INFO, ['stream_control_url'], DELETE), 'missing')
        self.assertRejected('mw_service_info', changed(SERVICE_INFO, ['archive_window_s'], '7200'), 'positive integer')

    def test_epg(self):   # Section 3.2.3
        second = dict(EVENT, event_id='svc-3-1794999900', start=1794999900, title='Premium Evening')
        two = changed(EPG, ['services', 0, 'events'], [EVENT, second])
        cases = ((['services', 0, 'events', 0, 'start'], 1794999650, 'not a multiple of 300'),
                 (['services', 0, 'events', 0, 'event_id'], 'svc-3-1', 'must be "svc-3-1794999600"'),
                 (['services', 0, 'events', 0, 'in_archive'], False, 'in_archive: must be true'),
                 (['services', 0, 'events', 0, 'title'], DELETE, 'title: missing'),
                 (['services', 0, 'events', 0, 'duration'], 0, 'positive integer'),
                 (['server_time'], 1794999000, 'in_archive: must be false'),
                 (['from'], 1795003000, 'from < to'),
                 (['to'], 1794993000 + 86401, 'to - from <= 86400'),
                 (['to'], 1794999600, 'does not overlap'))
        for path, value, fragment in cases:
            self.assertRejected('mw_epg', changed(EPG, path, value), fragment)
        self.assertRejected('mw_epg', changed(two, ['services', 0, 'events', 1, 'event_id'], 'svc-3-1794999600'),
                            'duplicate svc-3-1794999600')
        self.assertRejected('mw_epg', changed(two, ['services', 0, 'events', 0, 'duration'], 600), 'overlapping')
        unsorted = changed(EPG, ['services', 0, 'events'], [second, EVENT])
        self.assertRejected('mw_epg', unsorted, 'not sorted or overlapping')
        self.assertRejected('mw_epg', changed(EPG, ['to'], DELETE), 'to: missing')

    def test_http_errors_per_interface(self):   # Section 3.10, column "Interface"
        self.assertEqual(errors('mw_error', {'error': {'code': 'INVALID_RANGE', 'message': 'x'}}), [])
        self.assertRejected('mw_error', {'error': {'code': 'OUT_OF_ARCHIVE', 'message': 'x'}}, 'one of')
        self.assertRejected('mw_error', {'error': {'code': 'INVALID_REQUEST', 'message': 'x'}}, 'one of')
        self.assertEqual(errors('he_error', {'error': {'code': 'SESSION_NOT_FOUND', 'message': 'x'}}), [])
        self.assertRejected('he_error', {'error': {'code': 'INVALID_RANGE', 'message': 'x'}}, 'one of')
        self.assertRejected('he_error', {'error': {'code': 'FUTURE_TIME'}}, 'message: missing')
        self.assertRejected('he_error', {'code': 'FUTURE_TIME', 'message': 'x'}, 'error: missing')

    def test_stream_control(self):   # Section 3.3, IN-02
        for value in (-1, 4294967296, 'abc', 1.5):
            self.assertRejected('he_start_request', dict(START_REQUEST, ssrc=value), 'unsigned 32-bit')
            self.assertRejected('he_start', dict(START, ssrc=value), 'unsigned 32-bit')
        self.assertRejected('he_start_request', dict(START_REQUEST, start='yesterday'), '"live" or an integer')
        self.assertRejected('he_start_request', dict(START_REQUEST, client_port=70000), '1 - 65535')
        self.assertRejected('he_start', dict(START, payload_type=96), 'must be 33')
        self.assertRejected('he_stop', {'stopped': False}, 'must be true')
        self.assertRejected('he_keepalive', {'expires_in_s': '15'}, 'positive integer')
        self.assertRejected('he_session_request', {}, 'session_id: missing')

    def test_session_state(self):   # Section 3.6.3, 3.10, 4.3.4
        cases = (({'state': 'idle'}, 'never sent as an event'),
                 ({'state': 'paused'}, 'one of'),
                 ({'reason': 'no_key'}, 'must be null in state playing'),
                 ({'state': 'key_required', 'reason': None}, "one of ['key_rejected', 'no_key']"),
                 ({'state': 'no_signal', 'reason': 'no_key'}, "one of ['data_lost', 'no_data_at_start']"),
                 ({'state': 'error'}, 'must be an error object'),
                 ({'error': {'code': 'INVALID_STREAM', 'message': 'x'}}, 'must be null in state playing'),
                 ({'state': 'error', 'error': {'code': 'FUTURE_TIME', 'message': 'x'}}, 'one of'),
                 ({'state': 'error', 'error': {'code': 'INVALID_COMMAND', 'message': 'x'}}, 'one of'),
                 ({'playing_since': None}, 'must be set in state playing'),
                 ({'start_time': None}, 'must be set in state playing'),
                 ({'protected': None}, 'must be decided in state playing'),
                 ({'state': 'key_required', 'reason': 'no_key', 'protected': False}, 'must be true'),
                 ({'protected': 'yes'}, 'boolean or null'),
                 ({'start': 'now'}, '"live" or an integer'),
                 ({'start_time': 1794999600.5}, 'integer or null'),
                 ({'session': 0}, 'positive integer'))
        for changes, fragment in cases:
            self.assertRejected('ws_message', event('session_state', dict(SESSION_STATE, **changes)), fragment)
        self.assertRejected('ws_message', event('session_state', changed(SESSION_STATE, ['playing_since'], DELETE)),
                            'playing_since: missing')
        self.assertRejected('ws_message', event('session_state', changed(SESSION_STATE, ['error'], DELETE)),
                            'error: missing')

    def test_stream_info_and_mw_status(self):   # Section 3.6.3, REQ-17
        self.assertRejected('ws_message', event('stream_info', changed(STREAM_INFO, ['video', 'stream_type'], 15)), 'must be 27')
        self.assertRejected('ws_message', event('stream_info', changed(STREAM_INFO, ['audio', 'codec'], 'MP3')), '"AAC"')
        self.assertRejected('ws_message', event('stream_info', dict(STREAM_INFO, pmt_pid=9000)), 'PID 0 - 8191')
        self.assertRejected('ws_message', event('stream_info', dict(STREAM_INFO, video=None)), 'must be an object')
        self.assertRejected('ws_message', event('stream_info', changed(STREAM_INFO, ['audio'], DELETE)), 'audio: missing')
        self.assertRejected('ws_message', event('mw_status', {'reachable': 'true'}), 'boolean')

    def test_envelope(self):   # Section 3.6.1
        self.assertRejected('ws_message', event('key_required', {}), 'one of')
        self.assertRejected('ws_message', {'type': 'command', 'id': 1, 'name': 'stop', 'params': {}}, '"response" or "event"')
        self.assertRejected('ws_message', {'type': 'response', 'id': 1, 'ok': True}, 'result: missing')
        self.assertRejected('ws_message', {'type': 'response', 'id': '1', 'ok': True, 'result': {}}, 'integer')
        self.assertRejected('ws_message', {'type': 'response', 'id': 1, 'ok': False, 'error': {'code': 'NOPE', 'message': 'x'}}, 'one of')
        self.assertRejected('ws_message', '{"type": "event", "name": "mw_status", "data": {"reachable": NaN}}', 'not valid JSON')
        self.assertRejected('ws_message', '[1, 2', 'not valid JSON')


def recording(*items):
    """JSON Lines of ws_client.py: ('sent' | 'recv', message object or raw text)."""
    lines = []
    for t, (direction, message) in enumerate(items):
        text = message if isinstance(message, str) else json.dumps(message)
        lines.append(json.dumps({'t_ms': 1795000000000 + t, 'dir': direction, 'kind': 'text', 'text': text}))
    return '\n'.join(lines) + '\n'


def command(cid, name, params=None):
    return ('sent', {'type': 'command', 'id': cid, 'name': name, 'params': params or {}})


def ok(cid, result):
    return ('recv', {'type': 'response', 'id': cid, 'ok': True, 'result': result})


def fail(cid, code):
    return ('recv', {'type': 'response', 'id': cid, 'ok': False, 'error': {'code': code, 'message': 'x'}})


class Tst501ContractCheckRecording(unittest.TestCase):
    def test_valid_recording(self):   # Section 3.6.2 results and the IN-07 inputs of TST-532
        text = recording(
            ('recv', event('mw_status', {'reachable': True})),
            command(1, 'get_services'), ok(1, SERVICES),
            command(2, 'get_epg', {'from': 1794993000, 'to': 1795003000}),
            ok(2, {'server_time': EPG['server_time'], 'services': EPG['services']}),
            command(3, 'get_status'), ok(3, {'state': 'idle'}),
            command(4, 'play', {'service_id': 'svc-3', 'start': 1794999600}), ok(4, {'session': 12}),
            ('recv', event('session_state', dict(SESSION_STATE, state='starting', start_time=None,
                                                 playing_since=None, protected=None))),
            command(5, 'set_key', {'service_id': 'svc-3', 'key': '3A7F…'}), ok(5, {}),
            command(6, 'get_status'), ok(6, SESSION_STATE),
            command(7, 'play', {'service_id': 'svc-9', 'start': 'live'}), fail(7, 'SERVICE_NOT_FOUND'),
            command(8, 'play', {'service_id': 'svc-1', 'start': 'live'}), ok(8, {'session': 13}),
            ('sent', 'not json'),
            command(9, 'fly'), fail(9, 'INVALID_COMMAND'),
            command(10, 'play', {'start': -5.5}), fail(10, 'INVALID_COMMAND'),
            command(11, 'stop'), ok(11, {}))
        self.assertEqual(errors('ws', text), [])

    def assertRejected(self, text, fragment):
        found = errors('ws', text)
        self.assertTrue(any(fragment in e for e in found), '%r not in %s' % (fragment, found))

    def test_invalid_recordings(self):   # Sections 3.6.1, 3.6.2
        self.assertRejected(recording(command(1, 'play', {}), ok(1, {'session': 'x'})), 'positive integer')
        self.assertRejected(recording(command(1, 'set_key', {}), fail(1, 'MW_UNREACHABLE')), 'one of')
        self.assertRejected(recording(command(1, 'fly'), fail(1, 'SERVICE_NOT_FOUND')), "one of ['INVALID_COMMAND']")
        self.assertRejected(recording(command(1, 'get_status'), ok(1, {'state': 'playing'})), 'missing')
        self.assertRejected(recording(command(1, 'get_services'), ok(1, {})), 'services: missing')
        self.assertRejected(recording(command(1, 'stop'), command(2, 'stop'), ok(1, {})), 'no response to command 2')
        self.assertRejected(recording(command(1, 'stop'), ok(1, {}), ok(1, {})), 'second response')
        self.assertRejected(recording(command(1, 'stop'), ok(2, {})), 'which was not sent')
        self.assertRejected(recording(command(1, 'play', {}), ok(1, {'session': 5}),
                                      command(2, 'play', {}), ok(2, {'session': 5})), 'not higher')
        binary = json.dumps({'t_ms': 1, 'dir': 'recv', 'kind': 'binary', 'text': ''})
        self.assertRejected(binary, 'binary frame')
        self.assertRejected(recording(('recv', 'garbage')), 'not valid JSON')
        self.assertRejected('{"t_ms": 1}\n', 'not a recording line')


# ---------------------------------------------------------------- ws_client.py (TST-521)

class Tst521WsClientFrames(unittest.TestCase):
    def test_handshake_example_of_rfc_6455(self):   # RFC 6455, Section 1.3
        self.assertEqual(ws_client.accept_key('dGhlIHNhbXBsZSBub25jZQ=='), 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=')

    def test_frames_of_rfc_6455_section_5_7(self):
        hello = b'Hello'
        self.assertEqual(ws_client.encode_frame(1, hello, None), bytes([0x81, 0x05]) + hello)
        self.assertEqual(ws_client.encode_frame(1, hello, bytes([0x37, 0xFA, 0x21, 0x3D])),
                         bytes([0x81, 0x85, 0x37, 0xFA, 0x21, 0x3D, 0x7F, 0x9F, 0x4D, 0x51, 0x58]))
        self.assertEqual(ws_client.encode_frame(2, bytes(256), None)[:4], bytes([0x82, 0x7E, 0x01, 0x00]))
        self.assertEqual(ws_client.encode_frame(2, bytes(65536), None)[:10],
                         bytes([0x82, 0x7F, 0, 0, 0, 0, 0, 1, 0, 0]))

    def test_parser_joins_fragments_and_passes_control_frames(self):
        p = ws_client.FrameParser()
        data = (bytes([0x01, 0x03]) + b'Hel' + bytes([0x89, 0x05]) + b'Hello' + bytes([0x80, 0x02]) + b'lo'
                + bytes([0x82, 0x7E, 0x01, 0x00]) + bytes(256))
        out = []
        for i in range(len(data)):            # one byte at a time: frames split anywhere
            out += p.feed(data[i:i + 1])
        self.assertEqual(out, [(9, b'Hello'), (1, b'Hello'), (2, bytes(256))])

    def test_parser_rejects_masked_server_frames_and_bad_sequences(self):
        for data in (bytes([0x81, 0x85, 1, 2, 3, 4]) + bytes(5),     # masked (Section 5.1)
                     bytes([0x80, 0x00]),                            # continuation without a message
                     bytes([0x01, 0x00, 0x81, 0x00]),                # new message inside a fragmented one
                     bytes([0x09, 0x00]),                            # fragmented control frame
                     bytes([0xC1, 0x00]),                            # reserved bit
                     bytes([0x83, 0x00])):                           # reserved opcode
            with self.assertRaises(ws_client.ProtocolError, msg=data.hex()):
                ws_client.FrameParser().feed(data)

    def test_key_is_masked_in_sent_text(self):   # REQ-55, NFR-08
        text = json.dumps({'type': 'command', 'id': 3, 'name': 'set_key',
                           'params': {'service_id': 'svc-3', 'key': '3A7F1C9E5B2D8046A1E3C5079B4D6F28'}})
        self.assertEqual(json.loads(ws_client.masked(text))['params']['key'], '3A7F…')
        self.assertEqual(ws_client.masked('not json'), 'not json')

    def test_patterns(self):
        labels = {'p': {'id': 4, 'session': 12}}
        state = {'type': 'event', 'name': 'session_state', 'data': {'session': 12, 'state': 'playing', 'reason': None}}
        m = ws_client.matches
        self.assertTrue(m({'name': 'session_state', 'data': {'session': '$p', 'state': 'playing'}}, state, labels))
        self.assertTrue(m({'data': {'state': ['waiting_stream', 'playing']}}, state, labels))
        self.assertTrue(m({'data': {'reason': None}}, state, labels))
        self.assertFalse(m({'data': {'state': 'starting'}}, state, labels))
        self.assertFalse(m({'data': {'session': '$q'}}, state, labels))
        self.assertFalse(m({'data': {'missing': None}}, state, labels))
        self.assertFalse(m({'data': {'session': 12.0}}, state, labels))
        self.assertTrue(m({'response_to': 'p', 'ok': True}, {'type': 'response', 'id': 4, 'ok': True}, labels))
        self.assertFalse(m({'response_to': 'p'}, {'type': 'response', 'id': 5, 'ok': True}, labels))


def server_frame(opcode, payload, fin=True):
    """Unmasked server frame, written here independently of ws_client.encode_frame."""
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    head += bytes([n]) if n < 126 else bytes([126]) + struct.pack('!H', n)
    return head + payload


class TestServer:
    """A one-connection WebSocket server; `respond(command)` returns the frames to send."""

    def __init__(self, respond, greeting=b''):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.url = 'ws://127.0.0.1:%d/ws/control' % self.listener.getsockname()[1]
        self.respond = respond
        self.greeting = greeting
        self.received = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        conn, _ = self.listener.accept()
        self.listener.close()
        conn.settimeout(10)
        data = b''
        while b'\r\n\r\n' not in data:
            data += conn.recv(4096)
        key = [l.split(':', 1)[1].strip() for l in data.decode().split('\r\n') if l.lower().startswith('sec-websocket-key')][0]
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                     b'Sec-WebSocket-Accept: ' + accept + b'\r\n\r\n' + self.greeting)
        buf = b''
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 2:
                n, off = buf[1] & 0x7F, 2
                if n == 126:
                    n, off = struct.unpack('!H', buf[2:4])[0], 4
                if len(buf) < off + 4 + n:
                    break
                mask, payload = buf[off:off + 4], buf[off + 4:off + 4 + n]
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                opcode, buf = buf[0] & 0x0F, buf[off + 4 + n:]
                self.received.append((opcode, payload))
                if opcode == 8:
                    conn.sendall(server_frame(8, payload))
                    conn.close()
                    return
                if opcode == 1:
                    try:
                        command = json.loads(payload)
                    except ValueError:
                        continue
                    for frame in self.respond(command):
                        conn.sendall(frame)


def text_frame(obj):
    return server_frame(1, json.dumps(obj).encode())


def state_event(session, state):
    return {'type': 'event', 'name': 'session_state',
            'data': dict(SESSION_STATE, session=session, state=state)}


def run_client(server, steps, record=None):
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as f:
        json.dump(steps, f)
    argv = ['--url', server.url, '--script', f.name, '--linger-ms', '200']
    if record:
        argv += ['--record', record]
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            code = ws_client.main(argv)
    finally:
        os.unlink(f.name)
    server.thread.join(5)
    return code, out.getvalue()


PLAY_STEPS = [
    {'command': 'play', 'params': {'service_id': 'svc-3', 'start': 'live'}, 'as': 'p'},
    {'expect': {'response_to': 'p', 'ok': True}, 'within_ms': 2000},
    {'expect': {'name': 'session_state', 'data': {'session': '$p', 'state': 'starting'}}, 'within_ms': 2000},
    {'expect': {'name': 'session_state', 'data': {'session': '$p', 'state': 'waiting_stream'}}, 'optional': True,
     'within_ms': 2000},
    {'expect': {'name': 'session_state', 'data': {'session': '$p', 'state': 'playing'}}, 'within_ms': 2000},
    {'command': 'set_key', 'params': {'service_id': 'svc-3', 'key': '3A7F1C9E5B2D8046A1E3C5079B4D6F28'}},
    {'raw': 'not json'},
    {'binary_random': 300},
    {'sleep_ms': 100},
]


class Tst521WsClientRun(unittest.TestCase):
    def test_script_record_and_expectations(self):
        def respond(c):
            if c.get('name') == 'play':
                playing = json.dumps(state_event(7, 'playing')).encode()
                return [text_frame({'type': 'response', 'id': c['id'], 'ok': True, 'result': {'session': 7}}),
                        server_frame(9, b'ping'),
                        text_frame(state_event(6, 'stopped')),          # another session: skipped
                        text_frame(state_event(7, 'starting')),
                        server_frame(1, playing[:10], fin=False),        # fragmented message
                        server_frame(0, playing[10:])]
            return [text_frame({'type': 'response', 'id': c['id'], 'ok': True, 'result': {}})]
        server = TestServer(respond, greeting=text_frame({'type': 'event', 'name': 'mw_status',
                                                          'data': {'reachable': True}}))
        with tempfile.TemporaryDirectory() as d:
            record = os.path.join(d, 'rec.jsonl')
            code, out = run_client(server, PLAY_STEPS, record)
            with open(record, encoding='utf-8') as f:
                text = f.read()
        self.assertEqual(code, 0, out)
        self.assertIn('PASS: 6 message(s) received, 4 expectation(s), 0 failed', out)
        lines = [json.loads(l) for l in text.splitlines()]
        kinds = [(l['dir'], l['kind']) for l in lines]
        self.assertEqual(kinds[0], ('recv', 'text'))                     # mw_status of the handshake, first
        self.assertEqual((kinds.count(('recv', 'text')), kinds.count(('sent', 'text')),
                          kinds.count(('sent', 'binary'))), (6, 3, 1))
        self.assertNotIn('3A7F1C9E', text + out)                         # key masked (NFR-08)
        self.assertIn('3A7F…', text)
        self.assertTrue(all(isinstance(l['t_ms'], int) and l['t_ms'] > 1700000000000 for l in lines))
        self.assertEqual(contract_check.check_text('ws', text, 'rec'), [])
        kinds = [op for op, _ in server.received]
        self.assertEqual(kinds, [1, 10, 1, 1, 2, 8])                     # pong answered the ping; close at the end
        self.assertEqual(server.received[1][1], b'ping')
        self.assertIn('SKIP step 4', out)               # optional waiting_stream not sent
        self.assertIn('<binary 300 bytes>', out)

    def test_unexpected_state_in_between_fails(self):
        def respond(c):
            return [text_frame({'type': 'response', 'id': c['id'], 'ok': True, 'result': {'session': 7}}),
                    text_frame(state_event(7, 'starting')), text_frame(state_event(7, 'no_signal')),
                    text_frame(state_event(7, 'playing'))]
        code, out = run_client(TestServer(respond), PLAY_STEPS[:5])
        self.assertEqual(code, 1, out)
        self.assertIn('session 7 went to "no_signal"', out)

    def test_ids_of_raw_steps_are_not_assigned(self):   # IN-07 uses the id 5
        def respond(c):
            return [text_frame({'type': 'response', 'id': c['id'], 'ok': True, 'result': {}})]
        server = TestServer(respond)
        steps = [{'raw': json.dumps({'type': 'command', 'id': 2, 'name': 'fly', 'params': {}})},
                 {'command': 'stop'}, {'command': 'stop'}, {'sleep_ms': 100}]
        code, out = run_client(server, steps)
        self.assertEqual(code, 0, out)
        ids = [json.loads(p)['id'] for op, p in server.received if op == 1]
        self.assertEqual(ids, [2, 1, 3])

    def test_expect_all_accepts_any_order(self):   # response to stop and stopped: order not specified
        def respond(c):
            stopped = text_frame(state_event(7, 'stopped'))
            response = text_frame({'type': 'response', 'id': c['id'], 'ok': True, 'result': {}})
            return [stopped, response] if c['id'] == 1 else [response, stopped]
        both = {'expect_all': [{'response_to': 's'}, {'name': 'session_state', 'data': {'state': 'stopped'}}],
                'within_ms': 1000}
        code, out = run_client(TestServer(respond), [{'command': 'stop', 'as': 's'}, both,
                                                     {'command': 'stop', 'as': 's'}, both])
        self.assertEqual(code, 0, out)
        code, out = run_client(TestServer(lambda c: [text_frame(state_event(7, 'stopped'))]),
                               [{'command': 'stop', 'as': 's'}, dict(both, within_ms=300)])
        self.assertEqual(code, 1, out)
        self.assertIn('{"response_to": "s"} not received within 300 ms', out)

    def test_missing_message_fails_at_its_time(self):
        def respond(c):
            return [text_frame({'type': 'response', 'id': c['id'], 'ok': True, 'result': {'session': 7}})]
        steps = PLAY_STEPS[:2] + [dict(PLAY_STEPS[2], within_ms=300)]
        code, out = run_client(TestServer(respond), steps)
        self.assertEqual(code, 1, out)
        self.assertIn('not received within 300 ms', out)


# ---------------------------------------------------------------- hls_check.py (TST-522)

# The example of Section 3.7, one line each.
PLAYLIST = ('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:41\n'
            '#EXTINF:1.000,\nseg-41.ts\n#EXTINF:1.000,\nseg-42.ts\n#EXT-X-DISCONTINUITY\n#EXTINF:1.000,\nseg-43.ts\n')


def playlist(first, durations, marked=(), dseq=None):
    text = '#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:%d\n' % first
    if dseq is not None:
        text += '#EXT-X-DISCONTINUITY-SEQUENCE:%d\n' % dseq
    for i, d in enumerate(durations):
        if first + i in marked:
            text += '#EXT-X-DISCONTINUITY\n'
        text += '#EXTINF:%s,\nseg-%d.ts\n' % (d, first + i)
    return text


class Tst522HlsCheckPlaylist(unittest.TestCase):
    def test_example_of_section_3_7_passes(self):
        p, errors = hls_check.parse_playlist(PLAYLIST)
        self.assertEqual(errors, [])
        self.assertEqual(p.media_sequence, 41)
        self.assertEqual(p.segments, [(41, '1.000', False), (42, '1.000', False), (43, '1.000', True)])
        self.assertEqual(p.discontinuity_sequence, 0)
        self.assertEqual(hls_check.parse_playlist(playlist(0, ['0.980', '2.000'], dseq=1))[1], [])

    def test_variants_are_rejected(self):
        cases = ((PLAYLIST.replace('#EXTM3U\n', ''), 'first line is not #EXTM3U'),
                 (PLAYLIST.replace('#EXT-X-VERSION:3\n', ''), '#EXT-X-VERSION missing'),
                 (PLAYLIST.replace('VERSION:3', 'VERSION:4'), 'must be #EXT-X-VERSION:3'),
                 (PLAYLIST.replace('TARGETDURATION:2', 'TARGETDURATION:1'), 'must be #EXT-X-TARGETDURATION:2'),
                 (PLAYLIST.replace('#EXT-X-MEDIA-SEQUENCE:41\n', ''), '#EXT-X-MEDIA-SEQUENCE missing'),
                 (PLAYLIST.replace('SEQUENCE:41', 'SEQUENCE:40'), 'do not follow'),
                 (PLAYLIST.replace('seg-42.ts', 'seg-44.ts'), 'do not follow'),
                 (PLAYLIST.replace('seg-42.ts', 'segment-42.ts'), 'must be seg-<sequence>.ts'),
                 (PLAYLIST.replace('#EXTINF:1.000,\nseg-41', '#EXTINF:1.0,\nseg-41'), '3 decimals'),
                 (PLAYLIST.replace('#EXTINF:1.000,\nseg-41', '#EXTINF:1.000\nseg-41'), '3 decimals'),
                 (PLAYLIST.replace('#EXTINF:1.000,\nseg-41', '#EXTINF:2.001,\nseg-41'), 'not in (0, 2.000]'),
                 (PLAYLIST.replace('#EXTINF:1.000,\nseg-41', '#EXTINF:0.000,\nseg-41'), 'not in (0, 2.000]'),
                 (PLAYLIST + '#EXT-X-ENDLIST\n', 'tag not allowed'),
                 (PLAYLIST.replace('#EXTINF:1.000,\nseg-41', '#EXT-X-PROGRAM-DATE-TIME:x\n#EXTINF:1.000,\nseg-41'),
                  'tag not allowed'),
                 (PLAYLIST + '#EXTINF:1.000,\n', 'ends before the URI'),
                 (PLAYLIST.replace('seg-41.ts\n', ''), '#EXTINF without a segment URI'),
                 (PLAYLIST + 'seg-44.ts\n', 'segment URI without #EXTINF'),
                 (PLAYLIST + '#EXT-X-VERSION:3\n', 'appears twice'),
                 (playlist(0, ['1.000'] * 7), '7 segments listed'),
                 (playlist(0, []), '0 segments listed'))
        for text, fragment in cases:
            errors = hls_check.parse_playlist(text)[1]
            self.assertTrue(any(fragment in e for e in errors), '%r not in %s' % (fragment, errors))

    def test_reloads(self):   # REQ-24: #EXT-X-DISCONTINUITY-SEQUENCE when marked segments leave
        old = hls_check.parse_playlist(PLAYLIST)[0]
        compare = lambda text: hls_check.compare_playlists(old, hls_check.parse_playlist(text)[0])
        self.assertEqual(compare(playlist(42, ['1.000'] * 3, marked={43})), [])
        self.assertEqual(compare(playlist(44, ['1.000'] * 2, dseq=1)), [])
        self.assertIn('expected 1', compare(playlist(44, ['1.000'] * 2))[0])
        self.assertIn('expected 0', compare(playlist(42, ['1.000'] * 2, marked={43}, dseq=1))[0])
        self.assertIn('decreased', compare(playlist(40, ['1.000'] * 3))[0])
        self.assertIn('changed', compare(playlist(42, ['1.000', '1.040'], marked={43}))[0])
        self.assertIn('changed', compare(playlist(42, ['1.000', '1.000']))[0])
        self.assertEqual(compare(playlist(60, ['1.000'], dseq=3)), [])   # segments in between not seen


def ts_packet(pid, pusi=False, rai=False, scrambled=False):
    """188 bytes: header (ISO/IEC 13818-1, 2.4.3.2) and, for `rai`, an adaptation field with
    random_access_indicator = 1."""
    b1 = (0x40 if pusi else 0) | (pid >> 8)
    if rai:
        head = bytes([0x47, b1, pid & 0xFF, (0x80 if scrambled else 0) | 0x30, 1, 0x40])
    else:
        head = bytes([0x47, b1, pid & 0xFF, (0x80 if scrambled else 0) | 0x10])
    return head + bytes(TS_SIZE - len(head))


TS_SIZE = 188
ALLOWED = {0, 4096, 256, 257}


def segment(*packets):
    return b''.join(packets)


class Tst522HlsCheckSegment(unittest.TestCase):
    def good(self):
        return [ts_packet(0, pusi=True), ts_packet(4096, pusi=True), ts_packet(256, pusi=True, rai=True),
                ts_packet(257, pusi=True), ts_packet(256)]

    def test_valid_segment(self):
        pids, errors = hls_check.scan_ts(segment(*self.good()), ALLOWED, 4096, 256)
        self.assertEqual(errors, [])
        self.assertEqual(pids, ALLOWED)

    def test_invalid_segments(self):
        g = self.good()
        cases = ((g + [ts_packet(8191)], 'PIDs [8191] not in the program'),
                 (g + [ts_packet(257, scrambled=True)], 'transport_scrambling_control'),
                 ([g[1], g[0]] + g[2:], 'first packets have PIDs [4096, 0, 256]'),
                 (g[:2] + [ts_packet(256, pusi=True)] + g[3:], 'random_access_indicator'),
                 (g[:2] + [ts_packet(256, rai=True)] + g[3:], 'payload_unit_start_indicator'),
                 (g + [b'\x46' + bytes(187)], 'sync byte 0x46'),
                 (g + [bytes(10)], 'not a positive multiple of 188'))
        for packets, fragment in cases:
            errors = hls_check.scan_ts(segment(*packets), ALLOWED, 4096, 256)[1]
            self.assertTrue(any(fragment in e for e in errors), '%r not in %s' % (fragment, errors))
        self.assertTrue(hls_check.scan_ts(b'', ALLOWED, 4096, 256)[1])

    def test_segment_files_are_counted_with_temporary_names(self):   # B7: also the one being written
        with tempfile.TemporaryDirectory() as d:
            for name in ('seg-7.ts', 'seg-8.ts', '.tmp-3-seg-9.ts', 'index.m3u8', '.tmp-4-index.m3u8', 'seg-8.ts.old'):
                open(os.path.join(d, name), 'w').close()
            self.assertEqual(hls_check.count_segment_files(d), (3, True))
            self.assertEqual(hls_check.count_segment_files(os.path.join(d, 'missing')), (0, False))


if __name__ == '__main__':
    unittest.main()

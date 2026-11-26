#!/usr/bin/env python3
"""Self-tests of the integration test tools of tests/tools (Section 5.4.2).

Run with: python3 -m unittest discover -s tests/tools   (Python standard library only, NFR-11)

The test classes carry the ID of the first test of Section 5.4 that relies on the tool:
contract_check.py -> TST-501. Expected values come from Section 3 of SPECIFICATION.md: its JSON
examples must pass, and each variant below breaks exactly one rule of Section 3.
"""

import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import contract_check  # noqa: E402

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


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Contract checker: validates JSON messages against the formats of Section 3 (Section 5.4.2).

Checks mandatory fields, types and the value sets of `state`, `reason` and the error `code`, and
the provider guarantees of Section 3.2 that can be seen in a single message (order, uniqueness,
event grid, `event_id`, `in_archive`). Unknown fields are ignored (Section 3.1).

Usage (Python standard library only, NFR-11):
  contract_check.py --kind KIND FILE...    check each FILE ('-' = standard input) as KIND
  contract_check.py --spec-examples        check the JSON examples of Section 3 of SPECIFICATION.md

Kinds:
  mw_services, mw_service_info, mw_epg, mw_error            IF-EXT-03 responses (Section 3.2)
  he_start_request, he_session_request                      IF-EXT-04 request bodies (Section 3.3)
  he_start, he_stop, he_keepalive, he_error                 IF-EXT-04 responses (Section 3.3)
  ws_message                                                one IF-INT-01 message of the AL (Section 3.6)
  ws                                                        a recording of tests/tools/ws_client.py

A `ws` recording has one JSON object per line: {"t_ms": ..., "dir": "sent" | "recv",
"kind": "text" | "binary", "text": "..."}. Only the received messages are checked; the sent
commands are used to pair every response with its command (result schema and error codes of
Section 3.6.2) and to check that every command with a readable `id` got exactly one response
(Section 3.6.1). Sent messages may be invalid on purpose (IN-07).

Output: one line per error, "<source>: <path>: <problem>", then a summary line. The exit code is
0 if no error was found, 1 otherwise, 2 for a usage error or an unreadable input.
"""

import argparse
import json
import os
import re
import sys

DEFAULT_SPEC = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'SPECIFICATION.md'))

ARCHIVE_WINDOW_S = 7200   # Section 3.1, D-08
EPG_GRID_S = 300          # Section 3.2.3
EPG_MAX_RANGE_S = 86400   # Section 3.2.3

STATES = {'idle', 'starting', 'waiting_stream', 'key_required', 'validating_key', 'playing',
          'no_signal', 'stopped', 'error'}                                  # Section 3.6.3
REASONS = {'key_required': {'no_key', 'key_rejected'},
           'no_signal': {'no_data_at_start', 'data_lost'}}                 # Section 3.6.3

# Error codes per interface, column "Interface" of Section 3.10.
CODES_MW = {'INVALID_RANGE', 'SERVICE_NOT_FOUND'}
CODES_HE = {'INVALID_REQUEST', 'OUT_OF_ARCHIVE', 'FUTURE_TIME', 'SERVICE_NOT_FOUND',
            'SESSION_NOT_FOUND'}
# Codes of a session in state `error`: the transitions that end in `error` (Section 4.3.4):
# T-04, T-30 RECEIVER_ERROR; T-05 MW_UNREACHABLE, SERVICE_NOT_FOUND; T-08 the code of the head-end
# other than FUTURE_TIME (T-07) or HEADEND_UNREACHABLE; T-13, T-21 INVALID_STREAM; T-28, T-29
# HEADEND_UNREACHABLE. INVALID_REQUEST is expected here by ER-13 and TST-133 (c).
CODES_SESSION = {'RECEIVER_ERROR', 'MW_UNREACHABLE', 'SERVICE_NOT_FOUND', 'INVALID_REQUEST',
                 'OUT_OF_ARCHIVE', 'HEADEND_UNREACHABLE', 'INVALID_STREAM'}
# Error codes of the responses per command (Section 3.6.2); INVALID_COMMAND is possible for every
# command (Section 3.6.1: malformed command).
COMMAND_CODES = {
    'get_services': {'MW_UNREACHABLE'},
    'get_epg': {'MW_UNREACHABLE', 'INVALID_RANGE'},
    'play': {'SERVICE_NOT_FOUND'},
    'set_key': {'INVALID_KEY_FORMAT', 'SERVICE_NOT_FOUND'},
    'stop': set(),
    'get_status': set(),
}
CODES_UI = set().union(*COMMAND_CODES.values(), {'INVALID_COMMAND'})

SERVICE_ID = re.compile(r'svc-[0-9]+')   # Section 3.1


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_text(v):
    return isinstance(v, str) and v != ''


class Report:
    """Collects the errors of one source."""

    def __init__(self, source):
        self.source = source
        self.errors = []

    def error(self, path, problem):
        self.errors.append('%s: %s: %s' % (self.source, path or '<root>', problem))


def loads(text):
    """json.loads that rejects NaN and Infinity, which are not JSON (RFC 8259)."""
    def reject(name):
        raise ValueError('%s is not valid JSON' % name)
    return json.loads(text, parse_constant=reject)


# ---------------------------------------------------------------- field helpers

def need(r, obj, path, name, check, what):
    """Checks the mandatory field `name` of `obj`; returns its value or None."""
    p = '%s.%s' % (path, name) if path else name
    if name not in obj:
        r.error(p, 'missing')
        return None
    v = obj[name]
    if not check(v):
        r.error(p, 'must be %s, is %s' % (what, json.dumps(v)))
        return None
    return v


def need_object(r, v, path):
    if not isinstance(v, dict):
        r.error(path, 'must be an object, is %s' % json.dumps(v))
        return False
    return True


def need_list(r, obj, path, name):
    return need(r, obj, path, name, lambda v: isinstance(v, list), 'an array')


def uint32(v):
    return is_int(v) and 0 <= v <= 0xFFFFFFFF


def start_point(v):
    return v == 'live' or is_int(v)


def nullable(check):
    return lambda v: v is None or check(v)


# ---------------------------------------------------------------- IF-EXT-03 (Section 3.2)

def check_service_entry(r, e, path):
    if not need_object(r, e, path):
        return None
    sid = need(r, e, path, 'service_id', lambda v: isinstance(v, str) and SERVICE_ID.fullmatch(v),
               'a string "svc-<n>"')
    number = need(r, e, path, 'number', lambda v: is_int(v) and 1 <= v <= 999, 'an integer 1 - 999')
    need(r, e, path, 'name', is_text, 'a non-empty string')
    need(r, e, path, 'protected', lambda v: isinstance(v, bool), 'a boolean')
    return sid, number


def check_service_list(r, obj, path):
    services = need_list(r, obj, path, 'services')
    if services is None:
        return
    numbers, ids = [], set()
    for i, e in enumerate(services):
        p = '%s.services[%d]' % (path, i) if path else 'services[%d]' % i
        sid, number = check_service_entry(r, e, p) or (None, None)
        if sid is not None:
            if sid in ids:
                r.error(p + '.service_id', 'duplicate %s' % sid)
            ids.add(sid)
        if number is not None:
            if number in numbers:
                r.error(p + '.number', 'duplicate number %d' % number)
            elif numbers and number < numbers[-1]:
                r.error(p + '.number', 'not sorted by number (%d after %d)' % (number, numbers[-1]))
            numbers.append(number)


def check_mw_services(r, obj):
    if need_object(r, obj, ''):
        check_service_list(r, obj, '')


def check_mw_service_info(r, obj):
    if not need_object(r, obj, ''):
        return
    check_service_entry(r, obj, '')
    need(r, obj, '', 'description', lambda v: isinstance(v, str), 'a string')
    need(r, obj, '', 'stream_control_url', lambda v: isinstance(v, str) and v.startswith('http://'),
         'an http:// URL')
    need(r, obj, '', 'archive_window_s', lambda v: is_int(v) and v > 0, 'a positive integer')


def check_epg(r, obj, path, with_range):
    """EPG response of Section 3.2.3; `with_range` = false for the result of get_epg (3.6.2)."""
    pre = path + '.' if path else ''
    server_time = need(r, obj, path, 'server_time', is_int, 'an integer')
    frm = to = None
    if with_range:
        frm = need(r, obj, path, 'from', is_int, 'an integer')
        to = need(r, obj, path, 'to', is_int, 'an integer')
        if frm is None or to is None:
            frm = to = None
        elif not (frm < to and to - frm <= EPG_MAX_RANGE_S):
            r.error(pre + 'from', 'range must satisfy from < to and to - from <= %d' % EPG_MAX_RANGE_S)
            frm = to = None
    services = need_list(r, obj, path, 'services')
    if services is None:
        return
    event_ids = set()
    for i, s in enumerate(services):
        p = '%sservices[%d]' % (pre, i)
        if not need_object(r, s, p):
            continue
        sid = need(r, s, p, 'service_id', lambda v: isinstance(v, str) and SERVICE_ID.fullmatch(v),
                   'a string "svc-<n>"')
        events = need_list(r, s, p, 'events')
        if events is None:
            continue
        previous_end = None
        for j, e in enumerate(events):
            q = '%s.events[%d]' % (p, j)
            if not need_object(r, e, q):
                continue
            eid = need(r, e, q, 'event_id', is_text, 'a non-empty string')
            start = need(r, e, q, 'start', is_int, 'an integer')
            duration = need(r, e, q, 'duration', lambda v: is_int(v) and v > 0, 'a positive integer')
            need(r, e, q, 'title', is_text, 'a non-empty string')
            need(r, e, q, 'description', lambda v: isinstance(v, str), 'a string')
            in_archive = need(r, e, q, 'in_archive', lambda v: isinstance(v, bool), 'a boolean')
            if eid is not None:
                if eid in event_ids:
                    r.error(q + '.event_id', 'duplicate %s' % eid)
                event_ids.add(eid)
            if start is None:
                continue
            if start % EPG_GRID_S != 0:
                r.error(q + '.start', '%d is not a multiple of %d' % (start, EPG_GRID_S))
            if sid is not None and eid is not None and eid != '%s-%d' % (sid, start):
                r.error(q + '.event_id', 'must be "%s-%d", is "%s"' % (sid, start, eid))
            if server_time is not None and in_archive is not None:
                expected = server_time - ARCHIVE_WINDOW_S <= start <= server_time
                if in_archive != expected:
                    r.error(q + '.in_archive', 'must be %s for start %d and server_time %d'
                            % (json.dumps(expected), start, server_time))
            if duration is None:
                continue
            if previous_end is not None and start < previous_end:
                r.error(q + '.start', 'not sorted or overlapping the previous event (ends %d)' % previous_end)
            previous_end = start + duration
            if frm is not None and not (start < to and start + duration > frm):
                r.error(q, 'does not overlap the range [%d, %d)' % (frm, to))


def check_mw_epg(r, obj):
    if need_object(r, obj, ''):
        check_epg(r, obj, '', True)


def check_error_object(r, err, path, codes):
    if not need_object(r, err, path):
        return None
    code = need(r, err, path, 'code', lambda v: v in codes, 'one of %s' % sorted(codes))
    need(r, err, path, 'message', lambda v: isinstance(v, str), 'a string')
    return code


def http_error(codes):
    def check(r, obj):
        if need_object(r, obj, '') and need(r, obj, '', 'error', lambda v: True, '') is not None:
            check_error_object(r, obj['error'], 'error', codes)
    return check


# ---------------------------------------------------------------- IF-EXT-04 (Section 3.3)

def check_he_start_request(r, obj):
    if not need_object(r, obj, ''):
        return
    need(r, obj, '', 'service_id', lambda v: isinstance(v, str) and SERVICE_ID.fullmatch(v), 'a string "svc-<n>"')
    need(r, obj, '', 'start', start_point, '"live" or an integer')
    need(r, obj, '', 'client_host', is_text, 'a non-empty string')
    need(r, obj, '', 'client_port', lambda v: is_int(v) and 1 <= v <= 65535, 'an integer 1 - 65535')
    need(r, obj, '', 'ssrc', uint32, 'an unsigned 32-bit integer')


def check_he_session_request(r, obj):
    if need_object(r, obj, ''):
        need(r, obj, '', 'session_id', is_text, 'a non-empty string')


def check_he_start(r, obj):
    if not need_object(r, obj, ''):
        return
    need(r, obj, '', 'session_id', is_text, 'a non-empty string')
    need(r, obj, '', 'service_id', lambda v: isinstance(v, str) and SERVICE_ID.fullmatch(v), 'a string "svc-<n>"')
    need(r, obj, '', 'start_time', is_int, 'an integer')
    need(r, obj, '', 'ssrc', uint32, 'an unsigned 32-bit integer')
    need(r, obj, '', 'payload_type', lambda v: v == 33 and is_int(v), '33')


def check_he_stop(r, obj):
    if need_object(r, obj, ''):
        need(r, obj, '', 'stopped', lambda v: v is True, 'true')


def check_he_keepalive(r, obj):
    if need_object(r, obj, ''):
        need(r, obj, '', 'expires_in_s', lambda v: is_int(v) and v > 0, 'a positive integer')


# ---------------------------------------------------------------- IF-INT-01 (Section 3.6)

def check_session_state_data(r, d, path):
    if not need_object(r, d, path):
        return
    need(r, d, path, 'session', lambda v: is_int(v) and v >= 1, 'a positive integer')
    need(r, d, path, 'service_id', lambda v: isinstance(v, str) and SERVICE_ID.fullmatch(v), 'a string "svc-<n>"')
    state = need(r, d, path, 'state', lambda v: v in STATES, 'one of %s' % sorted(STATES))
    reason = need(r, d, path, 'reason', nullable(lambda v: isinstance(v, str)), 'a string or null')
    need(r, d, path, 'start', start_point, '"live" or an integer')
    need(r, d, path, 'start_time', nullable(is_int), 'an integer or null')
    need(r, d, path, 'playing_since', nullable(is_int), 'an integer or null')
    need(r, d, path, 'protected', nullable(lambda v: isinstance(v, bool)), 'a boolean or null')
    if 'error' not in d:
        r.error(path + '.error', 'missing')
    if state is None:
        return
    allowed = REASONS.get(state)
    if allowed is None and reason is not None:
        r.error(path + '.reason', 'must be null in state %s' % state)
    elif allowed is not None and reason not in allowed:
        r.error(path + '.reason', 'must be one of %s in state %s' % (sorted(allowed), state))
    err = d.get('error')
    if state == 'error':
        if err is None:
            r.error(path + '.error', 'must be an error object in state error')
        else:
            check_error_object(r, err, path + '.error', CODES_SESSION)
    elif 'error' in d and err is not None:
        r.error(path + '.error', 'must be null in state %s' % state)
    if state == 'playing':
        for name, what in (('playing_since', 'set'), ('start_time', 'set'), ('protected', 'decided')):
            if name in d and d[name] is None:
                r.error('%s.%s' % (path, name), 'must be %s in state playing' % what)
    if state == 'key_required' and 'protected' in d and d['protected'] is not True:
        r.error(path + '.protected', 'must be true in state key_required')


def check_stream_info_data(r, d, path):
    if not need_object(r, d, path):
        return
    pid = lambda v: is_int(v) and 0 <= v <= 0x1FFF
    need(r, d, path, 'session', lambda v: is_int(v) and v >= 1, 'a positive integer')
    need(r, d, path, 'service_id', lambda v: isinstance(v, str) and SERVICE_ID.fullmatch(v), 'a string "svc-<n>"')
    need(r, d, path, 'program_number', lambda v: is_int(v) and 1 <= v <= 0xFFFF, 'an integer 1 - 65535')
    need(r, d, path, 'pmt_pid', pid, 'a PID 0 - 8191')
    need(r, d, path, 'pcr_pid', pid, 'a PID 0 - 8191')
    for name, stream_type, codec, optional in (('video', 27, 'H.264', False), ('audio', 15, 'AAC', True)):
        if name not in d:
            r.error('%s.%s' % (path, name), 'missing')
            continue
        v = d[name]
        if v is None and optional:
            continue
        q = '%s.%s' % (path, name)
        if not need_object(r, v, q):
            continue
        need(r, v, q, 'pid', pid, 'a PID 0 - 8191')
        need(r, v, q, 'stream_type', lambda x, t=stream_type: is_int(x) and x == t, str(stream_type))
        need(r, v, q, 'codec', lambda x, c=codec: x == c, json.dumps(codec))


def check_event(r, m, path):
    name = need(r, m, path, 'name', lambda v: v in ('session_state', 'stream_info', 'mw_status'),
                'one of ["mw_status", "session_state", "stream_info"]')
    if 'data' not in m:
        r.error(path + '.data', 'missing')
        return name
    d = m['data']
    if name == 'session_state':
        check_session_state_data(r, d, path + '.data')
        if isinstance(d, dict) and d.get('state') == 'idle':
            r.error(path + '.data.state', 'idle is never sent as an event')
    elif name == 'stream_info':
        check_stream_info_data(r, d, path + '.data')
    elif name == 'mw_status' and need_object(r, d, path + '.data'):
        need(r, d, path + '.data', 'reachable', lambda v: isinstance(v, bool), 'a boolean')
    return name


def check_result(r, command, result, path):
    """Result of a successful response (Section 3.6.2)."""
    if not need_object(r, result, path):
        return
    if command == 'get_services':
        check_service_list(r, result, path)
    elif command == 'get_epg':
        check_epg(r, result, path, False)
    elif command == 'play':
        need(r, result, path, 'session', lambda v: is_int(v) and v >= 1, 'a positive integer')
    elif command == 'get_status':
        if result.get('state') != 'idle':
            check_session_state_data(r, result, path)


def check_response(r, m, path, command):
    """`command` is the name of the paired command, or None if it is not known."""
    need(r, m, path, 'id', is_int, 'an integer')
    ok = need(r, m, path, 'ok', lambda v: isinstance(v, bool), 'a boolean')
    if ok is True:
        if 'result' not in m:
            r.error(path + '.result', 'missing')
        elif command is not None:
            check_result(r, command, m['result'], path + '.result')
        else:
            need_object(r, m['result'], path + '.result')
    elif ok is False:
        if 'error' not in m:
            r.error(path + '.error', 'missing')
            return
        if command in COMMAND_CODES:
            codes = COMMAND_CODES[command] | {'INVALID_COMMAND'}
        elif command is not None:
            codes = {'INVALID_COMMAND'}   # unknown command name (Section 3.6.1)
        else:
            codes = CODES_UI
        check_error_object(r, m['error'], path + '.error', codes)


def check_ws_message(r, m, path='', command=None):
    """One message of the AL to the UI (Section 3.6.1); returns its type or None."""
    if not need_object(r, m, path):
        return None
    kind = need(r, m, path, 'type', lambda v: v in ('response', 'event'), '"response" or "event"')
    if kind == 'response':
        check_response(r, m, path, command)
    elif kind == 'event':
        check_event(r, m, path)
    return kind


def check_ws_recording(r, lines):
    commands = {}        # id -> command name (None if the name is not a string)
    answered = {}        # id -> number of responses
    last_session = None
    for n, line in enumerate(lines, 1):
        if not line.strip():
            continue
        path = 'line %d' % n
        try:
            rec = loads(line)
        except ValueError as e:
            r.error(path, 'not a recording line: %s' % e)
            continue
        if not isinstance(rec, dict) or rec.get('dir') not in ('sent', 'recv') or 'text' not in rec:
            r.error(path, 'not a recording line (needs "dir" and "text")')
            continue
        if rec['dir'] == 'sent':
            if rec.get('kind', 'text') != 'text':
                continue
            try:
                cmd = loads(rec['text'])
            except ValueError:
                continue   # not readable: no response expected (Section 3.6.1)
            if isinstance(cmd, dict) and is_int(cmd.get('id')):
                name = cmd.get('name')
                commands[cmd['id']] = name if isinstance(name, str) else None
                answered.setdefault(cmd['id'], 0)
            continue
        if rec.get('kind', 'text') != 'text':
            r.error(path, 'the AL sent a binary frame (Section 3.6: one JSON message per text frame)')
            continue
        try:
            m = loads(rec['text'])
        except ValueError as e:
            r.error(path, 'not valid JSON: %s' % e)
            continue
        command = None
        if isinstance(m, dict) and m.get('type') == 'response' and is_int(m.get('id')):
            mid = m['id']
            if commands:
                if mid not in commands:
                    r.error(path + '.id', 'response to %d, which was not sent' % mid)
                else:
                    answered[mid] += 1
                    if answered[mid] > 1:
                        r.error(path + '.id', 'second response to command %d' % mid)
                    command = commands[mid] or '<unknown>'
        check_ws_message(r, m, path, command)
        if command == 'play' and m.get('ok') is True and isinstance(m.get('result'), dict):
            s = m['result'].get('session')
            if is_int(s):
                if last_session is not None and s <= last_session:
                    r.error(path + '.result.session', 'session %d not higher than the previous %d' % (s, last_session))
                last_session = s
    for mid, count in sorted(answered.items()):
        if count == 0:
            r.error('', 'no response to command %d (%s)' % (mid, commands[mid]))


# ---------------------------------------------------------------- kinds and inputs

KINDS = {
    'mw_services': check_mw_services,
    'mw_service_info': check_mw_service_info,
    'mw_epg': check_mw_epg,
    'mw_error': http_error(CODES_MW),
    'he_start_request': check_he_start_request,
    'he_session_request': check_he_session_request,
    'he_start': check_he_start,
    'he_stop': check_he_stop,
    'he_keepalive': check_he_keepalive,
    'he_error': http_error(CODES_HE),
    'ws_message': lambda r, obj: check_ws_message(r, obj),
}


def check_text(kind, text, source):
    """Checks one input text as `kind`; returns the list of error lines."""
    r = Report(source)
    if kind == 'ws':
        check_ws_recording(r, text.splitlines())
        return r.errors
    try:
        obj = loads(text)
    except ValueError as e:
        r.error('', 'not valid JSON: %s' % e)
        return r.errors
    KINDS[kind](r, obj)
    return r.errors


def spec_examples(spec_text):
    """The JSON examples of Section 3 as (kind or None, text, source); None = not a receiver contract."""
    start = spec_text.index('\n## 3. Interface Specification')
    end = spec_text.index('\n## 4. Design')
    section = spec_text[start:end]
    offset = spec_text[:start].count('\n') + 1
    examples = []
    heading = ''
    lines = section.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('#'):
            heading = line.lstrip('#').strip()
        number = heading.split(' ', 1)[0]
        source = 'SPECIFICATION.md:%d (%s)' % (offset + i, number)
        if line.startswith('```') and line.strip() != '```json':
            i += 1
            while not lines[i].startswith('```'):   # another code block (C++ of 3.9.2)
                i += 1
        elif line.strip() == '```json':
            j = i + 1
            while lines[j].strip() != '```':
                j += 1
            text = '\n'.join(lines[i + 1:j])
            obj = loads(text)
            kind = {'3.2.1': 'mw_services', '3.2.2': 'mw_service_info', '3.2.3': 'mw_epg',
                    '3.6.3': 'ws_message'}.get(number)
            if number == '3.3.1':
                kind = 'he_start' if 'session_id' in obj else 'he_start_request'
            examples.append((kind, text, source))
            i = j
        elif number in ('3.3.2', '3.3.3') and line.startswith('Request:'):
            # Inline examples: Request: `{...}` → response `200`: `{...}`.
            spans = re.findall(r'`(\{[^`]*\})`', line)
            examples.append(('he_session_request', spans[0], source))
            examples.append(('he_stop' if number == '3.3.2' else 'he_keepalive', spans[1], source))
        i += 1
    return examples


def read_input(path):
    if path == '-':
        return sys.stdin.read()
    with open(path, encoding='utf-8') as f:
        return f.read()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--kind', choices=sorted(list(KINDS) + ['ws']), help='kind of every FILE')
    mode.add_argument('--spec-examples', action='store_true', help='check the JSON examples of Section 3')
    parser.add_argument('--spec', default=DEFAULT_SPEC, help='path of SPECIFICATION.md')
    parser.add_argument('files', nargs='*', metavar='FILE')
    args = parser.parse_args(argv)

    jobs = []
    try:
        if args.spec_examples:
            if args.files:
                parser.error('--spec-examples takes no FILE')
            for kind, text, source in spec_examples(read_input(args.spec)):
                if kind is None:
                    print('%s: skipped (not a contract of the receiver)' % source)
                else:
                    jobs.append((kind, text, source))
        else:
            if not args.files:
                parser.error('--kind needs at least one FILE')
            for path in args.files:
                jobs.append((args.kind, read_input(path), '<stdin>' if path == '-' else path))
    except (OSError, ValueError) as e:
        print('contract_check: %s' % e, file=sys.stderr)
        return 2

    errors = []
    for kind, text, source in jobs:
        errors += check_text(kind, text, source)
    for line in errors:
        print(line)
    print('checked %d input(s), %d error(s)' % (len(jobs), len(errors)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Log analysis of the acceptance measurements (Section 5.6.1): t_play, t_picture, t_reject, t_osd, t_gap.

Usage (Python standard library only, NFR-11), from the repository root:
  log_timing.py <interval> --limit-ms N [--min-within K] [--expect N]
                [--middleware-log logs/middleware.log] [--chromium-log logs/chromium.log]
                [--from YYYY-MM-DDTHH:MM:SS] [--to YYYY-MM-DDTHH:MM:SS] [--out FILE.csv]

Intervals (Section 5.6.1; all times from the one clock of the laptop, Section 5.1.8):
  t_play     middleware log: play received -> session_state playing sent for that session
  t_picture  DIAG key of the key press that caused the play -> DIAG frame of the new player
  t_reject   middleware log: first tuner Locked of the attempt -> session_state key_required key_rejected
  t_osd      DIAG key -> the next DIAG osd
  t_gap      DIAG wait -> the next DIAG resume

The middleware log has the line format of NFR-13 and the keywords of HANDOVER §5 ("recv command id=<n>
name=play", "send event session_state session=<n> state=<state> reason=<reason>", "tuner Locked
session=<n> generation=<g>", "key rejected ... session=<n> generation=<g>"). The Chromium log has the
DIAG lines of Section 5.1.8 ("DIAG <epoch_ms> <mark> <details>") inside its console messages.

Output: CSV with one row per repetition and a final row with the limit check (Section 5.6.1, MEASURE).
A repetition without an end mark (for example a play that never reached playing) has no value and
does not count as within the limit. The limit is met when at least --min-within repetitions (default:
all) are within --limit-ms and, with --expect, exactly that many repetitions were found.
Exit code 0 = limit met, 1 = not met, 2 = usage or input error.
"""

import argparse
import csv
import datetime
import re
import sys

INTERVALS = ('t_play', 't_picture', 't_reject', 't_osd', 't_gap')

LINE_TIME = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}) ')
PLAY = re.compile(r'\[AL-API\] recv command id=\d+ name=play (.*)$')
STATE = re.compile(r'\[AL-API\] send event session_state session=(\d+) state=(\w+) reason=(\S+)')
LOCKED = re.compile(r'\[AL-SES\] tuner Locked session=(\d+) generation=(\d+)')
REJECTED = re.compile(r'\[AL-DSC\] key rejected .*session=(\d+) generation=(\d+)')
DIAG = re.compile(r'DIAG (\d+) (\w+)(?: ([^"]*))?')


def local_ms(text):
    """Milliseconds since the epoch of a local ISO time of NFR-13 (YYYY-MM-DDTHH:MM:SS[.mmm])."""
    fmt = '%Y-%m-%dT%H:%M:%S.%f' if '.' in text else '%Y-%m-%dT%H:%M:%S'
    return round(datetime.datetime.strptime(text, fmt).timestamp() * 1000)


def iso(ms):
    return datetime.datetime.fromtimestamp(ms / 1000).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3]


def middleware_events(lines):
    """(t_ms, kind, data) of the middleware lines used here, in log order."""
    events = []
    for line in lines:
        m = LINE_TIME.match(line)
        if not m:
            continue
        t = local_ms(m.group(1))
        if (p := PLAY.search(line)):
            events.append((t, 'play', p.group(1)))
        elif (s := STATE.search(line)):
            events.append((t, 'state', (int(s.group(1)), s.group(2), s.group(3))))
        elif (lk := LOCKED.search(line)):
            events.append((t, 'locked', (int(lk.group(1)), int(lk.group(2)))))
        elif (r := REJECTED.search(line)):
            events.append((t, 'rejected', (int(r.group(1)), int(r.group(2)))))
    return events


def diag_marks(lines):
    """(t_ms, mark, details) of the DIAG lines, in log order."""
    marks = []
    for line in lines:
        m = DIAG.search(line)
        if m:
            marks.append((int(m.group(1)), m.group(2), m.group(3) or ''))
    return marks


def plays(events):
    """[(t_play, session, t_playing or None, note)]: each play, the session its response created (the
    first session_state starting of a new number after it; AL-API sends the response before the first
    event, T-01) and the first playing of that session before the next play. The note says when the
    session went through key_required before playing (the time then includes the key entry)."""
    result = []
    seen = set()
    starts = [i for i, e in enumerate(events) if e[1] == 'play']
    for k, i in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(events)
        session, playing, note = None, None, ''
        for t, kind, data in events[i + 1:end]:
            if kind != 'state':
                continue
            number, state, _ = data
            if session is None and state == 'starting' and number not in seen:
                session = number
                seen.add(number)
            elif session is not None and number == session and state == 'key_required':
                note = ' via key_required'
            elif session is not None and number == session and state == 'playing':
                playing = t
                break
        result.append((events[i][0], session, playing, note))
    return result


def t_play(events, marks):
    return [(tp, end, (f'session={s}' if s is not None else 'no new session') + note)
            for tp, s, end, note in plays(events)]


def t_picture(events, marks):
    rows = []
    for tp, session, _, note in plays(events):
        keys = [m for m in marks if m[1] == 'key' and m[0] <= tp]
        key = keys[-1] if keys else None
        frame = next((m for m in marks if m[1] == 'frame' and m[0] > tp
                      and m[2].startswith(f'/hls/{session}/')), None)
        start = key[0] if key else None
        detail = f'session={session} key={key[2] if key else "none"}{note}'
        rows.append((start, frame[0] if frame else None, detail))
    return rows


def t_reject(events, marks):
    rows = []
    for i, (t, kind, data) in enumerate(events):
        if kind != 'rejected':
            continue
        session, generation = data
        locked = next((e[0] for e in events if e[1] == 'locked' and e[2] == (session, generation)), None)
        sent = next((e[0] for e in events[i:] if e[1] == 'state' and e[2] == (session, 'key_required', 'key_rejected')),
                    None)
        rows.append((locked, sent, f'session={session} generation={generation}'))
    return rows


def pair_marks(marks, start_mark, end_mark, same_details):
    rows, used = [], set()
    for i, (t, mark, details) in enumerate(marks):
        if mark != start_mark:
            continue
        end = None
        for j in range(i + 1, len(marks)):
            t2, mark2, details2 = marks[j]
            if mark2 == end_mark and j not in used and (not same_details or details2 == details):
                end = t2
                used.add(j)
                break
        rows.append((t, end, f'{start_mark} {details}'.strip()))
    return rows


def t_osd(events, marks):
    return pair_marks(marks, 'key', 'osd', same_details=True)


def t_gap(events, marks):
    return pair_marks(marks, 'wait', 'resume', same_details=False)


COMPUTE = {'t_play': t_play, 't_picture': t_picture, 't_reject': t_reject, 't_osd': t_osd, 't_gap': t_gap}
NEEDS = {'t_play': ('m',), 't_picture': ('m', 'c'), 't_reject': ('m',), 't_osd': ('c',), 't_gap': ('c',)}


def evaluate(rows, limit_ms, min_within=None, expect=None):
    """CSV rows (header, one per repetition, final limit check) and whether the limit is met."""
    out = [['interval_row', 'start', 'end', 'value_ms', 'limit_ms', 'within', 'detail']]
    within = 0
    for n, (start, end, detail) in enumerate(rows, 1):
        value = end - start if start is not None and end is not None else None
        ok = value is not None and value <= limit_ms
        within += ok
        out.append([n, iso(start) if start is not None else '', iso(end) if end is not None else '',
                    '' if value is None else value, limit_ms, 'yes' if ok else 'no', detail])
    required = len(rows) if min_within is None else min_within
    met = within >= required and len(rows) > 0 and (expect is None or len(rows) == expect)
    values = [r[3] for r in out[1:] if r[3] != '']
    out.append(['limit check', '', '', max(values) if values else '', limit_ms,
                f'{within} of {len(rows)}',
                f'required {required}' + (f' of exactly {expect}' if expect is not None else '')
                + (': PASS' if met else ': FAIL')])
    return out, met


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read().splitlines()


def main(argv=None):
    ap = argparse.ArgumentParser(description='Timing intervals of Section 5.6.1 from the logs.')
    ap.add_argument('interval', choices=INTERVALS)
    ap.add_argument('--limit-ms', type=int, required=True)
    ap.add_argument('--min-within', type=int, help='repetitions that must be within the limit (default: all)')
    ap.add_argument('--expect', type=int, help='number of repetitions that must be found')
    ap.add_argument('--middleware-log', default='logs/middleware.log')
    ap.add_argument('--chromium-log', default='logs/chromium.log')
    ap.add_argument('--from', dest='frm', help='first time taken into account (local, YYYY-MM-DDTHH:MM:SS)')
    ap.add_argument('--to', help='last time taken into account (local, YYYY-MM-DDTHH:MM:SS)')
    ap.add_argument('--out', help='CSV file (default: standard output)')
    a = ap.parse_args(argv)
    try:
        lo = local_ms(a.frm) if a.frm else None
        hi = local_ms(a.to) if a.to else None
        events = middleware_events(read(a.middleware_log)) if 'm' in NEEDS[a.interval] else []
        marks = diag_marks(read(a.chromium_log)) if 'c' in NEEDS[a.interval] else []
    except (OSError, ValueError) as e:
        print(f'log_timing: {e}', file=sys.stderr)
        return 2
    inside = lambda t: (lo is None or t >= lo) and (hi is None or t <= hi)
    events = [e for e in events if inside(e[0])]
    marks = [m for m in marks if inside(m[0])]
    rows, met = evaluate(COMPUTE[a.interval](events, marks), a.limit_ms, a.min_within, a.expect)
    table = [['interval' if i == 0 else a.interval] + r for i, r in enumerate(rows)]
    if a.out:
        with open(a.out, 'w', newline='') as f:
            csv.writer(f).writerows(table)
    else:
        csv.writer(sys.stdout).writerows(table)
    return 0 if met else 1


if __name__ == '__main__':
    sys.exit(main())

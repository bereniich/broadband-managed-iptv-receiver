#!/usr/bin/env python3
"""Self-tests of log_timing.py (Section 5.6.1).

Run with: python3 -m unittest discover -s tests/acceptance   (Python standard library only, NFR-11)

The test classes carry the ID of the first test of Section 5.6 that relies on the interval:
t_play and t_picture -> TST-701, t_reject -> TST-703, t_osd -> TST-705, t_gap -> TST-706. The logs are
built here in the formats of NFR-13 / HANDOVER §5 (middleware) and Section 5.1.8 (DIAG lines in the
console messages of Chromium); the expected values are the differences of the times written into
them, i.e. the definitions of the intervals in Section 5.6.1.
"""

import csv
import datetime
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import log_timing  # noqa: E402

T0 = 1795878000000   # epoch ms of the first line of every test log (2026-11-28, local time of the test machine)


def mw(offset_ms, text):
    """One middleware log line (NFR-13) at T0 + offset_ms."""
    t = datetime.datetime.fromtimestamp((T0 + offset_ms) / 1000).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3]
    return f'{t} INFO {text}'


def diag(offset_ms, mark):
    """One Chromium console line with a DIAG mark (Section 5.1.8) at T0 + offset_ms."""
    return (f'[1:1:1128/160000.000000:INFO:CONSOLE:30] "DIAG {T0 + offset_ms} {mark}", '
            f'source: http://127.0.0.1:8090/js/main.js (30)')


def state(offset_ms, session, st, reason='null'):
    return mw(offset_ms, f'[AL-API] send event session_state session={session} state={st} reason={reason} '
                         f'start=live start_time=null')


# Two zaps (sessions 1 and 2) and a third play (session 3) that is replaced before it reaches playing.
MIDDLEWARE_ZAPS = [
    mw(0, '[AL-API] recv command id=4 name=play service_id=svc-1 start=live'),
    state(1, 1, 'starting'), state(5, 1, 'waiting_stream'), state(2010, 1, 'playing'),
    mw(10300, '[AL-API] recv command id=5 name=play service_id=svc-2 start=live'),
    state(10301, 1, 'stopped'), state(10302, 2, 'starting'), state(10306, 2, 'waiting_stream'),
    state(13400, 2, 'playing'),
    mw(20300, '[AL-API] recv command id=6 name=play service_id=svc-1 start=live'),
    state(20301, 2, 'stopped'), state(20302, 3, 'starting'),
    mw(20800, '[AL-API] recv command id=7 name=play service_id=svc-2 start=live'),
    state(20801, 3, 'stopped'), state(20802, 4, 'starting'), state(22900, 4, 'playing'),
]
CHROMIUM_ZAPS = [
    diag(-50, 'key UP'), diag(-49, 'osd UP'), diag(2400, 'frame /hls/1/index.m3u8'), diag(2401, 'frag 0'),
    diag(10000, 'key DOWN'), diag(10001, 'osd DOWN'), diag(16200, 'frame /hls/2/index.m3u8'),
    diag(20000, 'key UP'), diag(20500, 'key DOWN'), diag(23500, 'frame /hls/4/index.m3u8'),
]


class Tst701TPlayAndPicture(unittest.TestCase):
    def test_t_play_per_play(self):   # play received -> playing of its session
        rows = log_timing.t_play(log_timing.middleware_events(MIDDLEWARE_ZAPS), [])
        values = [(e - s) if e is not None else None for s, e, _ in rows]
        self.assertEqual(values, [2010, 3100, None, 2100])
        self.assertEqual([d for _, _, d in rows], ['session=1', 'session=2', 'session=3', 'session=4'])

    def test_t_picture_from_the_key_that_caused_the_play(self):   # DIAG key -> DIAG frame of the new player
        events = log_timing.middleware_events(MIDDLEWARE_ZAPS)
        rows = log_timing.t_picture(events, log_timing.diag_marks(CHROMIUM_ZAPS))
        values = [(e - s) if e is not None and s is not None else None for s, e, _ in rows]
        # zap 1: UP at -50, frame at 2400; zap 2: DOWN at 10000, frame at 16200; session 3 never had a frame;
        # session 4: the last key before its play is DOWN at 20500, frame at 23500.
        self.assertEqual(values, [2450, 6200, None, 3000])

    def test_play_through_key_required_is_marked(self):   # the time then includes the key entry
        lines = [mw(0, '[AL-API] recv command id=6 name=play service_id=svc-3 start=live'),
                 state(1, 1, 'starting'), state(80, 1, 'key_required', 'no_key'),
                 state(60000, 1, 'starting'), state(62000, 1, 'playing')]
        rows = log_timing.t_play(log_timing.middleware_events(lines), [])
        self.assertEqual([(e - s, d) for s, e, d in rows], [(62000, 'session=1 via key_required')])

    def test_limit_check_19_of_20(self):   # NFR-01: at least 19 of 20 within the limit
        rows = [(0, 2000, '')] * 19 + [(0, 3500, '')]
        out, met = log_timing.evaluate(rows, 3000, min_within=19, expect=20)
        self.assertTrue(met)
        self.assertEqual(out[-1][5], '19 of 20')
        self.assertTrue(out[-1][6].endswith('PASS'))
        out, met = log_timing.evaluate(rows[:18] + [(0, 3500, '')] * 2, 3000, min_within=19, expect=20)
        self.assertFalse(met)
        out, met = log_timing.evaluate(rows[:19], 3000, min_within=19, expect=20)   # one zap missing
        self.assertFalse(met)

    def test_missing_end_is_not_within(self):
        out, met = log_timing.evaluate([(0, None, ''), (0, 100, '')], 3000)
        self.assertFalse(met)
        self.assertEqual(out[1][3:6], ['', 3000, 'no'])
        self.assertEqual(out[2][3:6], [100, 3000, 'yes'])

    def test_command_line_csv_and_time_window(self):
        with tempfile.TemporaryDirectory() as d:
            m, c = os.path.join(d, 'middleware.log'), os.path.join(d, 'chromium.log')
            open(m, 'w').write('\n'.join(MIDDLEWARE_ZAPS) + '\n')
            open(c, 'w').write('\n'.join(CHROMIUM_ZAPS) + '\n')
            frm = datetime.datetime.fromtimestamp((T0 + 10000) / 1000).strftime('%Y-%m-%dT%H:%M:%S')
            buf = io.StringIO()
            with redirect_stdout(buf):
                code = log_timing.main(['t_play', '--limit-ms', '3000', '--middleware-log', m,
                                        '--chromium-log', c, '--from', frm])
            rows = list(csv.reader(io.StringIO(buf.getvalue())))
        self.assertEqual(rows[0][:3], ['interval', 'interval_row', 'start'])
        self.assertEqual([r[4] for r in rows[1:-1]], ['3100', '', '2100'])   # plays from T0 + 10 s on
        self.assertEqual(rows[-1][1], 'limit check')
        self.assertEqual(rows[-1][6], '1 of 3')
        self.assertEqual(code, 1)


class Tst703TReject(unittest.TestCase):
    def test_from_the_first_locked_of_the_attempt(self):   # first Locked of the attempt -> key_rejected
        lines = [
            mw(0, '[AL-API] recv command id=6 name=play service_id=svc-3 start=1795878300'),
            state(1, 2, 'starting'),
            mw(5, '[AL-SES] tuner Locked session=2 generation=2'),
            state(80, 2, 'key_required', 'no_key'),
            mw(30000, '[AL-API] recv command id=7 name=set_key key=0011… service_id=svc-3'),
            mw(30002, '[AL-SES] tuner Locked session=2 generation=3'),
            mw(32000, '[AL-SES] tuner Locked session=2 generation=3'),   # a later Locked of the same attempt
            mw(30090, '[AL-DSC] key rejected service=svc-3 key=0011… session=2 generation=3'),
            state(30091, 2, 'key_required', 'key_rejected'),
        ]
        rows = log_timing.t_reject(log_timing.middleware_events(lines), [])
        self.assertEqual([(e - s) for s, e, _ in rows], [89])
        self.assertEqual(rows[0][2], 'session=2 generation=3')


class Tst705TOsd(unittest.TestCase):
    def test_each_key_with_its_own_osd(self):   # DIAG key -> the next DIAG osd of that key
        marks = log_timing.diag_marks([diag(0, 'key E'), diag(16, 'osd E'), diag(1000, 'key •'),
                                       diag(1003, 'key •'), diag(1005, 'osd •'), diag(1009, 'osd •'),
                                       diag(2000, 'key I')])
        rows = log_timing.t_osd([], marks)
        self.assertEqual([(e - s) if e is not None else None for s, e, _ in rows], [16, 5, 6, None])


class Tst706TGap(unittest.TestCase):
    def test_wait_to_the_next_resume(self):   # DIAG wait -> the next DIAG resume
        marks = log_timing.diag_marks([diag(0, 'wait'), diag(285, 'resume'), diag(5000, 'frag 7'),
                                       diag(9000, 'wait'), diag(10706, 'resume')])
        rows = log_timing.t_gap([], marks)
        self.assertEqual([(e - s) for s, e, _ in rows], [285, 1706])
        out, met = log_timing.evaluate(rows, 1000)
        self.assertFalse(met)   # NFR-06: every t_gap at most 1 000 ms


if __name__ == '__main__':
    unittest.main()

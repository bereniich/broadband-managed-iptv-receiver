#!/usr/bin/env python3
"""Self-tests of random_keys.py and random_keys.txt (TST-719, Section 5.6.2).

Run with: python3 -m unittest discover -s tests/acceptance   (Python standard library only, NFR-11)

Expected values come from the text of TST-719: 300 keys, pseudo-random with seed 15, committed, made of
zap, digit, ENTER, ←, →, B, L, E, ESC, K and hexadecimal keys, sent within 60 s.
"""

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import random_keys  # noqa: E402


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


class Tst719RandomKeys(unittest.TestCase):
    def test_committed_file(self):   # 300 keys of the kinds named by TST-719
        keys = random_keys.read_keys(random_keys.DEFAULT_FILE)
        self.assertEqual(len(keys), 300)
        kinds = {'zap': {'Up', 'Down'}, 'digit': {str(d) for d in range(10)}, 'ENTER': {'Return'},
                 '←': {'Left'}, '→': {'Right'}, 'B': {'b'}, 'L': {'l'}, 'E': {'e'}, 'ESC': {'Escape'},
                 'K': {'k'}, 'hexadecimal': {'a', 'b', 'c', 'd', 'e', 'f'}}
        for kind, names in kinds.items():
            self.assertTrue(set(keys) & names, f'no {kind} key in the file')
        self.assertTrue(set(keys) <= set().union(*kinds.values()))

    def test_seed_15_gives_the_committed_file(self):   # the same sequence in every run
        self.assertEqual(random_keys.generate(seed=15, count=300),
                         random_keys.read_keys(random_keys.DEFAULT_FILE))
        self.assertNotEqual(random_keys.generate(seed=16, count=300), random_keys.generate(seed=15, count=300))

    def test_unknown_key_is_refused(self):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write('Up\nF5\n')
        try:
            with self.assertRaises(ValueError):
                random_keys.read_keys(f.name)
        finally:
            os.unlink(f.name)

    def test_sent_within_60_s(self):   # xdotool: window activated once, then one key command per key
        keys = random_keys.read_keys(random_keys.DEFAULT_FILE)
        clock, commands, log = FakeClock(), [], io.StringIO()
        elapsed = random_keys.send(keys, 60, commands.append, '1234', log,
                                   clock=clock.now, wall=clock.now, sleep=clock.sleep)
        self.assertEqual(commands[0], ['xdotool', 'windowactivate', '--sync', '1234'])
        self.assertEqual([c[-1] for c in commands[1:]], keys)
        self.assertTrue(all(c[:3] == ['xdotool', 'key', '--clearmodifiers'] for c in commands[1:]))
        self.assertLessEqual(elapsed, 60)
        self.assertAlmostEqual(elapsed, 59.8)   # 300 keys 0.2 s apart, the first at 0 s
        self.assertEqual(len(log.getvalue().splitlines()), 300)

    def test_dry_run_prints_the_plan(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = random_keys.main(['send', '--dry-run'])
        lines = buf.getvalue().splitlines()
        self.assertEqual(code, 0)
        self.assertEqual(len(lines), 300)
        self.assertTrue(lines[-1].lstrip().startswith('59.800 s'))


if __name__ == '__main__':
    unittest.main()

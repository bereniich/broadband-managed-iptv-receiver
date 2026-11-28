#!/usr/bin/env python3
"""Self-tests of sample_proc.py (Section 5.6.1).

Run with: python3 -m unittest discover -s tests/acceptance   (Python standard library only, NFR-11)

The test classes carry the ID of the first test of Section 5.6 that relies on the value: VmRSS growth
-> TST-706, CPU use -> TST-717. Expected values come from proc(5) (the fields of /proc/<pid>/status
and /proc/<pid>/stat) and from the formulas of Section 5.6.1 and NFR-06 / NFR-15, applied by hand to
the samples built here.
"""

import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sample_proc  # noqa: E402

STATUS = 'Name:\tdtv-middleware\nState:\tS (sleeping)\nVmPeak:\t  210000 kB\nVmRSS:\t   48212 kB\nThreads:\t6\n'
# Fields 3 - 17 after the command name; utime (14) = 120, stime (15) = 30.
STAT = '4242 (dtv middle) ware) S 1 4242 4242 0 -1 4194560 900 0 0 0 120 30 0 0 20 0 6 0 1000 214958080 12053'


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


class Tst706Memory(unittest.TestCase):
    def test_vmrss_from_status(self):   # proc(5): "VmRSS: <n> kB"
        self.assertEqual(sample_proc.read_rss_kb(STATUS), 48212)

    def test_growth_from_minute_1(self):   # NFR-06: max of minutes 1 - end minus the value at minute 1
        rss = [40000, 45000, 46000, 47000, 48000, 49000, 50000, 52000, 51000]   # samples at 0, 15, ... 120 s
        samples = [(0.0, 15.0 * i, kb, 0) for i, kb in enumerate(rss)]
        out, met = sample_proc.evaluate(samples, True, 100, rss_growth_limit_mb=20)
        self.assertTrue(met)
        self.assertEqual(out[-1][3], 52000 - 48000)   # minute 1 = the sample at 60 s
        out, met = sample_proc.evaluate(samples, True, 100, rss_growth_limit_mb=3)
        self.assertFalse(met)   # 4000 kB > 3 MB

    def test_process_ended(self):   # TST-706: no process terminates
        clock = FakeClock()
        reads = iter([(1000, 0), (1000, 10), None])
        samples, alive = sample_proc.sample(lambda: next(reads), 100, 10, clock=clock.now, wall=clock.now,
                                            sleep=clock.sleep)
        self.assertFalse(alive)
        self.assertEqual([s[1] for s in samples], [0.0, 10.0])
        out, met = sample_proc.evaluate(samples, alive, 100)
        self.assertFalse(met)

    def test_samples_every_interval(self):   # Section 5.6.1: every 10 s
        clock = FakeClock()
        samples, alive = sample_proc.sample(lambda: (1, 2), 30, 10, clock=clock.now, wall=clock.now,
                                            sleep=clock.sleep)
        self.assertTrue(alive)
        self.assertEqual([s[1] for s in samples], [0.0, 10.0, 20.0, 30.0])


class Tst717Cpu(unittest.TestCase):
    def test_ticks_after_the_command_name(self):   # proc(5): utime field 14, stime field 15
        self.assertEqual(sample_proc.read_cpu_ticks(STAT), 150)

    def test_cpu_of_an_interval(self):   # (Δ(utime + stime) / ticks per s) / Δ wall time
        self.assertAlmostEqual(sample_proc.cpu_percent((0, 0.0, 0, 1000), (0, 10.0, 0, 1250), 100), 25.0)

    def test_windows_of_10_minutes(self):   # NFR-15: average of each 10-min interval at most 25 %
        # 100 ticks/s: 20 % of one core = 200 ticks per 10 s in the first 600 s, then 30 % = 300 ticks.
        samples, ticks = [], 0
        for i in range(121):   # 0 ... 1200 s every 10 s
            samples.append((0.0, 10.0 * i, 1000, ticks))
            ticks += 200 if i < 60 else 300
        out, met = sample_proc.evaluate(samples, True, 100, cpu_limit_percent=25, cpu_window_s=600)
        windows = [r for r in out if str(r[0]).startswith('cpu window')]
        self.assertEqual([r[4] for r in windows], ['20.0', '30.0'])
        self.assertFalse(met)

    def test_late_sample_stays_in_its_window(self):   # the sample planned at 30 s is taken at 30.004 s
        samples = [(0.0, 0.0, 1000, 0), (0.0, 15.002, 1000, 150), (0.0, 30.004, 1000, 300)]
        out, met = sample_proc.evaluate(samples, True, 100, cpu_limit_percent=25, cpu_window_s=30)
        windows = [r for r in out if str(r[0]).startswith('cpu window')]
        self.assertEqual([(r[2], r[4]) for r in windows], [('0-30', '10.0')])
        self.assertTrue(met)

    def test_real_process(self):   # reads /proc of a child that is busy for about 1 s, then ends
        child = subprocess.Popen([sys.executable, '-c',
                                  'import time\nt = time.time()\nwhile time.time() - t < 1.0: pass'])
        try:
            samples, alive = sample_proc.sample(sample_proc.ProcReader(child.pid), 5, 0.25)
        finally:
            child.wait()
        self.assertFalse(alive)   # the child ended before the 5 s
        self.assertGreaterEqual(len(samples), 2)
        self.assertGreater(samples[0][2], 0)   # VmRSS in kB
        cpu = sample_proc.cpu_percent(samples[0], samples[-1], os.sysconf('SC_CLK_TCK'))
        self.assertGreater(cpu, 30.0)   # a busy loop uses most of one core


if __name__ == '__main__':
    unittest.main()

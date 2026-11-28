#!/usr/bin/env python3
"""Memory and CPU samples of the middleware process P2 (Section 5.6.1; NFR-06, NFR-15).

Usage (Python standard library only, NFR-11), from the repository root while the system runs:
  sample_proc.py --duration-s S [--pid N | --name dtv-middleware] [--interval-s 10]
                 [--rss-growth-limit-mb 20] [--cpu-limit-percent 25 --cpu-window-s 600] [--out FILE.csv]

Every --interval-s seconds (default 10 s, Section 5.6.1) it reads VmRSS from /proc/<pid>/status and
utime + stime from /proc/<pid>/stat. The CPU use of an interval is (Δ(utime + stime) / clock ticks per
second) / Δ wall time, in percent of one core (Section 5.6.1).

Output: CSV with one row per sample (time, seconds since the start, VmRSS in kB, CPU of the interval
since the previous sample) and final rows with the limit checks that were asked for:
  --rss-growth-limit-mb M   NFR-06 / TST-706, TST-707: VmRSS maximum of minutes 1 - end minus VmRSS at
                            minute 1 (the first sample at or after 60 s) at most M MB (1 MB = 1024 kB)
  --cpu-limit-percent P     NFR-15 / TST-717: average CPU use in each window of --cpu-window-s seconds
                            (default 600 s, from the start) at most P % of one core
If the process ends during the run, a row says so and the run fails (TST-706: "no process terminates").
Exit code 0 = every requested limit met, 1 = not met, 2 = usage error or process not found.
"""

import argparse
import csv
import datetime
import os
import subprocess
import sys
import time


def read_rss_kb(status_text):
    """VmRSS in kB from the text of /proc/<pid>/status."""
    for line in status_text.splitlines():
        if line.startswith('VmRSS:'):
            return int(line.split()[1])
    raise ValueError('no VmRSS line')


def read_cpu_ticks(stat_text):
    """utime + stime (fields 14 and 15 of proc(5)) from the text of /proc/<pid>/stat. The command name
    (field 2) is in parentheses and may contain spaces, so the fields are counted after the last ')'."""
    fields = stat_text[stat_text.rindex(')') + 2:].split()
    return int(fields[11]) + int(fields[12])   # fields 14 and 15; the list starts at field 3


class ProcReader:
    """Reads one sample of a process from /proc; returns None when the process does not exist."""

    def __init__(self, pid):
        self.pid = pid

    def __call__(self):
        try:
            with open(f'/proc/{self.pid}/status') as f:
                rss = read_rss_kb(f.read())
            with open(f'/proc/{self.pid}/stat') as f:
                ticks = read_cpu_ticks(f.read())
        except (OSError, ValueError):
            return None
        return rss, ticks


def sample(read, duration_s, interval_s, clock=time.monotonic, wall=time.time, sleep=time.sleep):
    """[(wall time, seconds since start, rss_kb, ticks)] every interval_s until duration_s; stops at
    the first None of read() (process ended) and then returns (samples, False)."""
    start = clock()
    samples = []
    k = 0
    while True:
        s = read()
        if s is None:
            return samples, False
        samples.append((wall(), clock() - start, s[0], s[1]))
        k += 1
        if k * interval_s > duration_s:
            return samples, True
        delay = start + k * interval_s - clock()
        if delay > 0:
            sleep(delay)


def cpu_percent(prev, cur, ticks_per_s):
    """CPU use between two samples in percent of one core."""
    dt = cur[1] - prev[1]
    return 100.0 * (cur[3] - prev[3]) / ticks_per_s / dt if dt > 0 else 0.0


def evaluate(samples, alive, ticks_per_s, rss_growth_limit_mb=None, cpu_limit_percent=None, cpu_window_s=600):
    """CSV rows (header, one per sample, final checks) and whether every requested limit is met."""
    out = [['row', 'time', 'elapsed_s', 'vmrss_kb', 'cpu_percent', 'detail']]
    for n, s in enumerate(samples):
        cpu = '' if n == 0 else f'{cpu_percent(samples[n - 1], s, ticks_per_s):.1f}'
        out.append([n + 1, datetime.datetime.fromtimestamp(s[0]).strftime('%Y-%m-%dT%H:%M:%S'),
                    f'{s[1]:.1f}', s[2], cpu, ''])
    met = alive and len(samples) >= 2
    if not alive:
        out.append(['process', '', '', '', '', 'the process ended during the run: FAIL'])
    if rss_growth_limit_mb is not None:
        base = next((s for s in samples if s[1] >= 60), None)
        if base is None:
            out.append(['rss growth', '', '', '', '', 'no sample at or after minute 1: FAIL'])
            met = False
        else:
            later = [s[2] for s in samples if s[1] >= base[1]]
            growth = max(later) - base[2]
            ok = growth <= rss_growth_limit_mb * 1024
            met = met and ok
            out.append(['rss growth', '', '', growth, '',
                        f'max {max(later)} kB - minute 1 {base[2]} kB = {growth / 1024:.1f} MB, '
                        f'limit {rss_growth_limit_mb} MB: {"PASS" if ok else "FAIL"}'])
    if cpu_limit_percent is not None:
        first = samples[0][1] if samples else 0
        k = 0
        while samples:
            lo, hi = first + k * cpu_window_s, first + (k + 1) * cpu_window_s
            # A sample is taken a few ms after its planned time; 1 s of tolerance keeps it in its window.
            window = [s for s in samples if lo - 1 <= s[1] <= hi + 1]
            if len(window) < 2:
                break
            avg = cpu_percent(window[0], window[-1], ticks_per_s)
            ok = avg <= cpu_limit_percent
            met = met and ok
            out.append([f'cpu window {k + 1}', '', f'{window[0][1]:.0f}-{window[-1][1]:.0f}', '', f'{avg:.1f}',
                        f'average of the window, limit {cpu_limit_percent} %: {"PASS" if ok else "FAIL"}'])
            k += 1
            if window[-1] is samples[-1]:
                break
    return out, met


def find_pid(name):
    r = subprocess.run(['pgrep', '-x', name], capture_output=True, text=True)
    pids = [int(p) for p in r.stdout.split()]
    if len(pids) != 1:
        raise ValueError(f'{len(pids)} processes named {name} (expected 1)')
    return pids[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description='VmRSS and CPU samples of P2 (Section 5.6.1).')
    who = ap.add_mutually_exclusive_group()
    who.add_argument('--pid', type=int)
    who.add_argument('--name', default='dtv-middleware', help='process name for pgrep -x (default dtv-middleware)')
    ap.add_argument('--duration-s', type=float, required=True)
    ap.add_argument('--interval-s', type=float, default=10.0)
    ap.add_argument('--rss-growth-limit-mb', type=float)
    ap.add_argument('--cpu-limit-percent', type=float)
    ap.add_argument('--cpu-window-s', type=float, default=600.0)
    ap.add_argument('--out', help='CSV file (default: standard output)')
    a = ap.parse_args(argv)
    try:
        pid = a.pid if a.pid is not None else find_pid(a.name)
    except (OSError, ValueError) as e:
        print(f'sample_proc: {e}', file=sys.stderr)
        return 2
    read = ProcReader(pid)
    if read() is None:
        print(f'sample_proc: no process {pid}', file=sys.stderr)
        return 2
    samples, alive = sample(read, a.duration_s, a.interval_s)
    rows, met = evaluate(samples, alive, os.sysconf('SC_CLK_TCK'), a.rss_growth_limit_mb,
                         a.cpu_limit_percent, a.cpu_window_s)
    if a.out:
        with open(a.out, 'w', newline='') as f:
            csv.writer(f).writerows(rows)
    else:
        csv.writer(sys.stdout).writerows(rows)
    return 0 if met else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Random key sequence of TST-719 (Section 5.6.2; HLR-10, REQ-56, REQ-07, REQ-39) and its xdotool runner.

Usage (Python standard library only, NFR-11), from the repository root:
  random_keys.py generate [--out tests/acceptance/random_keys.txt]
  random_keys.py send [--keys tests/acceptance/random_keys.txt] [--seconds 60]
                      [--window-name "M15 IPTV Receiver"] [--log logs/random_keys_sent.txt] [--dry-run]

generate  writes the 300 keys of TST-719, one xdotool key name per line: a pseudo-random sequence of
          random.Random(15) over the keys named by TST-719 (zap Up / Down, the digits, Return, Left,
          Right, b, l, e, Escape, k and the hexadecimal letters). The file is committed, so every run
          uses the same sequence; generating it again must give the same file.
send      finds the Chromium window by its title (the <title> of webui/index.html), activates it and
          sends the keys with xdotool at equal intervals, so that all of them are sent within --seconds
          (TST-719: 300 keys within 60 s). Every key is logged with its epoch time in ms, so that the run
          can be compared with the DIAG key marks of logs/chromium.log; the log is a .txt file, because
          TST-715 checks every line of logs/*.log (except chromium.log) against the format of NFR-13.
          --dry-run only prints the plan.
          xdotool needs an X11 session ("Ubuntu on Xorg"; HANDOVER §10): under Wayland it cannot reach
          the Chromium window.
Exit code 0 = all keys sent within the time, 1 = not, 2 = usage error, no window or no xdotool.
"""

import argparse
import os
import random
import subprocess
import sys
import time

SEED = 15
COUNT = 300
# The keys named by TST-719, as xdotool key names: zap, digits, ENTER, ←, →, B, L, E, ESC, K and the
# hexadecimal keys (the digits and b, e are already in the list; a, c, d, f are added).
KEYS = (['Up', 'Down'] + [str(d) for d in range(10)]
        + ['Return', 'Left', 'Right', 'b', 'l', 'e', 'Escape', 'k', 'a', 'c', 'd', 'f'])
DEFAULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'random_keys.txt')


def generate(seed=SEED, count=COUNT):
    rng = random.Random(seed)
    return [rng.choice(KEYS) for _ in range(count)]


def read_keys(path):
    with open(path, encoding='utf-8') as f:
        keys = [line.strip() for line in f if line.strip()]
    unknown = sorted(set(keys) - set(KEYS))
    if unknown:
        raise ValueError(f'{path}: unknown keys {unknown}')
    return keys


def plan(count, seconds):
    """Offsets in seconds of the keys: equal steps, the last one before `seconds`."""
    step = seconds / count
    return [i * step for i in range(count)]


def send(keys, seconds, run, window, log, clock=time.monotonic, wall=time.time, sleep=time.sleep):
    """Sends the keys at the offsets of plan(); run(args) runs one xdotool command. Returns the time
    from the first to the last key in seconds."""
    run(['xdotool', 'windowactivate', '--sync', window])
    start = clock()
    for key, offset in zip(keys, plan(len(keys), seconds)):
        delay = start + offset - clock()
        if delay > 0:
            sleep(delay)
        run(['xdotool', 'key', '--clearmodifiers', key])
        log.write(f'{round(wall() * 1000)} {key}\n')
    return clock() - start


def find_window(name):
    r = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', name], capture_output=True, text=True)
    ids = r.stdout.split()
    if not ids:
        raise RuntimeError(f'no visible window named "{name}" (X11 session and scripts/start.sh needed)')
    return ids[-1]


def main(argv=None):
    ap = argparse.ArgumentParser(description='Random key sequence of TST-719 and its xdotool runner.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('generate')
    g.add_argument('--out', default=DEFAULT_FILE)
    s = sub.add_parser('send')
    s.add_argument('--keys', default=DEFAULT_FILE)
    s.add_argument('--seconds', type=float, default=60.0)
    s.add_argument('--window-name', default='M15 IPTV Receiver')
    s.add_argument('--log', default='logs/random_keys_sent.txt')
    s.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)

    if a.cmd == 'generate':
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write('\n'.join(generate()) + '\n')
        return 0
    try:
        keys = read_keys(a.keys)
    except (OSError, ValueError) as e:
        print(f'random_keys: {e}', file=sys.stderr)
        return 2
    if a.dry_run:
        for key, offset in zip(keys, plan(len(keys), a.seconds)):
            print(f'{offset:7.3f} s  {key}')
        return 0
    try:
        window = find_window(a.window_name)
    except (OSError, RuntimeError) as e:
        print(f'random_keys: {e}', file=sys.stderr)
        return 2
    os.makedirs(os.path.dirname(a.log) or '.', exist_ok=True)
    with open(a.log, 'w', encoding='utf-8') as log:
        elapsed = send(keys, a.seconds, lambda args: subprocess.run(args, check=True), window, log)
    ok = elapsed <= a.seconds
    print(f'random_keys: {len(keys)} keys sent in {elapsed:.1f} s (limit {a.seconds:.0f} s): '
          f'{"PASS" if ok else "FAIL"}; log {a.log}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

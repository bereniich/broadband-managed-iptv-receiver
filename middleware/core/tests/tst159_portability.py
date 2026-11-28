#!/usr/bin/env python3
"""TST-159 (Section 5.3.2): portability check of the AL library dtvcore (NFR-09, rule R3).

Usage (Python standard library only, NFR-11), from any directory:
  python3 middleware/core/tests/tst159_portability.py [--build-dir DIR] [--keep] [--jobs N]

Steps and expected results of TST-159:
  1. Build the target dtvcore_tests in an empty build directory (a new temporary directory, or
     DIR, which must not exist or be empty) and run it: it builds and all its tests pass.
  2. List its link libraries and include directories with the CMake file API (codemodel-v2):
     dtvcore_tests depends only on dtvcore and links only dtvcore and GoogleTest; dtvcore links
     nothing. The include directories are middleware/include/ (which holds only pal/, the
     IF-INT-04 headers) and third_party/nlohmann/, besides the own directories of the AL
     (middleware/core/ and middleware/core/tests/); the C++ standard library and the GoogleTest
     headers come from the compiler's system directories and are not listed by CMake.
  3. Search middleware/core/ (case-insensitive) for the four forbidden patterns of TST-159: the
     crypto library, the HTTP / WebSocket library, the socket header and the HAL directory: no match.

The patterns of step 3 are written in pieces below, so that this file, which lies in
middleware/core/tests/, does not match its own search.

Output: one line per check, "TST-159 step <n>: PASS|FAIL ...", and finally "TST-159 passed" or
"TST-159 failed". Exit code 0 = passed, 1 = failed, 2 = the check could not be run.
"""

import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
CORE = os.path.join(ROOT, 'middleware', 'core')

# Step 3: the regular expression of TST-159, step 3 (written in pieces, see above).
FORBIDDEN = re.compile('open' 'ssl' '|' 'mon' 'goose' '|' 'sys/' 'socket\\.h' '|' 'ha' 'l/', re.IGNORECASE)

# Step 2: what dtvcore_tests may link (TST-159: "links only dtvcore and GoogleTest").
LINK_DTVCORE = re.compile(r'libdtvcore\.a')
LINK_GTEST = re.compile(r'(lib)?gtest(_main)?(\.a|\.so[.0-9]*)?|-lgtest(_main)?')

INCLUDE_EXTERNAL = [os.path.join(ROOT, 'middleware', 'include'),
                    os.path.join(ROOT, 'third_party', 'nlohmann')]
INCLUDE_OWN = [CORE, os.path.join(CORE, 'tests')]


class Result:
    def __init__(self):
        self.failed = False

    def check(self, step, ok, text):
        print('TST-159 step %d: %s %s' % (step, 'PASS' if ok else 'FAIL', text))
        self.failed |= not ok
        return ok


def first_line(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
        return out.splitlines()[0] if out else '?'
    except OSError:
        return '?'


def header(build_dir):
    commit = first_line(['git', '-C', ROOT, 'rev-parse', 'HEAD'])
    dirty = subprocess.run(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no'],
                           capture_output=True, text=True).stdout.strip()
    os_name = '?'
    if os.path.exists('/etc/os-release'):
        with open('/etc/os-release', encoding='utf-8') as f:
            for line in f:
                if line.startswith('PRETTY_NAME='):
                    os_name = line.split('=', 1)[1].strip().strip('"')
    print('TST-159 portability check (Section 5.3.2; NFR-09, R3)')
    print('command:     python3 middleware/core/tests/tst159_portability.py')
    print('commit:      %s%s' % (commit, ' (with local changes)' if dirty else ''))
    print('environment: %s; %s; %s; Python %s' % (os_name, first_line(['cmake', '--version']),
                                                  first_line(['g++', '--version']), platform.python_version()))
    print('build dir:   %s (empty at the start)' % build_dir)


def run_logged(cmd, cwd=None):
    """Runs a command; returns (exit code, output)."""
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout


def tail(text, n=20):
    return '\n'.join('    ' + l for l in text.splitlines()[-n:])


# ---------------------------------------------------------------- step 1

def step1(res, build_dir, jobs):
    query = os.path.join(build_dir, '.cmake', 'api', 'v1', 'query')
    os.makedirs(query)
    open(os.path.join(query, 'codemodel-v2'), 'w').close()
    code, out = run_logged(['cmake', '-S', os.path.join(ROOT, 'middleware'), '-B', build_dir,
                            '-DCMAKE_BUILD_TYPE=Release'])
    if not res.check(1, code == 0, 'cmake configure of middleware/ in the empty build directory'):
        print(tail(out))
        return False
    code, out = run_logged(['cmake', '--build', build_dir, '--target', 'dtvcore_tests', '-j', str(jobs)])
    warnings = [l for l in out.splitlines() if 'warning:' in l]
    if not res.check(1, code == 0, 'build of the target dtvcore_tests (%d compiler warning(s))' % len(warnings)):
        print(tail(out))
        return False
    exe = os.path.join(build_dir, 'bin', 'dtvcore_tests')
    code, out = run_logged([exe, '--gtest_list_tests'], cwd=ROOT)
    listed = sum(1 for l in out.splitlines() if l.startswith('  ') and l.strip())
    code, out = run_logged([exe, '--gtest_color=no'], cwd=ROOT)
    m = re.search(r'^\[  PASSED  \] (\d+) tests?\.', out, re.M)
    passed = int(m.group(1)) if m else 0
    failed_lines = [l for l in out.splitlines() if l.startswith('[  FAILED  ]')]
    ok = code == 0 and passed == listed and listed > 0 and not failed_lines
    res.check(1, ok, 'dtvcore_tests: %d of %d tests passed (exit code %d)' % (passed, listed, code))
    if not ok:
        print(tail('\n'.join(failed_lines) or out))
    return True


# ---------------------------------------------------------------- step 2

def codemodel_targets(build_dir):
    reply = os.path.join(build_dir, '.cmake', 'api', 'v1', 'reply')
    index = json.load(open(sorted(glob.glob(os.path.join(reply, 'index-*.json')))[-1], encoding='utf-8'))
    cm = index['reply']['codemodel-v2']
    model = json.load(open(os.path.join(reply, cm['jsonFile']), encoding='utf-8'))
    targets = model['configurations'][0]['targets']
    names = {t['id']: t['name'] for t in targets}
    detail = {t['name']: json.load(open(os.path.join(reply, t['jsonFile']), encoding='utf-8')) for t in targets}
    return names, detail


def libraries(target):
    return [f['fragment'] for f in target.get('link', {}).get('commandFragments', [])
            if f.get('role') == 'libraries' and f['fragment'].strip()]


def includes(target):
    return sorted({os.path.realpath(i['path']) for cg in target.get('compileGroups', [])
                   for i in cg.get('includes', [])})


def rel(path):
    return os.path.relpath(path, ROOT) + '/'


def step2(res, build_dir):
    names, detail = codemodel_targets(build_dir)
    tests, core = detail.get('dtvcore_tests'), detail.get('dtvcore')
    if not res.check(2, tests is not None and core is not None, 'CMake file API lists dtvcore_tests and dtvcore'):
        return
    deps = sorted(names.get(d['id'], d['id']) for d in tests.get('dependencies', []))
    res.check(2, deps == ['dtvcore'], 'dtvcore_tests depends on targets %s (expected: dtvcore only)' % deps)
    core_deps = sorted(names.get(d['id'], d['id']) for d in core.get('dependencies', []))
    res.check(2, core.get('type') == 'STATIC_LIBRARY' and not core_deps and not libraries(core),
              'dtvcore: %s, depends on targets %s, links %s (expected: nothing)'
              % (core.get('type'), core_deps, libraries(core)))
    libs = libraries(tests)
    shown = [os.path.basename(l) for l in libs]
    other = [l for l in libs if not (LINK_DTVCORE.fullmatch(os.path.basename(l))
                                     or LINK_GTEST.fullmatch(os.path.basename(l)))]
    has_core = any(LINK_DTVCORE.fullmatch(os.path.basename(l)) for l in libs)
    has_gtest = any(LINK_GTEST.fullmatch(os.path.basename(l)) for l in libs)
    res.check(2, has_core and has_gtest and not other,
              'dtvcore_tests links %s (expected: only dtvcore and GoogleTest)%s'
              % (shown, ' -- not allowed: %s' % other if other else ''))
    allowed = INCLUDE_EXTERNAL + INCLUDE_OWN
    for name, target in (('dtvcore', core), ('dtvcore_tests', tests)):
        found = includes(target)
        bad = [p for p in found if p not in allowed]
        res.check(2, not bad, '%s include directories %s%s' % (
            name, [rel(p) for p in found], ' -- not allowed: %s' % [rel(p) for p in bad] if bad else ''))
    include_dir = INCLUDE_EXTERNAL[0]
    files = sorted(os.path.relpath(os.path.join(d, f), include_dir)
                   for d, _, fs in os.walk(include_dir) for f in fs)
    outside = [f for f in files if not f.startswith('pal' + os.sep)]
    res.check(2, files and not outside, 'middleware/include/ holds only pal/: %s%s'
              % (files, ' -- outside pal/: %s' % outside if outside else ''))


# ---------------------------------------------------------------- step 3

def step3(res):
    matches = []
    count = 0
    for d, dirs, files in os.walk(CORE):
        dirs[:] = sorted(x for x in dirs if x != '__pycache__')
        for f in sorted(files):
            path = os.path.join(d, f)
            count += 1
            with open(path, encoding='utf-8', errors='replace') as fh:
                for n, line in enumerate(fh, 1):
                    if FORBIDDEN.search(line):
                        matches.append('%s:%d: %s' % (os.path.relpath(path, ROOT), n, line.strip()[:120]))
    res.check(3, not matches, 'search of middleware/core/ (%d files) for the four patterns of TST-159: '
              '%d match(es)' % (count, len(matches)))
    for m in matches[:20]:
        print('    ' + m)


def main():
    parser = argparse.ArgumentParser(description='TST-159 portability check of dtvcore (Section 5.3.2).')
    parser.add_argument('--build-dir', help='empty or missing directory (default: a new temporary one)')
    parser.add_argument('--keep', action='store_true', help='keep the build directory')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 2, help='parallel build jobs')
    args = parser.parse_args()

    if args.build_dir:
        build_dir = os.path.abspath(args.build_dir)
        if os.path.exists(build_dir) and os.listdir(build_dir):
            print('TST-159: the build directory %s is not empty' % build_dir, file=sys.stderr)
            return 2
        os.makedirs(build_dir, exist_ok=True)
    else:
        build_dir = tempfile.mkdtemp(prefix='tst159-')
    if not shutil.which('cmake'):
        print('TST-159: cmake not found', file=sys.stderr)
        return 2

    res = Result()
    try:
        header(build_dir)
        if step1(res, build_dir, args.jobs):
            step2(res, build_dir)
        step3(res)
    finally:
        if not args.keep:
            shutil.rmtree(build_dir, ignore_errors=True)
    print('TST-159 %s' % ('failed' if res.failed else 'passed'))
    return 1 if res.failed else 0


if __name__ == '__main__':
    sys.exit(main())

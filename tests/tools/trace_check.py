#!/usr/bin/env python3
"""Traceability generator and checker of SPECIFICATION.md (Sections 5.7 and 5.9.2).

Section 5.7 is generated from the columns "Verifies" and "Anchors" of the test tables of
Sections 5.3 - 5.6. Section 5.9.2 is recomputed from the results of Section 5.9.3 (Section 5.9.1):
a requirement counts as verified when every test listed for it in Section 5.7 has the status
"passed" and all of them were run on the same commit (Section 5.1.5, "in the same version of the
code").

Usage (Python standard library only, NFR-11):
  trace_check.py [--spec FILE]            print the generated Section 5.7
  trace_check.py [--spec FILE] --summary  print the recomputed Section 5.9.2
  trace_check.py [--spec FILE] --check    compare both sections with the file
  trace_check.py [--spec FILE] --write    replace both sections in the file

Every detailed requirement, user story, transition, error and buffer without a test is listed on
standard error ("missing: [...]"). The exit code is 1 if something is missing, if --check finds a
difference or if the results table of Section 5.9.3 is inconsistent; otherwise 0.
"""

import argparse
import difflib
import os
import re
import sys

DEFAULT_SPEC = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'SPECIFICATION.md'))

H_57 = '### 5.7 Traceability'
H_58 = '### 5.8 Defense'
H_592 = '#### 5.9.2 Summary'
H_593 = '#### 5.9.3 Results'

INTRO_57 = (
    'The tables of this section are generated from the columns "Verifies" and "Anchors" of the '
    'test tables of Sections 5.3 – 5.6 by `tests/tools/trace_check.py`; a range such as '
    '`REQ-33 – REQ-36` counts for every requirement in it. The script fails if a detailed '
    'requirement, user story, transition, error or buffer of Sections 2 – 4 has no test. It is run '
    'after every change of a test table, and the tables below are replaced with its output, so '
    'that this section never differs from the test definitions. The first column is printed in '
    'bold so that it is not taken for the definition of the ID by the consistency check. Columns: '
    '**U** unit tests (Section 5.3), **I** integration tests (5.4), **A** acceptance, '
    'non-functional and robustness tests (5.5, 5.6); "Verif." repeats the verification method of '
    'Section 2.\n'
    '\n'
    'A high-level requirement is verified when all detailed requirements refined from it are '
    'verified (Section 2; traceability in Section 2.3.14); Section 5.7.3 lists in addition the '
    'tests that refer to a high-level requirement directly.')

STATUSES = ('passed', 'failed', 'not executed', 'not verifiable')
LEVELS = ('Unit', 'Integration', 'Acceptance')


def tst_number(tid):
    return int(tid[4:])


def level_of(tid):
    n = tst_number(tid)
    return 'Unit' if n < 500 else ('Integration' if n < 600 else 'Acceptance')


def key_of(prefix, i):
    return f'B{i}' if prefix == 'B' else f'{prefix}-{i:02d}'


class Spec:
    def __init__(self, text):
        self.text = text
        # Definitions of Sections 1 - 5.6; Sections 5.7 and 5.9 only repeat IDs in bold.
        self.defs = text[:text.index(H_57)]
        self.tests = re.findall(r'^\| (TST-\d+) \| ([^|]*) \| ([^|]*) \|', self.defs, re.M)
        self.verif = {k: v.strip()
                      for p in ('REQ', 'NFR')
                      for k, v in re.findall(rf'^\| ({p}-\d+) \|.*\| ([^|]+) \|\s*$', self.defs, re.M)}

    def defined(self, prefix):
        """Numbers of the IDs defined in Sections 1 - 5.6, as the consistency check counts them."""
        if prefix == 'US':
            found = re.findall(r'^\*\*US-(\d+) ', self.defs, re.M)
        elif prefix == 'B':
            found = re.findall(r'^\| B(\d+) \|', self.defs, re.M)
        else:
            found = re.findall(rf'^\| {prefix}-(\d+) \|', self.defs, re.M)
        return sorted({int(x) for x in found})

    @staticmethod
    def ids(cell, prefix):
        """IDs named in a table cell, including ranges such as `REQ-33 – REQ-36`."""
        single = r'\bB(\d+)\b' if prefix == 'B' else rf'\b{prefix}-(\d+)\b'
        ranged = r'\bB(\d+) – B(\d+)' if prefix == 'B' else rf'\b{prefix}-(\d+) – {prefix}-(\d+)'
        out = {int(x) for x in re.findall(single, cell)}
        for a, b in re.findall(ranged, cell):
            out |= set(range(int(a), int(b) + 1))
        return out

    def tests_for(self, prefix, columns):
        """Map ID number -> set of test IDs, from "Verifies" ('v') or "Verifies" + "Anchors" ('va')."""
        result = {}
        for tid, verifies, anchors in self.tests:
            cell = verifies if columns == 'v' else verifies + ' ' + anchors
            for i in self.ids(cell, prefix):
                result.setdefault(i, set()).add(tid)
        return result


def generate_57(spec, missing):
    def joined(tests, level=None):
        chosen = sorted((t for t in tests if level is None or level_of(t) == level), key=tst_number)
        return ', '.join(chosen) if chosen else '—'

    out = [H_57, '', INTRO_57, '']
    for prefix, title in (('REQ', '#### 5.7.1 Detailed functional requirements → tests'),
                          ('NFR', '#### 5.7.2 Non-functional requirements → tests')):
        mapping = spec.tests_for(prefix, 'v')
        out += [title, '', '| Requirement | Verif. | U | I | A |', '|---|---|---|---|---|']
        for i in spec.defined(prefix):
            k = key_of(prefix, i)
            tests = mapping.get(i, set())
            if not tests:
                missing.append(k)
            out.append(f'| **{k}** | {spec.verif[k]} | {joined(tests, "Unit")} '
                       f'| {joined(tests, "Integration")} | {joined(tests, "Acceptance")} |')
        out.append('')

    out += ['#### 5.7.3 High-level requirements and user stories → tests', '',
            '| High-level requirement | Tests referring to it directly |', '|---|---|']
    mapping = spec.tests_for('HLR', 'v')
    for i in spec.defined('HLR'):
        tests = mapping.get(i, set())
        text = joined(tests) if tests else '— (through its detailed requirements, Section 2.3.14)'
        out.append(f'| **{key_of("HLR", i)}** | {text} |')
    out += ['', '| User story | Tests |', '|---|---|']
    mapping = spec.tests_for('US', 'v')
    for i in spec.defined('US'):
        tests = mapping.get(i, set())
        if not tests:
            missing.append(key_of('US', i))
        out.append(f'| **{key_of("US", i)}** | {joined(tests)} |')

    out += ['', '#### 5.7.4 Design elements → tests', '']
    for prefix, name, intro in (('T', 'Transition', 'Transitions of Section 4.3.4:'),
                                ('ER', 'Error', 'Errors of Section 4.8.2:'),
                                ('B', 'Buffer', 'Buffers of Section 4.5.4:')):
        mapping = spec.tests_for(prefix, 'va')
        out += [intro, '', f'| {name} | Tests |', '|---|---|']
        for i in spec.defined(prefix):
            tests = mapping.get(i, set())
            if not tests:
                missing.append(key_of(prefix, i))
            out.append(f'| **{key_of(prefix, i)}** | {joined(tests)} |')
        out.append('')
    return '\n'.join(out)


def parse_results(spec, errors):
    """Rows of Section 5.9.3: test ID -> (level, status, commit, rest of the row)."""
    results = spec.text[spec.text.index(H_593):]
    rows = {}
    for tid, level, status, _date, commit, rest in re.findall(
            r'^\| \*\*(TST-\d+)\*\* \| ([^|]+?) \| ([^|]+?) \| ([^|]+?) \| ([^|]+?) \| (.*)$',
            results, re.M):
        if tid in rows:
            errors.append(f'{tid}: more than one row in Section 5.9.3')
        if level != level_of(tid):
            errors.append(f'{tid}: level "{level}", expected "{level_of(tid)}"')
        if status not in STATUSES:
            errors.append(f'{tid}: unknown status "{status}"')
        if status == 'failed' and not re.search(r'#\d+', rest):
            errors.append(f'{tid}: status "failed" without a GitHub issue (#<n>)')
        rows[tid] = (level, status, commit, rest)
    defined = {tid for tid, _, _ in spec.tests}
    for tid in sorted(defined - rows.keys(), key=tst_number):
        errors.append(f'{tid}: no row in Section 5.9.3')
    for tid in sorted(rows.keys() - defined, key=tst_number):
        errors.append(f'{tid}: row in Section 5.9.3 but no test definition')
    return rows


def generate_592(spec, errors, warnings):
    rows = parse_results(spec, errors)
    counts = {lvl: dict.fromkeys(STATUSES, 0) for lvl in LEVELS}
    for tid, (_level, status, _commit, _rest) in rows.items():
        if status in STATUSES:
            counts[level_of(tid)][status] += 1

    out = [H_592, '', '| Level | Tests | passed | failed | not executed | not verifiable |',
           '|---|---|---|---|---|---|']
    for lvl in LEVELS + ('**Total**',):
        if lvl == '**Total**':
            c = {s: sum(counts[x][s] for x in LEVELS) for s in STATUSES}
        else:
            c = counts[lvl]
        out.append(f'| {lvl} | {sum(c.values())} | ' + ' | '.join(str(c[s]) for s in STATUSES) + ' |')

    verified = {}
    for prefix in ('REQ', 'NFR'):
        mapping = spec.tests_for(prefix, 'v')
        verified[prefix] = []
        for i in spec.defined(prefix):
            tests = mapping.get(i, set())
            if not tests or any(t not in rows or rows[t][1] != 'passed' for t in tests):
                continue
            commits = {rows[t][2] for t in tests}
            if len(commits) == 1:
                verified[prefix].append(key_of(prefix, i))
            else:
                warnings.append(f'{key_of(prefix, i)}: all tests passed, but on different commits '
                                f'({", ".join(sorted(commits))}); not counted as verified')
    out += ['', f'Requirements verified: {len(verified["REQ"])} of {len(spec.defined("REQ"))} '
                f'detailed functional requirements, {len(verified["NFR"])} of '
                f'{len(spec.defined("NFR"))} non-functional requirements.']
    if verified['REQ'] or verified['NFR']:
        out += ['', 'Verified requirements: ' + ', '.join(verified['REQ'] + verified['NFR']) + '.']
    out.append('')
    return '\n'.join(out)


def current(text, start, end):
    return text[text.index(start):text.index(end)]


def report_diff(name, old, new):
    diff = list(difflib.unified_diff(old.rstrip('\n').splitlines(), new.rstrip('\n').splitlines(),
                                     f'{name} in the specification', f'{name} generated',
                                     lineterm='', n=0))
    for line in diff[:80]:
        sys.stderr.write(line + '\n')
    if len(diff) > 80:
        sys.stderr.write(f'... {len(diff) - 80} more lines\n')
    return bool(diff)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--spec', default=DEFAULT_SPEC, help='path of SPECIFICATION.md')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--summary', action='store_true', help='print the recomputed Section 5.9.2')
    mode.add_argument('--check', action='store_true', help='compare Sections 5.7 and 5.9.2 with the file')
    mode.add_argument('--write', action='store_true', help='replace Sections 5.7 and 5.9.2 in the file')
    args = parser.parse_args(argv)

    with open(args.spec, encoding='utf-8') as f:
        text = f.read()
    for heading in (H_57, H_58, H_592, H_593):
        if heading not in text:
            sys.stderr.write(f'heading not found: {heading}\n')
            return 1
    spec = Spec(text)

    missing, errors, warnings = [], [], []
    new_57 = generate_57(spec, missing)
    new_592 = generate_592(spec, errors, warnings)
    sys.stderr.write('missing: %s\n' % missing)
    for line in errors:
        sys.stderr.write(f'error: {line}\n')
    for line in warnings:
        sys.stderr.write(f'warning: {line}\n')
    failed = bool(missing or errors)

    if args.check:
        failed |= report_diff('Section 5.7', current(text, H_57, H_58), new_57)
        failed |= report_diff('Section 5.9.2', current(text, H_592, H_593), new_592)
        sys.stderr.write('check: %s\n' % ('FAILED' if failed else 'OK'))
    elif args.write:
        text = text.replace(current(text, H_57, H_58), new_57.rstrip('\n') + '\n\n', 1)
        text = text.replace(current(text, H_592, H_593), new_592.rstrip('\n') + '\n\n', 1)
        with open(args.spec, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    elif args.summary:
        sys.stdout.write(new_592)
    else:
        sys.stdout.write(new_57)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

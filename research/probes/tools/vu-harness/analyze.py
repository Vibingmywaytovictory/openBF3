"""Apply the S5 decision rules to extracted runs (DESIGN.md section 10).

    python -I analyze.py <run-dir> [<run-dir> ...] [--items W1,W2,...] [--out <dir>] [--json]

Each run dir is a research/measurements/<run>/ folder written by extract.py (a P0 run among them supplies the P0.11
unit verdict). For every rule the data allows it prints the verdict, its numbers, the exact code change and a draft
SOURCES.md row; summary.json and sources-draft.md go to --out (default: the first non-P0 run dir). Rules: W1-W5 (+W6,
W7 reports), S1 (+S2 report), S3, M1, M2, T1 (needs a 30 Hz and a 60 Hz run of the same scenario).
Exit 0; 1 on a usage error (no readable run).
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'py'))

from mhlib import rules_modes, rules_soldier, rules_tick, rules_weapons  # noqa: E402
from mhlib.rundata import Run, p0_values  # noqa: E402

ITEMS = ['W1', 'W2', 'W3', 'W4', 'W5', 'W7', 'S1', 'S3', 'M1', 'M2', 'T1']
RULES = {
    'W1': rules_weapons.rule_w1, 'W2': rules_weapons.rule_w2, 'W3': rules_weapons.rule_w3, 'W4': rules_weapons.rule_w4,
    'W5': rules_weapons.rule_w5, 'W7': rules_weapons.rule_w7, 'S1': rules_soldier.rule_s1, 'S3': rules_soldier.rule_s3,
    'M1': rules_modes.rule_m1, 'M2': rules_modes.rule_m2, 'T1': rules_tick.rule_t1,
}
NEEDS = {'W1': 'WA', 'W2': 'WA', 'W3': 'WA', 'W4': 'WA', 'W5': 'WB', 'W7': 'WC', 'S1': 'SA', 'S3': 'SB', 'M1': 'MA',
         'M2': 'MB', 'T1': None}


def analyze(runs, items=None):
    """Run the rules; returns a list of Result objects in item order."""
    ctx = {'p0': p0_values([r for r in runs if r.sc == 'P0'] + [r for r in runs if r.sc != 'P0'])}
    scs = set(r.sc for r in runs)
    hzs = set(int(r.hz) for r in runs)
    out = []
    for item in items or ITEMS:
        need = NEEDS.get(item)
        if item == 'T1' and len(hzs) < 2:
            continue
        if need and need not in scs and not (item == 'W2' and 'SB' in scs):
            continue
        res = RULES[item](runs, ctx)
        res = res if isinstance(res, list) else [res]
        out.extend(res)
        if item == 'W3':
            for r in res:
                if r.item == 'W3' and r.variant in ('linear', 'exp'):
                    ctx['w3'] = (r.variant, r.numbers.get('param'))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description='S5 decision rules')
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--items', default=None)
    ap.add_argument('--out', default=None)
    ap.add_argument('--json', action='store_true', help='print the summary as JSON instead of text')
    a = ap.parse_args(argv)
    runs = []
    for p in a.runs:
        try:
            runs.append(Run.load(p))
        except (OSError, ValueError, KeyError) as exc:
            print('analyze: cannot read %s (%s)' % (p, type(exc).__name__))
    if not runs:
        return 1
    items = [x.strip() for x in a.items.split(',')] if a.items else None
    if items and any(i not in RULES for i in items):
        print('analyze: unknown item in --items (known: %s)' % ','.join(ITEMS))
        return 1
    results = analyze(runs, items)
    summary = {'runs': [r.id for r in runs], 'results': [r.to_dict() for r in results]}
    if a.json:
        print(json.dumps(summary, sort_keys=True, indent=1, default=str))
    else:
        for r in results:
            print(r.text())
    out = a.out or next((r.path for r in runs if r.sc != 'P0'), runs[0].path)
    if out:
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, 'summary.json'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(json.dumps(summary, sort_keys=True, indent=1, default=str) + '\n')
        rows = [s for r in results for s in r.source_rows]
        with open(os.path.join(out, 'sources-draft.md'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('# SOURCES draft (analyze.py; an agent reviews before research/measurements/SOURCES.md)\n\n')
            f.write('| id | quantity | value | unit | run | VU | Hz | n | statistic | consumer | oracle |\n')
            f.write('|---|---|---|---|---|---|---|---|---|---|---|\n')
            for s in rows:
                f.write(s + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())

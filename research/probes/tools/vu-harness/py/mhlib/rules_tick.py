"""T1 tick coupling (DESIGN.md section 10): the same quantities from a 30 Hz and a 60 Hz run, ratio 60/30.

Informative: W2 decay slope, W3 recovery parameter, W4 t_rec, gravity. Not informative (VU changeset 20056 patched
them for high frequency): the S3 acceleration gains; they are reported as "VU-corrected, 30 Hz values are the oracle".
"""

import math

import numpy as np

from . import rules_soldier, rules_weapons, stats
from .sources import Result, fmt, no_data


def _ms(values):
    v = np.array([x for x in values if x is not None and math.isfinite(x)], dtype=float)
    if len(v) == 0:
        return None
    return float(np.median(v)), (float(v.std(ddof=1)) / math.sqrt(len(v)) if len(v) > 1 else 0.0), len(v)


def quantities(run):
    q = {}
    if run.sc == 'WA':
        sl = []
        for w in run.windows():
            s, _ = rules_weapons.decay_slopes(run, w)
            sl.extend(x['slope'] for x in s)
        q['W2 decay slope'] = _ms(sl)
        curves = []
        for w in run.windows():
            curves.extend(rules_weapons.recovery_curves(run, w))
        counted, wins, params = rules_weapons.classify_recovery(curves)
        if counted:
            law = max(wins, key=wins.get)
            q['W3 %s parameter' % law] = _ms(params[law])
    if run.sc == 'SB':
        r = rules_soldier.rule_s3([run])
        for res in r:
            if res.item == 'S3' and res.variant in ('fraction', 'linear'):
                steps = [s for s in res.numbers.get('steps', []) if str(s['cfg']).startswith('accel')]
                key = 'gain30' if res.variant == 'fraction' else 'a'
                q['S3 acceleration (%s)' % key] = _ms([s.get(key) for s in steps])
            if res.item == 'S3-gravity':
                q['gravity'] = _ms(res.numbers.get('g', []))
    return {k: v for k, v in q.items() if v}


NOT_INFORMATIVE = ('S3 acceleration',)


def rule_t1(runs, ctx=None):
    by = {}
    for r in runs:
        by.setdefault(r.sc, {}).setdefault(int(r.hz), []).append(r)
    rows = []
    for sc, hzs in sorted(by.items()):
        if 30 not in hzs or 60 not in hzs:
            continue
        q30, q60 = {}, {}
        for r in hzs[30]:
            q30.update(quantities(r))
        for r in hzs[60]:
            q60.update(quantities(r))
        for k in sorted(set(q30) & set(q60)):
            (m1, s1, n1), (m2, s2, n2) = q30[k], q60[k]
            if not m1:
                continue
            ratio = m2 / m1
            se = abs(ratio) * math.sqrt((s1 / m1) ** 2 + (s2 / m2) ** 2) if m2 else float('inf')
            equal = abs(ratio - 1) < max(3 * se, 0.05)
            rows.append({'sc': sc, 'q': k, 'v30': m1, 'v60': m2, 'ratio': ratio, 'se': se, 'equal': equal,
                         'informative': not k.startswith(NOT_INFORMATIVE)})
    if not rows:
        return no_data('T1', 'no matching 30 Hz / 60 Hz runs')
    coupled = sorted(set(r['q'] for r in rows if r['informative'] and not r['equal']))
    patched = sorted(set(r['q'] for r in rows if not r['informative'] and not r['equal']))
    nums = {'rows': rows, 'vu_patched_differ': patched}
    tags = []
    for c in coupled:
        tags.append('decay' if c.startswith('W2') else ('recovery' if c.startswith('W3') else c))
    if not coupled:
        txt = 'per-second rates equal at 30 and 60 Hz for every informative quantity (C5 holds, D4 stands)'
        if patched:
            txt += '; differ but VU-corrected (not informative): ' + ', '.join(patched)
        return Result('T1', txt, 'none' + ('+patched' if patched else ''), nums, 'no change')
    txt = 'tick-coupled: %s (ratios %s)' % (', '.join(coupled), ', '.join(
        '%s %s' % (r['q'], fmt(r['ratio'])) for r in rows if r['q'] in coupled))
    if patched:
        txt += '; VU-corrected (not informative): ' + ', '.join(patched)
    return Result('T1', txt, ','.join(sorted(tags)), nums,
                  'our 60 Hz sim must convert these per tick (MotorTuning::reference_hz; weapons recovery "per whole '
                  'tick", m2a-weapons 5.6)')

"""Mode rules M1 (ticket bleed) and M2 (capture) of DESIGN.md section 10. Planned consumer: the D12 functions (M4)."""

import math

import numpy as np

from . import stats
from .sources import Result, fmt, no_data, row as src_row


def _build(run):
    for m in run.metas:
        if isinstance(m.get('build'), int):
            return m['build']
    return run.meta.get('vu_build')


def model_T(n):
    return 20.0 / min(1 + 0.34 * (n - 1), 4)


def rule_m1(runs, ctx=None):
    mas = [r for r in runs if r.sc == 'MA']
    if not mas:
        return no_data('M1', 'no M-A run')
    states = []
    run0 = mas[0]
    for run in mas:
        for m in run.marks('hold'):
            k0, k1 = m.get('k0'), m.get('k1')
            if k0 is None or k1 is None:
                continue
            rows = [r for r in run.of('tk') if k0 <= r['k'] <= k1]
            if len(rows) < 10:
                continue
            for team in (1, 2):
                t, tf, cav = [], [], {1: [], 2: []}
                for r in rows:
                    for e in r['tm']:
                        if int(e[0]) == team:
                            t.append(r['k'] / run.hz / 60.0)
                            tf.append(e[1])
                        if int(e[0]) in (1, 2) and e[3] >= 0:
                            cav[int(e[0])].append(e[3])
                fit = stats.linfit(t, tf)
                if not fit:
                    continue
                own = stats.median(cav[team]) or 0.0
                enemy = stats.median(cav[3 - team]) or 0.0
                predA = 15.0 if (enemy > 100 and enemy > own) else 0.0
                states.append({'cfg': m.get('cfg'), 'team': team, 'rate': -fit['b'], 'ci95': 1.96 * fit['se_b'],
                               'own': own, 'enemy': enemy, 'lead': enemy - own, 'predA': predA, 'own_vec': m.get('own')})
    if not states:
        return no_data('M1', 'no hold windows with tickets')
    devA = [abs(s['rate'] - s['predA']) for s in states]
    nums = {'states': states, 'max_dev_A': max(devA)}
    if max(devA) <= 1.0:
        srow = src_row('M1', 'conquest ticket bleed rule', 'A', 'tickets/min', run0.id, _build(run0), run0.hz,
                       len(states), 'LSQ per hold window; max |rate - A| %s' % fmt(max(devA)),
                       'D12 bleed function (planned M4; CapturePointDef::area_value)', 'P 7.3 conquest row')
        return Result('M1', 'rule A: 15 tickets/min iff the enemy area > 100 and > own (max dev %s/min)' %
                      fmt(max(devA)), 'A', nums,
                      'D12 bleed function = rule A (planned M4, P 3.7 "Bleed rule"; inputs CapturePointDef::area_value)',
                      [srow])
    bl = [s for s in states if s['lead'] > 0]
    if bl:
        x = np.array([s['lead'] for s in bl])
        y = np.array([s['rate'] for s in bl])
        c = float((x * y).sum() / (x * x).sum())
        rms_lin = float(np.sqrt(np.mean((c * x - y) ** 2)))
        step = float(np.mean(y))
        rms_step = float(np.sqrt(np.mean((step - y) ** 2)))
        nums.update({'per_area': c, 'per_60_area': 60 * c, 'rms_linear': rms_lin, 'rms_step': rms_step})
        f = 'linear in the lead (%s tickets/min per 60 area)' % fmt(60 * c) if rms_lin <= rms_step else \
            'step (%s tickets/min when behind)' % fmt(step)
    else:
        f = 'no bleeding state'
    return Result('M1', 'rule B: the rate grows with the lead, f %s' % f, 'B', nums,
                  'D12 bleed function = rule B with the fitted f (planned M4)')


def rule_m2(runs, ctx=None):
    mbs = [r for r in runs if r.sc == 'MB']
    if not mbs:
        return no_data('M2', 'no M-B run')
    series = {'N': {}, 'FLIP': {}}
    contest, leave = [], []
    run0 = mbs[0]
    for run in mbs:
        ms = run.marks('series')
        for m in ms:
            x = m.get('x') or {}
            e = next((y for y in run.marks('series-end') if y['q'] > m['q']), None)
            if e is None or not (e.get('x') or {}).get('ok'):
                continue
            n, cp = x.get('n'), x.get('cp')
            rows = [r for r in run.of('cp', m['q'], e['q']) if r.get('cp') == cp]
            kn = next((r['k'] for r in rows if (r.get('n1') or 0) >= n), None)
            ev = [r for r in run.of('ev', m['q'], e['q']) if r.get('name') == 'Player:EnteredCapturePoint' and r.get('id2') == cp]
            if len(ev) >= n:
                kev = ev[n - 1]['k']
                kn = kev if kn is None else min(kn, kev)
            kc = next((r['k'] for r in rows if kn is not None and r['k'] >= kn and r.get('team') == 1 and r.get('ctrl')), None)
            if kn is None or kc is None:
                continue
            series[m.get('cfg')].setdefault(n, []).append((kc - kn) / run.hz)
        for m in run.marks('contest'):
            e = next((y for y in run.marks('contest-end') if y['q'] > m['q']), None)
            rows = run.of('cp', m['q'] + 1, e['q'] if e else None)
            ks = [r for r in rows if r['k'] > m['k'] + 2 * run.hz]
            if len(ks) >= 3:
                fit = stats.linfit([r['k'] / run.hz for r in ks], [r['loc'] for r in ks])
                if fit:
                    contest.append(fit['b'])
        for m in run.marks('leave'):
            rows = [r for r in run.of('cp', m['q'], None) if m['k'] + 1 <= r['k'] <= m['k'] + 20 * run.hz]
            rows = [r for r in rows if 0 < r['loc'] < 100]
            if len(rows) >= 3:
                fit = stats.linfit([r['k'] / run.hz for r in rows], [r['loc'] for r in rows])
                if fit:
                    leave.append(fit['b'])
    T = {n: float(np.median(v)) for n, v in sorted(series['N'].items())}
    F = {n: float(np.median(v)) for n, v in sorted(series['FLIP'].items())}
    nums = {'T': T, 'T_model': {n: model_T(n) for n in T}, 'flip': F, 'contest_slopes': contest, 'leave_slopes': leave}
    out = []
    if len(T) < 2:
        out.append(Result('M2', 'inconclusive: %d attacker counts measured' % len(T), 'inconclusive', nums))
        return out
    dev = {n: T[n] / model_T(n) - 1 for n in T}
    nums['dev'] = dev
    if all(abs(d) <= 0.05 for d in dev.values()):
        v, txt = 'model', 'model holds: T(n) = 20 / min(1 + 0.34 (n-1), 4) within 5 %'
        ch = ('D12 capture function: time_to_get_control 20 s, added_multiplier_per_player 0.34, '
              'max_capture_multiplier 4 (planned M4, mode_def.h CapturePointDef)')
    else:
        # fit T = 20 / (1 + m (n-1))
        xs = np.array([n - 1 for n in T if n > 1], dtype=float)
        ys = np.array([20.0 / T[n] - 1 for n in T if n > 1])
        m = float((xs * ys).sum() / (xs * xs).sum()) if len(xs) else float('nan')
        nums['multiplier'] = m
        if abs(m - 1) <= 0.05:
            v, txt = 'linear_n', 'capture time 20 / n (per-player multiplier %s)' % fmt(m)
        else:
            v, txt = 'other', 'per-player multiplier %s' % fmt(m)
        ch = 'D12 capture function with the fitted multiplier %s (planned M4)' % fmt(m)
    srow = src_row('M2', 'capture time T(n)', ' '.join('%d:%s' % (n, fmt(T[n])) for n in T), 's', run0.id, _build(run0),
                   run0.hz, sum(len(v) for v in series['N'].values()), 'median per n vs model', 'D12 capture function '
                   '(planned M4; CapturePointDef)', 'P 7.3 conquest row')
    out.append(Result('M2', txt, v, nums, ch, [srow]))
    if F:
        ratios = [F[n] / T[n] for n in F if n in T and T[n] > 0]
        r = stats.median(ratios)
        if r is not None:
            fv = 'lose_get' if abs(r - 2) <= 0.15 else ('direct' if abs(r - 1) <= 0.15 else 'other')
            out.append(Result('M2-flip', 'flip = %s (T_flip / T = %s)' % (
                'lose + get' if fv == 'lose_get' else fv, fmt(r)), fv, {'ratios': ratios},
                'D12 capture: seesaw (lose then get)' if fv == 'lose_get' else 'D12 capture: direct flip (review)'))
    if contest:
        out.append(Result('M2-contest', 'contested flag: loc slope %s per s' % fmt(stats.median(contest)),
                          'still' if abs(stats.median(contest)) < 0.05 else 'moves', {'slopes': contest}))
    if leave:
        out.append(Result('M2-leave', 'empty flag returns at %s loc/s' % fmt(stats.median(leave)), 'return',
                          {'slopes': leave}, 'D12 capture: return_multiplier from the fitted rate'))
    return out

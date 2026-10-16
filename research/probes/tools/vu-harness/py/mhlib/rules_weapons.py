"""Weapon rules W1-W5 (+ W6/W7 reports) of DESIGN.md section 10 on extracted W-A / W-B / W-C / S-B runs."""

import json
import math
import os

import numpy as np

from . import fits, stats
from .sources import MECH, Result, fmt, no_data, row as src_row

HERE = os.path.dirname(os.path.abspath(__file__))
CONSTANTS = os.path.join(HERE, '..', '..', 'data', 'weapon_constants.json')


def constants():
    with open(CONSTANTS, encoding='utf-8') as f:
        return json.load(f)['weapons']


def _build(run):
    for m in run.metas:
        if isinstance(m.get('build'), int):
            return m['build']
    return run.meta.get('vu_build')


# ---------------------------------------------------------------------------------------------------------------------
# shared helpers

def part_windows(run, win):
    """{part: (q0, q1)} from the 'part' marks inside a config window."""
    ms = [m for m in run.marks('part', win.q0, win.q1)]
    out = {}
    for i, m in enumerate(ms):
        q1 = ms[i + 1]['q'] if i + 1 < len(ms) else win.q1
        p = (m.get('x') or {}).get('p')
        if p:
            out[p] = (m['q'], q1)
    return out


def unit_windows(run, q0, q1, part):
    """[(mark, q0, q1)] of the 'u' marks of one part."""
    ms = [m for m in run.marks('u', q0, q1) if (m.get('x') or {}).get('p') == part]
    out = []
    for i, m in enumerate(ms):
        e = ms[i + 1]['q'] if i + 1 < len(ms) else q1
        out.append((m, m['q'], e))
    return out


class GS(object):
    """gs rows of one soldier: k -> last row (srv preferred over poll)."""

    def __init__(self, run, sid, q0=None, q1=None):
        self.k = {}
        for r in run.of('gs', q0, q1):
            if r.get('id') != sid:
                continue
            old = self.k.get(r['k'])
            if old is None or r.get('src') == 'srv' or old.get('src') != 'srv':
                self.k[r['k']] = r
        self.keys = sorted(self.k)

    def at(self, k):
        return self.k.get(k)

    def p(self, k):
        r = self.k.get(k)
        return r['rec'][0] if r else None

    def after(self, k0, k1):
        return [(k, self.k[k]) for k in self.keys if k0 <= k <= k1]


def shooter_of(shots):
    ids = {}
    for s in shots:
        ids[s['id']] = ids.get(s['id'], 0) + 1
    return max(ids, key=ids.get) if ids else None


def post_offset(gs, shot_ks):
    """0 if the gs row of the shot tick already shows the kick, else 1."""
    d0, d1 = [], []
    for k in shot_ks:
        a, b, c = gs.p(k - 1), gs.p(k), gs.p(k + 1)
        if a is not None and b is not None:
            d0.append(b - a)
        if a is not None and c is not None:
            d1.append(c - a)
    if not d0:
        return 1
    m0 = float(np.median(d0))
    m1 = float(np.median(d1)) if d1 else 0.0
    return 0 if m0 > 0.5 * max(m1, 1e-12) else 1


def weapon_of(win):
    x = win.x or {}
    if x.get('w'):
        return x['w']
    lab = win.label or ''
    return lab.split('-')[1] if lab.startswith('wb-') else lab.split('-')[0]


def zoom_of(win):
    x = win.x or {}
    if 'zoom' in x:
        return bool(x['zoom'])
    return (win.label or '').endswith('ads')


# ---------------------------------------------------------------------------------------------------------------------
# W1

def w1_points(run, win):
    parts = part_windows(run, win)
    if 'a' not in parts:
        return None
    q0, q1 = parts['a']
    shots = [s for s in run.of('shot', q0, q1) if s.get('kind') == 'bullet']
    sid = shooter_of(shots)
    shots = [s for s in shots if s['id'] == sid]
    if len(shots) < 10:
        return {'n0': len(shots)}
    hits = {}
    for h in run.of('hit', q0, win.q1):
        hits.setdefault(h['uid'], []).append(h)
    gs = GS(run, sid, win.q0, win.q1)
    ks = [s['k'] for s in shots]
    off = post_offset(gs, ks)
    kicks = []
    for k in ks:
        a, b = gs.p(k - 1 + off), gs.p(k + off)
        if a is not None and b is not None:
            kicks.append(b - a)
    kick = float(np.median(kicks)) if kicks else None
    pts, rr, idx, dropped_rec = [], [], [], 0
    hp = []
    for i, s in enumerate(shots):
        hl = hits.get(s['uid'])
        if not hl:
            continue
        pre = gs.p(s['k'] - 1 + off)
        if kick and pre is not None and pre > 0.02 * kick:
            dropped_rec += 1
            continue
        hp.append(np.array(hl[0]['pos'], dtype=float))
        pts.append((np.array(s['pos'], dtype=float), np.array(hl[0]['pos'], dtype=float)))
        g = gs.at(s['k'])
        if g is None or not g.get('fs'):
            g = gs.at(s['k'] + 1)
        rr.append(g['rr'] if g is not None else None)
        idx.append(i)
    if len(pts) < 10:
        return {'n0': len(shots), 'hits': len(pts)}
    H = np.array(hp)
    c = H.mean(axis=0)
    _, _, vt = np.linalg.svd(H - c)
    nrm = vt[2]
    offp = np.abs((H - c) @ nrm)
    on = offp <= 0.05
    dirs = np.array([(h - o) / np.linalg.norm(h - o) for o, h in pts])[on]
    f = dirs.mean(axis=0)
    f /= np.linalg.norm(f)
    up = np.array([0.0, 1.0, 0.0])
    r = np.cross(up, f)
    if np.linalg.norm(r) < 1e-6:
        r = np.array([1.0, 0.0, 0.0])
    r /= np.linalg.norm(r)
    u = np.cross(f, r)
    df = dirs @ f
    P = np.stack([np.arctan2(dirs @ r, df), np.arctan2(dirs @ u, df)], axis=1)
    rr = [x for x, o in zip(rr, on) if o]
    idx = [x for x, o in zip(idx, on) if o]
    dm = [g.get('dmin') for g in GS(run, sid, q0, q1).k.values() if g.get('dmin') is not None]
    return {'P': P, 'rr': rr, 'idx': idx, 'n0': len(shots), 'off_plane': int((~on).sum()), 'dropped_rec': dropped_rec,
            'kick': kick, 'dmin': float(np.median(dm)) if dm else None, 'sid': sid}


def w1_config(run, win):
    d = w1_points(run, win)
    x = win.x or {}
    res = {'cfg': win.label, 'proto': x.get('proto'), 'verdict': 'no data'}
    if d is None or 'P' not in d:
        res.update(d or {})
        return res
    P = d['P']
    n = len(P)
    res.update({'n': n, 'off_plane': d['off_plane'], 'dropped_recoil': d['dropped_rec'], 'dmin': d['dmin']})
    if d['dmin'] is not None and d['dmin'] <= 0:
        res['verdict'] = 'no spread'
        return res
    DA, dropA = stats.w1_stat(P, 'A')
    DB, dropB = stats.w1_stat(P, 'B')
    cA, cB = stats.w1_crit('A', n), stats.w1_crit('B', n)
    th = np.hypot(*(P - P.mean(0)).T)
    rho = stats.spearman(np.array(d['idx'], dtype=float), th)
    res.update({'DA': DA, 'DB': DB, 'critA': cA, 'critB': cB, 'screened': dropA, 'spearman': rho,
                'ks_pvalue_ref': {'A': stats.ks_pvalue(DA, n), 'B': stats.ks_pvalue(DB, n)}})
    if x.get('sway') or abs(rho) > 0.3:
        res['verdict'] = 'sway-contaminated'
        return res
    if dropA > 0.02 * n:
        res['verdict'] = 'contaminated'
        return res
    accA, accB = DA <= cA, DB <= cB
    if accA and not accB:
        v = 'uniform_area'
    elif accB and not accA:
        v = 'uniform_radius'
    elif not accA and not accB:
        v = 'both rejected'
    else:
        v = 'inconclusive'
    # secondary: rr normalised by its maximum
    rr = [r for r in d['rr'] if isinstance(r, (int, float))]
    if len(rr) >= 20 and max(rr) > 0 and np.ptp(rr) > 0:
        R = np.array(rr, dtype=float)[None, :]
        sA = float(stats.w1_secondary_batch(R, 'A')[0])
        sB = float(stats.w1_secondary_batch(R, 'B')[0])
        scA, scB = stats.w1_secondary_crit('A', len(rr)), stats.w1_secondary_crit('B', len(rr))
        sec = 'uniform_area' if (sA <= scA and sB > scB) else ('uniform_radius' if (sB <= scB and sA > scA) else 'none')
        res['secondary'] = {'DA': sA, 'DB': sB, 'critA': scA, 'critB': scB, 'model': sec}
        if v in ('uniform_area', 'uniform_radius') and sec != 'none' and sec != v:
            v = 'inconclusive'
            res['secondary']['conflict'] = True
    else:
        res['secondary'] = 'unavailable (primary stands alone)'
    res['verdict'] = v
    return res


def rule_w1(runs, ctx=None):
    was = [r for r in runs if r.sc == 'WA']
    if not was:
        return no_data('W1', 'no W-A run')
    cfgs = []
    for run in was:
        for w in run.windows():
            cfgs.append((run, w1_config(run, w)))
    decisive = [c for run, c in cfgs if c.get('proto') == 'full' and c.get('n', 0) >= 150
                and c['verdict'] in ('uniform_area', 'uniform_radius', 'both rejected', 'inconclusive')]
    models = [c['verdict'] for c in decisive if c['verdict'] in ('uniform_area', 'uniform_radius')]
    both = [c for c in decisive if c['verdict'] == 'both rejected']
    numbers = {'configs': [dict((k, v) for k, v in c.items() if k != 'ks_pvalue_ref') for _, c in cfgs],
               'decisive': len(decisive)}
    run0 = was[0]
    if len(set(models)) > 1:
        return Result('W1', 'conflict: decisive configs disagree (%s); no change, review the pattern' %
                      ', '.join('%s=%s' % (c['cfg'], c['verdict']) for c in decisive), 'conflict', numbers)
    if len(both) >= 2 and not models:
        return Result('W1', 'empirical: both models rejected in %d decisive configs' % len(both), 'empirical', numbers,
                      change='add DiscDistribution::Empirical with a 16-knot CDF table from the pooled normalised radii; '
                             'Mechanics::disc = Empirical in ' + MECH + ' (m2a-weapons 5.6, 6.9)')
    if len(models) >= 2:
        m = models[0]
        enum = 'UniformArea' if m == 'uniform_area' else 'UniformRadius'
        n = sum(c['n'] for c in decisive)
        D = ', '.join('%s D_A=%.3f D_B=%.3f' % (c['cfg'], c['DA'], c['DB']) for c in decisive)
        srow = src_row('W1', 'spread disc distribution', m, '-', run0.id, _build(run0), run0.hz, n,
                       'KS vs uniform-area/radius, bootstrap alpha 0.01 (%s)' % D, MECH + ' Mechanics::disc',
                       'P 7.1 "spread disc"')
        return Result('W1', '%s (%d decisive configs agree)' % (m, len(models)),
                      'area' if m == 'uniform_area' else 'radius', numbers,
                      change='Mechanics::disc = DiscDistribution::%s in %s (m2a-weapons 5.6, 6.9)' % (enum, MECH),
                      source_rows=[srow])
    return Result('W1', 'inconclusive (%d decisive configs, %d with a model)' % (len(decisive), len(models)),
                  'inconclusive', numbers)


# ---------------------------------------------------------------------------------------------------------------------
# W2 decay and min rise

def burst_curves(run, win, parts=('b', 'e', 'd')):
    """[(sid, shots_k, gs, q0, q1, part)] per unit of the given parts."""
    out = []
    pw = part_windows(run, win)
    for p in parts:
        if p not in pw:
            continue
        for m, q0, q1 in unit_windows(run, pw[p][0], pw[p][1], p):
            shots = [s for s in run.of('shot', q0, q1) if s.get('kind') == 'bullet']
            if not shots:
                continue
            sid = shooter_of(shots)
            ks = sorted(s['k'] for s in shots if s['id'] == sid)
            out.append((sid, ks, q0, q1, p, m))
    return out


def decay_slopes(run, win):
    out = []
    jumps = []
    gs_cache = {}
    for sid, ks, q0, q1, p, m in burst_curves(run, win, ('b', 'e', 'd')):
        gs = gs_cache.get(sid) or GS(run, sid, win.q0, win.q1)
        gs_cache[sid] = gs
        seq = gs.after(ks[-1], ks[-1] + int(3 * run.hz))
        samples = []
        prev = None
        started = False
        for k, g in seq:
            da, dmin = g.get('da'), g.get('dmin')
            if da is None or dmin is None:
                continue
            if prev is not None and da < prev - 1e-9 and da > dmin + 1e-6:
                started = True
                samples.append((k / run.hz, da))
            elif started and (da >= prev - 1e-9 or da <= dmin + 1e-6):
                break
            prev = da
        if len(samples) >= 3:
            fit = stats.linfit([s[0] for s in samples], [s[1] for s in samples])
            if fit and fit['r2'] > 0.99:
                out.append({'slope': -fit['b'], 'dmin': seq[0][1].get('dmin') if seq else None, 'n': fit['n']})
        for a, b in zip(ks, ks[1:]):
            ga, gb = gs.at(a), gs.at(b)
            if ga and gb and b - a <= 3:
                jumps.append(gb['da'] - ga['da'])
    return out, jumps


def units_of(ctx):
    p0 = (ctx or {}).get('p0', {})
    v = p0.get('P0.11', {})
    return v.get('units', 'unknown'), v.get('angle', 'unknown')


def rule_w2(runs, ctx=None):
    was = [r for r in runs if r.sc == 'WA']
    C = constants()
    unit, angle = units_of(ctx)
    per = []
    for run in was:
        for w in run.windows():
            x = w.x or {}
            sl, jumps = decay_slopes(run, w)
            if not sl:
                continue
            slope = float(np.median([s['slope'] for s in sl]))
            dmin = float(np.median([s['dmin'] for s in sl if s['dmin'] is not None]))
            wk = weapon_of(w)
            mode = 'ads' if zoom_of(w) else 'hip'
            data = (C.get(wk) or {}).get(mode) or {}
            item = {'cfg': w.label, 'slope': slope, 'dmin': dmin, 'n_curves': len(sl), 'ratio': slope / dmin if dmin else None,
                    'jump': float(np.median(jumps)) if jumps else None, 'stance': x.get('stance'), 'run': run.id}
            if x.get('stance', 'stand') == 'stand' and data.get('min') and data.get('dec_per_s'):
                item['data_ratio'] = data['dec_per_s'] / data['min']
                item['community_ratio'] = 8.0 / data['min']
            per.append(item)
    out = []
    if not per:
        out.append(no_data('W2', 'no W-A burst curves'))
    else:
        cand = [p for p in per if p.get('data_ratio') and p['ratio']]
        verdict, variant, change = 'inconclusive', 'inconclusive', 'no change'
        if cand:
            if unit == 'deg':
                rel_d = [abs(p['slope'] / 15.0 - 1) for p in cand]
                rel_c = [abs(p['slope'] / 8.0 - 1) for p in cand]
            else:
                rel_d = [abs(p['ratio'] / p['data_ratio'] - 1) for p in cand]
                rel_c = [abs(p['ratio'] / p['community_ratio'] - 1) for p in cand]
            if max(rel_d) <= 0.05:
                verdict, variant = 'data confirmed: DecreasePerSecond 15 deg/s (C14 closes)', '15'
            elif max(rel_c) <= 0.05:
                verdict, variant = 'community value: 8 deg/s', '8'
                change = ('add Mechanics::dispersion_decrease_scale = %.4g (8/15) in %s and apply it to '
                          'DecreasePerSecond' % (8 / 15.0, MECH))
            else:
                verdict = 'neither 15 nor 8 within 5 %% (rel. to data %s)' % fmt(float(np.median(rel_d)))
                variant = 'other'
        run0 = was[0]
        srow = src_row('W2', 'dispersion decrease', float(np.median([p['slope'] for p in per])),
                       'deg/s' if unit == 'deg' else 'da units/s (unit %s)' % unit, run0.id, _build(run0), run0.hz,
                       sum(p['n_curves'] for p in per), 'LSQ slope, R2>0.99, per config median',
                       MECH + ' (dispersion decrease)', 'P 7.1 M16A3 hip 2.5-7 +0.1; P 8 C14')
        out.append(Result('W2', verdict, variant, {'configs': per, 'unit': unit}, change, [srow] if variant != 'inconclusive' else []))
    # min rise from S-B
    sbs = [r for r in runs if r.sc == 'SB']
    rises = []
    for run in sbs:
        for m in run.marks('step'):
            if not str(m.get('cfg', '')).startswith('minrise'):
                continue
            off = [x for x in run.marks('step-off') if x['q'] > m['q']]
            q1 = off[0]['q'] if off else None
            gss = {}
            for r in run.of('gs', m['q'], q1):
                gss.setdefault(r['id'], {})[r['k']] = r
            for sid, ser in gss.items():
                ks = sorted(ser)
                km = next((k for k in ks if ser[k].get('mov')), None)
                if km is None:
                    continue
                dnew = ser[km]['dmin']
                prev = [ser[k] for k in ks if k < km]
                if not prev or dnew <= prev[-1]['dmin'] + 1e-9:
                    continue
                kr = next((k for k in ks if k >= km and ser[k]['da'] >= dnew - 1e-6), None)
                if kr is None:
                    continue
                pts = [(k / run.hz, ser[k]['da']) for k in ks if km <= k <= kr]
                fit = stats.linfit([p[0] for p in pts], [p[1] for p in pts]) if len(pts) >= 3 else None
                rises.append({'ticks': kr - km, 'slope': fit['b'] if fit else None, 'cfg': m.get('cfg'),
                              'rise': dnew - prev[-1]['dmin']})
    if rises:
        # immediate: the gs row of the first moving tick already shows da at the new minimum
        imm = [r for r in rises if r['ticks'] == 0]
        if len(imm) >= 0.8 * len(rises):
            out.append(Result('W2-minrise', 'immediate: da steps to the moving minimum in the same tick', 'immediate',
                              {'rises': rises}, 'Mechanics::min_spread_rise = MinSpreadRise::Immediate in ' + MECH))
        else:
            sl = stats.median([r['slope'] for r in rises if r['slope']])
            out.append(Result('W2-minrise', 'ramp: da rises at %s per s (compare the W2 decrease rate)' % fmt(sl),
                              'ramp', {'rises': rises, 'slope': sl},
                              'Mechanics::min_spread_rise = MinSpreadRise::AtDecreaseRate in ' + MECH))
    elif sbs:
        out.append(no_data('W2-minrise', 'no moving-minimum step in S-B'))
    return out


# ---------------------------------------------------------------------------------------------------------------------
# W3 recovery

def recovery_curves(run, win):
    out = []
    gsc = {}
    for sid, ks, q0, q1, p, m in burst_curves(run, win, ('b', 'd', 'e')):
        gs = gsc.get(sid) or GS(run, sid, win.q0, win.q1)
        gsc[sid] = gs
        off = post_offset(gs, ks)
        k_end = run.of('mark', q1, q1)
        kmax = k_end[0]['k'] if k_end else ks[-1] + int(3 * run.hz)
        seq = gs.after(ks[-1] + off, min(kmax - 1, ks[-1] + off + int(3 * run.hz)))
        # the curve starts once the weapon no longer counts as firing (the recovery while firing is scaled)
        while len(seq) > 1 and seq[0][1].get('fir'):
            seq = seq[1:]
        if not seq:
            continue
        t0 = seq[0][0]
        t = np.array([(k - t0) / run.hz for k, _ in seq])
        pv = np.array([g['rec'][0] for _, g in seq])
        out.append({'t': t, 'p': pv, 'part': p, 'ks': ks, 'sid': sid, 'gs': gs, 'off': off})
    return out


def classify_recovery(curves):
    counted, wins = 0, {'linear': 0, 'exp': 0}
    params = {'linear': [], 'exp': []}
    for c in curves:
        nz = int((c['p'] > 1e-9).sum())
        if nz < 6 or c['p'][0] <= 0:
            continue
        counted += 1
        lin = fits.fit_linear_to_zero(c['t'], c['p'])
        ex = fits.fit_exp(c['t'], c['p'])
        p0 = c['p'][0]
        if lin['rms'] < 0.5 * ex['rms'] and lin['rms'] < 0.05 * p0:
            wins['linear'] += 1
            params['linear'].append(lin['a'])
        elif ex['rms'] < 0.5 * lin['rms'] and ex['rms'] < 0.05 * p0:
            wins['exp'] += 1
            params['exp'].append(ex['b'])
    return counted, wins, params


def rule_w3(runs, ctx=None):
    was = [r for r in runs if r.sc == 'WA']
    if not was:
        return [no_data('W3', 'no W-A run')]
    C = constants()
    unit, _ = units_of(ctx)
    curves, yaw_incs = [], []
    for run in was:
        for w in run.windows():
            cs = recovery_curves(run, w)
            curves.extend(cs)
            for c in cs:
                if c['part'] != 'd':
                    continue
                gs, off = c['gs'], c['off']
                inc, gap = [], []
                prev = None
                for k in c['ks']:
                    a, b = gs.at(k - 1 + off), gs.at(k + off)
                    if a and b:
                        inc.append(b['rec'][1] - a['rec'][1])
                        gap.append(k - prev if prev is not None else 0)
                    prev = k
                if len(inc) >= 5:
                    # the recovery inside a shot tick depends on the tick gap pattern of the fire accumulator: remove
                    # the mean per gap length so only the kick sequence remains
                    inc, gap = np.array(inc), np.array(gap)
                    for gv in set(gap.tolist()):
                        m = gap == gv
                        inc[m] = inc[m] - inc[m].mean()
                    yaw_incs.append(inc)
    counted, wins, params = classify_recovery(curves)
    out = []
    numbers = {'curves': len(curves), 'counted': counted, 'wins': wins}
    run0 = was[0]
    if counted < 10:
        out.append(Result('W3', 'inconclusive: %d counted curves (< 10)' % counted, 'inconclusive', numbers))
    else:
        best = max(wins, key=wins.get)
        if wins[best] >= 0.8 * counted:
            par = float(np.median(params[best]))
            numbers['param'] = par
            dec = (C.get('M16A3') or {}).get('recoil', {}).get('dec')
            if best == 'linear':
                numbers['vs_RecoilAmplitudeDecreaseFactor'] = (par / dec) if (dec and unit == 'deg') else 'unit %s' % unit
                change = ('Mechanics::recovery = Recovery::Linear (rate %s %s/s; RecoilAmplitudeDecreaseFactor read as '
                          'degrees per second, m2a-weapons 5.6) in %s' % (fmt(par), 'deg' if unit == 'deg' else 'unit',
                                                                          MECH))
            else:
                change = 'Mechanics::recovery = Recovery::Exponential (b = %s 1/s) in %s' % (fmt(par), MECH)
            srow = src_row('W3', 'recoil recovery law', best, '1/s' if best == 'exp' else 'units/s', run0.id,
                           _build(run0), run0.hz, counted, 'RMS linear vs exp per curve (win in %d/%d)' %
                           (wins[best], counted), MECH + ' Mechanics::recovery', 'P 7.1 M16A3 "dec 18"')
            out.append(Result('W3', '%s recovery (%d/%d curves), parameter %s' % (best, wins[best], counted, fmt(par)),
                              best, numbers, change, [srow], notes=['while-firing decay ratio: not computed (as built)']))
        else:
            out.append(Result('W3', 'inconclusive: no law wins 80 %% of %d curves' % counted, 'inconclusive', numbers))
    if yaw_incs:
        x = np.concatenate([v for v in yaw_incs])
        r = stats.lag1(x)
        n = len(x)
        corr = abs(r) > 2 / math.sqrt(n)
        out.append(Result('W3-yaw', 'horizontal kicks %s (lag-1 r = %.3f, n = %d, threshold %.3f)' % (
            'correlated' if corr else 'independent', r, n, 2 / math.sqrt(n)), 'correlated' if corr else 'none',
            {'r': r, 'n': n}, 'new open item for D7: correlated horizontal recoil (code change)' if corr else 'no change'))
    return out


# ---------------------------------------------------------------------------------------------------------------------
# W4 first-shot re-arm

def rule_w4(runs, ctx=None):
    was = [r for r in runs if r.sc == 'WA']
    C = constants()
    units = []
    t_rec, t_min = [], []
    w3 = (ctx or {}).get('w3')
    for run in was:
        for w in run.windows():
            wk = weapon_of(w)
            fsrm = ((C.get(wk) or {}).get('recoil') or {}).get('fsrm')
            pw = part_windows(run, w)
            if 'c' not in pw:
                continue
            gs = None
            # t_rec and t_min from part a single shots
            if 'a' in pw:
                shots = [s for s in run.of('shot', *pw['a']) if s.get('kind') == 'bullet']
                sid = shooter_of(shots)
                gs = GS(run, sid, w.q0, w.q1)
                off = post_offset(gs, [s['k'] for s in shots])
                for s in shots[:60]:
                    k = s['k'] + off
                    pk = gs.p(k)
                    if not pk or pk <= 0:
                        continue
                    for kk in range(k, k + int(3 * run.hz)):
                        v = gs.p(kk)
                        if v is not None and v <= 0.05 * pk:
                            t_rec.append((kk - s['k']) / run.hz)
                            break
                    for kk in range(s['k'], s['k'] + int(3 * run.hz)):
                        g = gs.at(kk)
                        if g is not None and kk > s['k'] and g['da'] <= g['dmin'] + 1e-6:
                            t_min.append((kk - s['k']) / run.hz)
                            break
            for m, q0, q1 in unit_windows(run, pw['c'][0], pw['c'][1], 'c'):
                shots = sorted([s for s in run.of('shot', q0, q1) if s.get('kind') == 'bullet'], key=lambda s: s['q'])
                if len(shots) < 2:
                    continue
                sid = shots[0]['id']
                if gs is None:
                    gs = GS(run, sid, w.q0, w.q1)
                off = post_offset(gs, [s['k'] for s in shots]) if 'a' not in pw else off
                k1, k2 = shots[0]['k'], shots[1]['k']
                d = []
                for k in (k1, k2):
                    a, b = gs.p(k - 1 + off), gs.p(k + off)
                    if a is None or b is None:
                        d = None
                        break
                    corr = 0.0
                    if a > 0 and w3:
                        if w3[0] == 'linear':
                            corr = min(a, w3[1] / run.hz)
                        elif w3[0] == 'exp':
                            corr = a * (1 - math.exp(-w3[1] / run.hz))
                    d.append(b - a + corr)
                if not d or d[0] <= 0:
                    continue
                g = (m.get('x') or {}).get('g')
                units.append({'gap_k': k2 - k1, 'held': g is not None and g <= 0.08, 'g': g, 'ratio': d[1] / d[0],
                              'fsrm': fsrm, 'cfg': w.label})
    if not units:
        return no_data('W4', 'no part c pairs')
    fsrm = stats.median([u['fsrm'] for u in units if u['fsrm']]) or 2.5
    for u in units:
        r = u['ratio']
        u['state'] = 'armed' if abs(r - 1) <= 0.15 else ('not' if abs(r * fsrm - 1) <= 0.15 else 'unclear')
    hz = was[0].hz
    bins = {}
    for u in units:
        key = (u['gap_k'], u['held'])
        b = bins.setdefault(key, {'armed': 0, 'not': 0, 'unclear': 0})
        b[u['state']] += 1
    obs = {}
    for key, b in bins.items():
        tot = b['armed'] + b['not']
        if tot >= 2:
            obs[key] = b['armed'] / tot >= 0.5
    trec = stats.median(t_rec)
    tmin = stats.median(t_min)
    numbers = {'bins': {'%d%s' % (k[0], 'h' if k[1] else ''): v for k, v in sorted(bins.items())}, 't_rec': trec,
               't_min': tmin, 'fsrm': fsrm, 'hz': hz}
    one = 1.0 / hz + 1e-9

    def consistent(pred):
        return all(pred(g / hz, held) == armed for (g, held), armed in obs.items())

    cands = []
    if consistent(lambda gap, held: not held):
        cands.append('release')
    if trec is not None and consistent(lambda gap, held: gap >= trec - one):
        cands.append('recovered')
    if tmin is not None and consistent(lambda gap, held: gap >= tmin - one):
        cands.append('spreadmin')
    # timer: the step between the largest non-armed and the smallest armed gap
    na = [g for (g, h), a in obs.items() if not a]
    ar = [g for (g, h), a in obs.items() if a]
    timer = None
    if na and ar and max(na) < min(ar):
        T = (max(na) + min(ar)) / 2.0 / hz
        if consistent(lambda gap, held: gap >= T):
            timer = T
    numbers['consistent'] = cands
    numbers['timer_T'] = timer
    if 'release' in cands:
        also = [c for c in cands if c != 'release']
        return Result('W4', 'OnTriggerRelease: held pairs not armed, every released gap armed' +
                      (' (also consistent: %s)' % ', '.join(also) if also else ''), 'release', numbers,
                      'Mechanics::first_shot_rearm = FirstShotRearm::OnTriggerRelease in ' + MECH)
    if 'recovered' in cands:
        return Result('W4', 'WhenRecovered: armed iff gap >= t_rec %s s' % fmt(trec), 'recovered', numbers,
                      'Mechanics::first_shot_rearm = FirstShotRearm::WhenRecovered in ' + MECH)
    if 'spreadmin' in cands:
        return Result('W4', 'WhenSpreadMin: armed iff gap >= t_min %s s' % fmt(tmin), 'spreadmin', numbers,
                      'add FirstShotRearm::WhenSpreadMin (new enum value); Mechanics::first_shot_rearm = WhenSpreadMin '
                      'in ' + MECH)
    if timer is not None:
        return Result('W4', 'AfterTime(%.3f s): a step matching neither t_rec nor t_min' % timer,
                      'timer:%.1f' % (round(timer * 10) / 10.0), numbers,
                      'add FirstShotRearm::AfterTime with T = %.3f s (new enum value) in %s' % (timer, MECH))
    return Result('W4', 'inconclusive: no candidate law fits every gap bin', 'inconclusive', numbers)


# ---------------------------------------------------------------------------------------------------------------------
# W5 falloff (+ W6 pass-through report)

def wb_points(run, win):
    passes = []
    for m in run.marks('pass', win.q0, win.q1):
        e = [x for x in run.marks('pass-end', m['q'], win.q1)]
        passes.append((m['q'], e[0]['q'] if e else win.q1, m.get('x') or {}))
    pts = []
    flagged = 0
    for r in run.of('dmg', win.q0, win.q1):
        if r.get('ph') != 'pre' or not r.get('bul'):
            continue
        if any(a <= r['q'] <= b for a, b, _ in passes):
            continue
        dist, fxd = r.get('dist'), r.get('fxd')
        if dist is None:
            continue
        if fxd is not None and abs(dist - fxd) > 0.5:
            flagged += 1
            continue
        pts.append((dist, r['dmg'], r.get('bone', -1)))
    return pts, flagged, passes


def rule_w5(runs, ctx=None):
    wbs = [r for r in runs if r.sc == 'WB']
    if not wbs:
        return [no_data('W5', 'no W-B run')]
    C = constants()
    out = []
    for run in wbs:
        for w in run.windows():
            wk = weapon_of(w)
            fo = (C.get(wk) or {}).get('falloff')
            pts, flagged, passes = wb_points(run, w)
            if not fo or len(pts) < 6:
                out.append(no_data('W5', '%s: %d points' % (w.label, len(pts))))
                continue
            d = np.array([p[0] for p in pts])
            dmg = np.array([p[1] for p in pts])
            bones = np.array([p[2] for p in pts])
            knots = (fo['start_dmg'], fo['end_dmg'], fo['start_m'], fo['end_m'])
            lin = fits.fit_falloff(d, dmg, bones, knots, 'linear')
            smo = fits.fit_falloff(d, dmg, bones, knots, 'smooth')
            nums = {'cfg': w.label, 'n': len(pts), 'flagged': flagged, 'rms_linear': lin['rms'], 'rms_smooth': smo['rms'],
                    'bone_scales': lin['scales'], 'dmax': float(d.max())}
            item = 'W5 %s' % wk
            if wk == 'M98B' and 'end_m_community' in fo:
                if d.max() >= 160:
                    best, rbest, _ = fits.free_end_knot(d, dmg, bones, knots, 100, 175)
                    r150 = fits.fit_falloff(d, dmg, bones, knots)['rms']
                    k120 = (knots[0], knots[1], knots[2], fo['end_m_community'])
                    r120 = fits.fit_falloff(d, dmg, bones, k120)['rms']
                    nums.update({'end_free': best, 'rms_free': rbest, 'rms_150': r150, 'rms_120': r120})
                    v = '150' if r150 < r120 else '120'
                    srow = src_row('W5', 'M98B falloff end distance', float(v), 'm', run.id, _build(run), run.hz,
                                   len(pts), 'RMS 150: %s, 120: %s, free knot %s' % (fmt(r150), fmt(r120), fmt(best)),
                                   MECH + ' falloff data (M98B)', 'P 7.1 M98B row; P 8 C14')
                    out.append(Result(item + ' end knot', 'end knot %s m (%s; free fit %s m)' % (
                        v, 'data' if v == '150' else 'community', fmt(best)), v, nums,
                        'no change (data 150 m confirmed)' if v == '150' else
                        'M98B falloff end distance 120 m (community) replaces the data knot 150 m: review the data '
                        'path before changing', [srow]))
                else:
                    out.append(Result(item + ' end knot', 'inconclusive: no points beyond 150 m (range %s m)' %
                                      fmt(float(d.max())), 'inconclusive', nums))
            if lin['rms'] < 0.1:
                srow = src_row('W5', '%s falloff shape' % wk, 'linear', 'hp', run.id, _build(run), run.hz, len(pts),
                               'RMS %s hp vs data knots, per-bone scale' % fmt(lin['rms']), MECH + ' Mechanics::falloff',
                               'P 7.1 %s row' % wk)
                out.append(Result(item, 'linear confirmed (RMS %s hp)' % fmt(lin['rms']), 'linear', nums,
                                  'no change (Mechanics::falloff = Linear)', [srow]))
            elif smo['rms'] < 0.1:
                out.append(Result(item, 'non-linear: smoothstep between the data knots (RMS %s hp; linear %s)' % (
                    fmt(smo['rms']), fmt(lin['rms'])), 'smooth', nums,
                    'add Falloff::SmoothStep (new value + table) and set Mechanics::falloff in ' + MECH))
            else:
                s0, s1, r = fits.free_knots(d, dmg, bones, knots)
                nums.update({'free_start': s0, 'free_end': s1, 'rms_free': r})
                out.append(Result(item, 'neither linear nor smooth; free knots %s-%s m (RMS %s)' % (
                    fmt(s0), fmt(s1), fmt(r)), 'other', nums, 'review: a new falloff table in ' + MECH))
            # W6 report: pass-through
            for a, b, x in passes:
                t1, t2 = x.get('t1'), x.get('t2')
                d1 = [r for r in run.of('dmg', a, b) if r.get('ph') == 'pre' and r.get('victim') == t1]
                d2 = [r for r in run.of('dmg', a, b) if r.get('ph') == 'pre' and r.get('victim') == t2]
                ratio = (np.mean([r['dmg'] for r in d2]) / np.mean([r['dmg'] for r in d1])) if d1 and d2 else None
                out.append(Result('W6 %s' % wk, 'bullets pass soldiers: %s (T2 hits %d, T1 hits %d%s)' % (
                    'yes' if d2 else 'no', len(d2), len(d1), ', ratio %.3f' % ratio if ratio else ''),
                    'pass' if d2 else 'stop', {'t1': len(d1), 't2': len(d2), 'ratio': ratio},
                    'Mechanics::bullets_pass_soldiers = %s in %s' % ('true' if d2 else 'false', MECH)))
    return out


def rule_w7(runs, ctx=None):
    wcs = [r for r in runs if r.sc == 'WC']
    if not wcs:
        return [no_data('W7', 'no W-C run')]
    out = []
    for run in wcs:
        exp = [r for r in run.of('dmg') if r.get('ph') == 'pre' and r.get('exp')]
        direct = [r for r in run.of('dmg') if r.get('ph') == 'pre' and not r.get('exp') and r.get('dmg', 0) > 90]
        pts = [(r['dist'], r['dmg']) for r in exp if r.get('dist') is not None]
        nums = {'explosion_hits': len(exp), 'direct': [r['dmg'] for r in direct][:10]}
        if len(pts) >= 4:
            d = np.array([p[0] for p in pts])
            y = np.array([p[1] for p in pts])
            lf = stats.linfit(d, y)
            q = np.polyfit(d, y, 2)
            rq = float(np.sqrt(np.mean((np.polyval(q, d) - y) ** 2)))
            nums.update({'rms_linear': lf['rms'] if lf else None, 'rms_quadratic': rq})
            shape = 'linear' if lf and lf['rms'] <= rq * 1.5 else 'quadratic'
        else:
            shape = 'insufficient'
        out.append(Result('W7', 'blast falloff %s; direct hits %s' % (shape, nums['direct'][:3]), shape, nums,
                          'Mechanics::blast_falloff = %s in %s' % ('Linear' if shape == 'linear' else 'review', MECH)))
    return out

"""Soldier rules S1 (regeneration), S2 (spawn protection report), S3 (motor, gravity, falls) of DESIGN.md 10."""

import math

import numpy as np

from . import fits, stats
from .sources import MOTOR_CFG, SOLDIER_DEF, Result, fmt, no_data, row as src_row


def _build(run):
    for m in run.metas:
        if isinstance(m.get('build'), int):
            return m['build']
    return run.meta.get('vu_build')


def _hp(run, sid, q0, q1):
    out = {}
    for r in run.of('hp', q0, q1):
        if r.get('id') == sid:
            out[r['k']] = r
    return out


def _victim(run, q0, q1):
    c = {}
    for r in run.of('dmg', q0, q1):
        c[r.get('victim')] = c.get(r.get('victim'), 0) + 1
    return max(c, key=c.get) if c else None


def rep_windows(run, win, cfg):
    reps = [m for m in run.marks('rep', win.q0, win.q1) if m.get('cfg') == cfg]
    out = []
    for i, m in enumerate(reps):
        out.append((m, m['q'], reps[i + 1]['q'] if i + 1 < len(reps) else win.q1))
    return out


def regen_rep(run, T, q0, q1, k_from=None):
    """(k_last_damage, k_onset, rate) for one rep window."""
    dmg = [r for r in run.of('dmg', q0, q1) if r.get('victim') == T and r.get('ph') == 'post' and r.get('dmg', 0) > 0]
    if not dmg:
        return None
    kd = dmg[-1]['k']
    hp = _hp(run, T, q0, q1)
    ks = sorted(k for k in hp if k >= kd)
    kon = None
    for a, b in zip(ks, ks[1:]):
        if hp[b]['hp'] > hp[a]['hp'] + 1e-6 and hp[b].get('alive', True):
            kon = b
            break
    if kon is None:
        return {'kd': kd, 'kon': None}
    rise = [(k / run.hz, hp[k]['hp']) for k in ks if k >= kon - 1 and hp[k]['hp'] < 100 - 1e-6]
    fit = stats.linfit([p[0] for p in rise], [p[1] for p in rise]) if len(rise) >= 3 else None
    return {'kd': kd, 'kon': kon, 'rate': fit['b'] if fit else None}


def rule_s1(runs, ctx=None):
    sas = [r for r in runs if r.sc == 'SA']
    out = []
    if not sas:
        return [no_data('S1', 'no S-A run')]
    onsets, rates, supp = [], [], []
    immune = []
    run0 = sas[0]
    for run in sas:
        for w in run.windows():
            T = _victim(run, w.q0, w.q1)
            if w.label == 'regen':
                for m, q0, q1 in rep_windows(run, w, 'regen'):
                    r = regen_rep(run, T, q0, q1)
                    if r and r.get('kon'):
                        onsets.append((r['kon'] - r['kd']) / run.hz)
                        if r.get('rate'):
                            rates.append(r['rate'])
            elif w.label == 'supp':
                for m, q0, q1 in rep_windows(run, w, 'supp'):
                    seen = [x for x in run.marks(None, q0, q1) if x.get('lbl') in ('supp-seen', 'supp-none')]
                    if not seen:
                        continue
                    ok = seen[0]['lbl'] == 'supp-seen'
                    r = regen_rep(run, T, q0, q1)
                    if not r or not r.get('kon'):
                        continue
                    ev = [e for e in run.of('ev', q0, q1) if e.get('name') == 'Player:SuppressedEnemy' and e.get('id2') == T
                          and e['k'] < r['kon']]
                    ks = ev[-1]['k'] if ev else None
                    if ks is None:
                        g = [x for x in run.of('gs', q0, q1) if x.get('id') == T and x.get('smf') not in (None, 1, 1.0)
                             and x['k'] < r['kon']]
                        ks = g[-1]['k'] if g else None
                    supp.append({'seen': ok, 'onset_d': (r['kon'] - r['kd']) / run.hz,
                                 'onset_s': (r['kon'] - ks) / run.hz if ks is not None else None})
            elif w.label == 'immune':
                ims = [m for m in run.marks('immune', w.q0, w.q1)]
                for i, m in enumerate(ims):
                    q1 = ims[i + 1]['q'] if i + 1 < len(ims) else w.q1
                    pre = [r for r in run.of('dmg', m['q'], q1) if r.get('victim') == T and r.get('ph') == 'pre'
                           and r.get('dmg', 0) > 0]
                    post = [r for r in run.of('dmg', m['q'], q1) if r.get('victim') == T and r.get('ph') == 'post']
                    if pre:
                        prot = all(abs(p['hp'] - pre[j]['hp']) < 1e-6 for j, p in enumerate(post[:len(pre)])) if post else True
                        immune.append({'delay': (m.get('x') or {}).get('delay'), 'protected': prot,
                                       't': (pre[0]['k'] - m['k']) / run.hz})
    nums = {'onsets': onsets, 'rates': rates, 'supp': supp}
    if len(onsets) < 2:
        out.append(Result('S1', 'inconclusive: %d regeneration onsets' % len(onsets), 'inconclusive', nums))
    else:
        D = float(np.median(onsets))
        R = stats.median(rates)
        nums.update({'delay': D, 'rate': R})
        if abs(D - 10) <= 0.5 and R is not None and abs(R - 5) <= 0.25:
            v, txt, ch = '10', 'data confirmed: regeneration delay 10 s, rate 5 hp/s', \
                'no change (SoldierDef::regeneration_delay 10, regeneration_rate 5 in %s)' % SOLDIER_DEF
        elif abs(D - 16) <= 0.5:
            v, txt, ch = '16', 'community value: onset 16 s', ('model a second (suppression) timer in the health '
                                                               'module; SoldierDef::regeneration_delay stays 10 (%s)' % SOLDIER_DEF)
        else:
            v, txt, ch = 'other', 'onset %s s, rate %s hp/s (neither 10 nor 16)' % (fmt(D), fmt(R)), \
                'set SoldierDef::regeneration_delay = %s, regeneration_rate = %s (%s)' % (fmt(D), fmt(R), SOLDIER_DEF)
        srow = src_row('S1', 'health regeneration delay / rate', '%s s / %s hp/s' % (fmt(D), fmt(R)), 's, hp/s', run0.id,
                       _build(run0), run0.hz, len(onsets), 'median onset after the last damage; LSQ rise slope',
                       SOLDIER_DEF + ' regeneration_delay, regeneration_rate', 'P 7.2 health row; P 8 C14')
        out.append(Result('S1', txt, v, nums, ch, [srow]))
        ev = [s for s in supp if s['seen'] and s['onset_s'] is not None]
        if not [s for s in supp if s['seen']]:
            out.append(Result('S1-supp', 'suppression not induced: no SUPP verdict', 'not-induced', {'supp': supp}))
        elif ev:
            ab = [s for s in ev if abs(s['onset_s'] - D) <= 0.5 and s['onset_d'] > D + 1]
            na = [s for s in ev if abs(s['onset_d'] - D) <= 0.5]
            if len(ab) >= 0.8 * len(ev):
                out.append(Result('S1-supp', 'suppression aborts regeneration: onset %s s after the last suppression'
                                  % fmt(stats.median([s['onset_s'] for s in ab])), 'abort', {'supp': supp},
                                  'SuppressionAbortsHealthRegeneration: suppression restarts the regeneration delay '
                                  '(health module, %s)' % SOLDIER_DEF))
            elif len(na) >= 0.8 * len(ev):
                out.append(Result('S1-supp', 'suppression does not abort regeneration (onset %s s after damage)' %
                                  fmt(stats.median([s['onset_d'] for s in na])), 'no-abort', {'supp': supp},
                                  'no change (suppression does not touch the regeneration timer)'))
            else:
                out.append(Result('S1-supp', 'inconclusive suppression pattern', 'inconclusive', {'supp': supp}))
    if immune:
        prot = [i['delay'] for i in immune if i['protected'] and i['delay'] is not None]
        out.append(Result('S2', 'spawn protection: largest protected delay %s s' % (fmt(max(prot)) if prot else 'none'),
                          str(max(prot)) if prot else 'none', {'immune': immune},
                          'Mechanics::spawn_protection_blocks / SoldierDef::immortal_time_after_spawn (report only)'))
    return out


# ---------------------------------------------------------------------------------------------------------------------
# S3

def _speed(r):
    v = r.get('vel')
    if v is None:
        return None
    return math.hypot(v[0], v[2])


def _bot_of(run, q0, q1):
    c = {}
    for r in run.of('hp', q0, q1):
        c[r['id']] = c.get(r['id'], 0) + 1
    return max(c, key=c.get) if c else None


def motor_step(run, m, off):
    q0, q1 = m['q'], off['q']
    sid = _bot_of(run, q0, q1)
    hp = _hp(run, sid, q0, None)
    k0, k1 = m['k'], off['k']
    ks = [k for k in sorted(hp) if k0 <= k < k1]
    sp = [(k, _speed(hp[k])) for k in ks if _speed(hp[k]) is not None]
    if len(sp) < 8:
        return None
    tail = [v for k, v in sp if k >= k1 - run.hz]
    vstar = float(np.mean(tail))
    v0 = sp[0][1]
    t = np.array([(k - k0) / run.hz for k, _ in sp])
    v = np.array([x for _, x in sp])
    f = fits.fit_motor(t, v, vstar, v0, 1 / run.hz)
    order = sorted(f, key=lambda n: f[n]['rms'])
    win = order[0] if f[order[0]]['rms'] < 0.5 * f[order[1]]['rms'] else None
    if f['instant']['rms'] <= f[order[0]]['rms'] + 1e-9 and f['instant']['rms'] < 0.05 * max(vstar, 1e-6):
        win = 'instant'
    res = {'cfg': m.get('cfg'), 'vstar': vstar, 'law': win or 'unclear', 'rms': {n: f[n]['rms'] for n in f}}
    if win == 'exponential':
        res['tau'] = f['exponential']['tau']
        res['gain30'] = fits.gain_from_tau(f['exponential']['tau'])
    if win == 'linear':
        res['a'] = f['linear']['a']
    return res


def rule_s3(runs, ctx=None):
    sbs = [r for r in runs if r.sc == 'SB']
    if not sbs:
        return [no_data('S3', 'no S-B run')]
    steps, falls, jumps = [], [], []
    run0 = sbs[0]
    for run in sbs:
        ms = run.marks('step')
        offs = run.marks('step-off')
        for m in ms:
            if str(m.get('cfg', '')).startswith('minrise'):
                continue
            off = next((o for o in offs if o['q'] > m['q']), None)
            if off is None:
                continue
            r = motor_step(run, m, off)
            if r:
                steps.append(r)
        for m in run.marks('drop'):
            e = next((x for x in run.marks('drop-end') if x['q'] > m['q']), None)
            if e is None:
                continue
            sid = _bot_of(run, m['q'], e['q'])
            hp = _hp(run, sid, m['q'], e['q'])
            ks = sorted(hp)
            if not ks:
                continue
            ys = [(k, hp[k]['pos'][1]) for k in ks]
            top = max(range(len(ys)), key=lambda i: ys[i][1])
            ymin = min(y for _, y in ys)
            seg = [(k, y) for k, y in ys[top:] if y > ymin + 0.05]
            if len(seg) >= 5:
                ff = fits.fit_fall([(k - seg[0][0]) / run.hz for k, _ in seg], [y for _, y in seg])
                x0, x1 = m.get('x') or {}, e.get('x') or {}
                falls.append({'h': x0.get('h'), 'g': ff['g'] if ff else None,
                              'dmg': (x0.get('hp0') or 0) - (x1.get('hp1') or 0) if x1.get('hp1') is not None else None,
                              'dead': x1.get('dead')})
        for m in run.marks('jump'):
            sid = _bot_of(run, m['q'], None)
            hp = _hp(run, sid, m['q'], None)
            ks = [k for k in sorted(hp) if m['k'] <= k <= m['k'] + 2.5 * run.hz]
            if ks:
                y0 = hp[ks[0]]['pos'][1]
                jumps.append(max(hp[k]['pos'][1] for k in ks) - y0)
    nums = {'steps': steps, 'falls': falls, 'jump_apex': stats.median(jumps)}
    out = []
    acc = [s for s in steps if str(s['cfg']).startswith('accel') or s['cfg'] == 'sprint']
    laws = {}
    for s in acc:
        laws[s['law']] = laws.get(s['law'], 0) + 1
    gs = [f['g'] for f in falls if f['g'] is not None]
    g = stats.median(gs)
    if acc:
        best = max(laws, key=laws.get)
        if laws[best] >= 0.8 * len(acc) and best != 'unclear':
            if best == 'exponential':
                gain = stats.median([s['gain30'] for s in acc if s['law'] == 'exponential'])
                txt = 'exponential lag (FractionPerReferenceTick, gain %s at 30 Hz; data AccelerationGain 0.35)' % fmt(gain)
                ch = ('%s: acceleration_model = fraction_per_reference_tick, gain %s, reference_hz 30 (MotorTuning, '
                      'm1.md 6.4)' % (MOTOR_CFG, fmt(gain, 3)))
                v = 'fraction'
            elif best == 'linear':
                a = stats.median([s['a'] for s in acc if s['law'] == 'linear'])
                txt = 'linear acceleration %s m/s^2' % fmt(a)
                ch = '%s: acceleration_model = linear, a = %s m/s^2 (MotorTuning, m1.md 6.4)' % (MOTOR_CFG, fmt(a, 3))
                v = 'linear'
            else:
                txt, v, ch = 'instant speed change', 'instant', '%s: acceleration_model = instant' % MOTOR_CFG
            srow = src_row('S3', 'soldier acceleration law', v, '-', run0.id, _build(run0), run0.hz, len(acc),
                           'RMS exp/linear/instant, winner < 0.5 x next', MOTOR_CFG + ' acceleration_model', 'P 7.2 speeds')
            out.append(Result('S3', txt, v, nums, ch, [srow]))
        else:
            out.append(Result('S3', 'inconclusive motor law (%s)' % laws, 'inconclusive', nums))
    if g is not None:
        gv = 'g=%.2f' % g
        out.append(Result('S3-gravity', 'free-fall gravity %s m/s^2 (%d drops)' % (fmt(g), len(gs)), gv, {'g': gs},
                          '%s: gravity = %s' % (MOTOR_CFG, fmt(g, 4)),
                          [src_row('S3', 'gravity', g, 'm/s^2', run0.id, _build(run0), run0.hz, len(gs),
                                   'quadratic fit of y(t)', MOTOR_CFG + ' gravity', 'P 7.2 jump/fall rows')]))
    return out

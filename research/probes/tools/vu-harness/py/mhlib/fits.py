"""Curve fits for the S5 rules (DESIGN.md section 10), numpy only. Time is always k/hz.

recovery: linear-to-zero p0 - a t (clipped at 0) and exponential p0 e^(-b t); motor: exponential lag, linear ramp,
instant step (with a 0-2 tick input delay); free fall: quadratic; falloff: piecewise linear / smoothstep between
knots, per-bone scale factors, free-knot grid search.
"""

import math

import numpy as np


def _rms(r):
    r = np.asarray(r, dtype=float)
    return float(math.sqrt(float((r * r).mean()))) if len(r) else float('nan')


def golden(f, lo, hi, it=60):
    g = (math.sqrt(5) - 1) / 2
    a, b = lo, hi
    c, d = b - g * (b - a), a + g * (b - a)
    fc, fd = f(c), f(d)
    for _ in range(it):
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - g * (b - a)
            fc = f(c)
        else:
            a, c, fc = c, d, fd
            d = a + g * (b - a)
            fd = f(d)
    x = (a + b) / 2
    return x, f(x)


def grid_then_golden(f, lo, hi, n=200, log=True):
    xs = np.geomspace(lo, hi, n) if log else np.linspace(lo, hi, n)
    vals = [f(x) for x in xs]
    i = int(np.argmin(vals))
    a = xs[max(0, i - 1)]
    b = xs[min(n - 1, i + 1)]
    return golden(f, a, b)


# --- recovery -----------------------------------------------------------------------------------------------------

def fit_linear_to_zero(t, p, eps=1e-9):
    """p = max(0, p0 - a t): least squares line through the non-zero samples; RMS over all samples (incl. clipped)."""
    t = np.asarray(t, dtype=float)
    p = np.asarray(p, dtype=float)
    m = p > eps
    if m.sum() < 2:
        return {'p0': float(p[0]) if len(p) else 0.0, 'a': 0.0, 'rms': float('inf')}
    A = np.stack([np.ones(int(m.sum())), -t[m]], axis=1)
    (p0, a), *_ = np.linalg.lstsq(A, p[m], rcond=None)
    pred = np.maximum(0.0, p0 - a * t)
    return {'p0': float(p0), 'a': float(a), 'rms': _rms(pred - p)}


def fit_exp(t, p):
    """p = p0 e^(-b t): grid on b with closed-form p0; returns dict(p0, b, rms)."""
    t = np.asarray(t, dtype=float)
    p = np.asarray(p, dtype=float)

    def cost(b):
        e = np.exp(-b * t)
        den = float((e * e).sum())
        p0 = float((p * e).sum() / den) if den > 0 else 0.0
        cost.p0 = p0
        return _rms(p0 * e - p)

    b, v = grid_then_golden(cost, 1e-3, 1e3, n=150)
    cost(b)
    return {'p0': cost.p0, 'b': float(b), 'rms': v}


# --- motor --------------------------------------------------------------------------------------------------------

def fit_motor(t, v, vstar, v0=0.0, dt=1 / 30.0):
    """Fit exponential lag, linear ramp and instant step to speeds v(t) after a step at t=0.

    Each model may start after a delay of 0, 1 or 2 ticks. Returns {model: dict(rms, param, delay)}.
    """
    t = np.asarray(t, dtype=float)
    v = np.asarray(v, dtype=float)
    out = {}
    span = vstar - v0
    for name in ('exponential', 'linear', 'instant'):
        best = None
        for dly in (0, 1, 2):
            tt = np.maximum(0.0, t - dly * dt)
            if name == 'exponential':
                def cost(tau):
                    return _rms(vstar - span * np.exp(-tt / tau) - v)
                tau, r = grid_then_golden(cost, 1e-3, 10.0, n=120)
                cand = {'rms': r, 'tau': float(tau), 'delay': dly}
            elif name == 'linear':
                def cost(a):
                    if span >= 0:
                        m = np.minimum(vstar, v0 + a * tt)
                    else:
                        m = np.maximum(vstar, v0 - a * tt)
                    return _rms(m - v)
                a, r = grid_then_golden(cost, 1e-2, 1e3, n=120)
                cand = {'rms': r, 'a': float(a), 'delay': dly}
            else:
                m = np.where(t >= (dly + 1) * dt - 1e-9, vstar, v0)
                cand = {'rms': _rms(m - v), 'delay': dly}
            if best is None or cand['rms'] < best['rms']:
                best = cand
        out[name] = best
    return out


def gain_from_tau(tau, ref_hz=30.0):
    """FractionPerReferenceTick gain equivalent to an exponential lag tau: g = 1 - e^(-1/(ref_hz tau))."""
    return 1 - math.exp(-1.0 / (ref_hz * tau))


def fit_fall(t, y):
    """y = c + b t - g t^2 / 2 -> dict(g, rms)."""
    t = np.asarray(t, dtype=float)
    y = np.asarray(y, dtype=float)
    if len(t) < 4:
        return None
    A = np.stack([np.ones_like(t), t, -0.5 * t * t], axis=1)
    coef, *_ = np.linalg.lstsq(A, y, rcond=None)
    return {'g': float(coef[2]), 'rms': _rms(A @ coef - y), 'n': len(t)}


# --- falloff ------------------------------------------------------------------------------------------------------

def falloff_model(d, knots, shape='linear'):
    """Damage at distance d for knots (start_dmg, end_dmg, start_m, end_m)."""
    sd, ed, s0, s1 = knots
    d = np.asarray(d, dtype=float)
    s = np.clip((d - s0) / (s1 - s0), 0.0, 1.0)
    if shape == 'smooth':
        s = s * s * (3 - 2 * s)
    return sd + (ed - sd) * s


def fit_falloff(d, dmg, bones, knots, shape='linear'):
    """RMS of dmg against the knots model with one least-squares scale factor per bone index."""
    d = np.asarray(d, dtype=float)
    dmg = np.asarray(dmg, dtype=float)
    bones = np.asarray(bones)
    base = falloff_model(d, knots, shape)
    pred = np.empty_like(dmg)
    scales = {}
    for b in sorted(set(bones.tolist())):
        m = bones == b
        den = float((base[m] ** 2).sum())
        sc = float((base[m] * dmg[m]).sum() / den) if den > 0 else 1.0
        scales[int(b)] = sc
        pred[m] = sc * base[m]
    return {'rms': _rms(pred - dmg), 'scales': scales}


def free_end_knot(d, dmg, bones, knots, lo, hi, step=0.5, shape='linear'):
    """Grid over the end distance knot in [lo, hi]: returns (best_end, rms(best), {end: rms})."""
    res = {}
    for e in np.arange(lo, hi + 1e-9, step):
        k = (knots[0], knots[1], knots[2], float(e))
        res[round(float(e), 3)] = fit_falloff(d, dmg, bones, k, shape)['rms']
    best = min(res, key=res.get)
    return best, res[best], res


def free_knots(d, dmg, bones, knots, step=0.5):
    """Grid over both distance knots (damage knots fixed): returns (start, end, rms)."""
    dmax = float(np.max(d))
    best = (knots[2], knots[3], fit_falloff(d, dmg, bones, knots)['rms'])
    for s0 in np.arange(0.0, min(60.0, dmax), step):
        for s1 in np.arange(s0 + step, min(200.0, dmax + 25), step * 4):
            r = fit_falloff(d, dmg, bones, (knots[0], knots[1], float(s0), float(s1)))['rms']
            if r < best[2]:
                best = (float(s0), float(s1), r)
    return best

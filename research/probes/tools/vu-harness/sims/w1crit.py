"""Design-time simulation for the W1 decision rule (DESIGN.md section 10.1), numpy only, run with python -I.

Centre and median-based scale are estimated per model, with a model-free outlier screen (theta > 3 median); the
critical values come from a parametric bootstrap that replicates the whole estimator. Prints the alpha 0.01 critical
values, P(correct verdict) under both true models and the effect of planted outliers for n = 100, 150, 200.
The analysis implementation (py/mhlib/stats.py) must reproduce these numbers within Monte Carlo error.
"""
import numpy as np
rng = np.random.default_rng(1)
M_MED = {'A': 1/np.sqrt(2), 'B': 0.5}
def F(m, x):
    x = np.clip(x, 0, 1)
    return x*x if m == 'A' else x
def draw(m, n, R=1.0):
    u = rng.random(n); a = rng.random(n)*2*np.pi
    r = R*np.sqrt(u) if m == 'A' else R*u
    return np.c_[r*np.cos(a), r*np.sin(a)] + rng.normal(0, 3, 2)  # unknown centre
def stat(p, m):
    c = p.mean(0); th = np.hypot(*(p-c).T)
    keep = th <= 3*np.median(th)
    p2 = p[keep]; c = p2.mean(0); th = np.sort(np.hypot(*(p2-c).T))
    R = np.median(th)/M_MED[m]
    n = len(th); x = F(m, th/R); i = np.arange(1, n+1)
    return max((i/n - x).max(), (x - (i-1)/n).max()), (~keep).sum()
def crit(m, n, B=4000):
    return np.quantile([stat(draw(m, n), m)[0] for _ in range(B)], 0.99)
for n in (100, 150, 200):
    cA, cB = crit('A', n), crit('B', n)
    res = {}
    for true in 'AB':
        ok = 0; both_rej = 0; both_acc = 0
        for _ in range(2000):
            p = draw(true, n)
            dA = stat(p, 'A')[0]; dB = stat(p, 'B')[0]
            accA, accB = dA <= cA, dB <= cB
            if true == 'A' and accA and not accB: ok += 1
            if true == 'B' and accB and not accA: ok += 1
            both_rej += (not accA and not accB); both_acc += (accA and accB)
        res[true] = (ok/2000, both_rej/2000, both_acc/2000)
    # outlier robustness: one point at 1.2R and 3 points at 1.3R
    out = {}
    for k, f in ((1, 1.2), (3, 1.3)):
        ok = 0
        for _ in range(1000):
            p = draw('A', n); c = p.mean(0)
            ang = rng.random(k)*2*np.pi
            p[:k] = c + f*np.c_[np.cos(ang), np.sin(ang)]
            dA = stat(p, 'A')[0]; dB = stat(p, 'B')[0]
            ok += (dA <= cA and dB > cB)
        out[(k, f)] = ok/1000
    print(n, 'crit A %.3f B %.3f' % (cA, cB), 'P(correct|A,B), both-rej, both-acc', res, 'A with outliers', out)

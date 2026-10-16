"""Statistics for the S5 decision rules (DESIGN.md section 10), numpy only.

* KS one-sample statistic against a CDF and the asymptotic Kolmogorov p-value (printed for reference only).
* The W1 estimator of section 10.1 (centre, model-free 3-median screen, median scale per model, KS against the
  clipped model CDF), vectorised over bootstrap replicates; critical values from a parametric bootstrap that
  replicates the whole estimator (sims/w1crit.py gives 0.217/0.216 at n=100, 0.181/0.180 at 150, 0.160/0.161 at 200).
* The secondary W1 statistic (a radius sample normalised by its maximum), Spearman rho, lag-1 autocorrelation,
  least squares with standard errors.
"""

import math

import numpy as np

M_MED = {'A': 1 / math.sqrt(2), 'B': 0.5}   # median of theta / R under uniform-area (A) and uniform-radius (B)


def model_cdf(model, x):
    x = np.clip(x, 0.0, 1.0)
    return x * x if model == 'A' else x


def ks_sorted(x_cdf):
    """KS distance of sorted CDF values x_cdf (1-D) against the uniform: max(i/n - x, x - (i-1)/n)."""
    n = len(x_cdf)
    if n == 0:
        return float('nan')
    i = np.arange(1, n + 1)
    return float(max((i / n - x_cdf).max(), (x_cdf - (i - 1) / n).max()))


def ks_one(sample, cdf):
    """One-sample KS distance of sample against a vectorised CDF function."""
    s = np.sort(np.asarray(sample, dtype=float))
    return ks_sorted(cdf(s))


def kolmogorov_sf(lam):
    """P(K > lam) for the Kolmogorov distribution (series)."""
    if lam <= 0:
        return 1.0
    s = 0.0
    for k in range(1, 101):
        t = 2 * (-1) ** (k - 1) * math.exp(-2 * k * k * lam * lam)
        s += t
        if abs(t) < 1e-12:
            break
    return min(1.0, max(0.0, s))


def ks_pvalue(d, n):
    """Asymptotic p-value with the Stephens correction (reference only; invalid with estimated parameters)."""
    sq = math.sqrt(n)
    return kolmogorov_sf((sq + 0.12 + 0.11 / sq) * d)


# ---------------------------------------------------------------------------------------------------------------------
# W1 estimator (section 10.1), batch form: P has shape (B, n, 2)

def w1_stat_batch(P, model):
    """Return (D, dropped) arrays of shape (B,) for points P (B, n, 2)."""
    P = np.asarray(P, dtype=float)
    B, n, _ = P.shape
    c = P.mean(axis=1, keepdims=True)
    th = np.hypot(P[..., 0] - c[..., 0], P[..., 1] - c[..., 1])
    med = np.median(th, axis=1)
    keep = th <= 3 * med[:, None]
    cnt = keep.sum(axis=1)
    w = keep.astype(float)
    c2 = (P * w[..., None]).sum(axis=1) / np.maximum(cnt, 1)[:, None]
    th2 = np.hypot(P[..., 0] - c2[:, None, 0], P[..., 1] - c2[:, None, 1])
    th2n = np.where(keep, th2, np.nan)
    med2 = np.nanmedian(th2n, axis=1)
    R = med2 / M_MED[model]
    ths = np.sort(np.where(keep, th2, np.inf), axis=1)
    x = model_cdf(model, ths / R[:, None])
    j = np.arange(1, n + 1)[None, :]
    nn = cnt[:, None].astype(float)
    valid = j <= cnt[:, None]
    dplus = np.where(valid, j / nn - x, -np.inf).max(axis=1)
    dminus = np.where(valid, x - (j - 1) / nn, -np.inf).max(axis=1)
    return np.maximum(dplus, dminus), n - cnt


def w1_stat(points, model):
    D, dropped = w1_stat_batch(np.asarray(points, dtype=float)[None, ...], model)
    return float(D[0]), int(dropped[0])


def draw_disc(rng, model, B, n, R=1.0):
    u = rng.random((B, n))
    a = rng.random((B, n)) * 2 * np.pi
    r = R * (np.sqrt(u) if model == 'A' else u)
    centre = rng.normal(0, 3, (B, 1, 2))
    return np.stack([r * np.cos(a), r * np.sin(a)], axis=-1) + centre


_CRIT = {}


def w1_crit(model, n, alpha=0.01, B=4000, seed=1):
    """alpha critical value of w1_stat under model for sample size n (parametric bootstrap of the whole estimator)."""
    key = (model, n, alpha, B, seed)
    if key not in _CRIT:
        rng = np.random.default_rng([seed, n, 0 if model == 'A' else 1])
        vals = []
        for lo in range(0, B, 1000):
            b = min(1000, B - lo)
            D, _ = w1_stat_batch(draw_disc(rng, model, b, n), model)
            vals.append(D)
        _CRIT[key] = float(np.quantile(np.concatenate(vals), 1 - alpha))
    return _CRIT[key]


def w1_secondary_batch(Rr, model):
    """KS of rho = r / max(r) against the model CDF; Rr (B, n)."""
    Rr = np.asarray(Rr, dtype=float)
    mx = Rr.max(axis=1, keepdims=True)
    rho = np.sort(Rr / np.where(mx > 0, mx, 1), axis=1)
    x = model_cdf(model, rho)
    n = Rr.shape[1]
    j = np.arange(1, n + 1)[None, :]
    return np.maximum((j / n - x).max(axis=1), (x - (j - 1) / n).max(axis=1))


_CRIT2 = {}


def w1_secondary_crit(model, n, alpha=0.01, B=4000, seed=2):
    key = (model, n, alpha, B, seed)
    if key not in _CRIT2:
        rng = np.random.default_rng([seed, n, 0 if model == 'A' else 1])
        u = rng.random((B, n))
        r = np.sqrt(u) if model == 'A' else u
        _CRIT2[key] = float(np.quantile(w1_secondary_batch(r, model), 1 - alpha))
    return _CRIT2[key]


# ---------------------------------------------------------------------------------------------------------------------

def rankdata(x):
    x = np.asarray(x, dtype=float)
    order = np.argsort(x, kind='mergesort')
    ranks = np.empty(len(x))
    ranks[order] = np.arange(1, len(x) + 1)
    # average ties
    xs = x[order]
    i = 0
    while i < len(xs):
        j = i
        while j + 1 < len(xs) and xs[j + 1] == xs[i]:
            j += 1
        if j > i:
            ranks[order[i:j + 1]] = (i + j + 2) / 2.0
        i = j + 1
    return ranks


def spearman(x, y):
    if len(x) < 3:
        return float('nan')
    rx, ry = rankdata(x), rankdata(y)
    rx -= rx.mean()
    ry -= ry.mean()
    den = math.sqrt(float((rx * rx).sum() * (ry * ry).sum()))
    return float((rx * ry).sum() / den) if den > 0 else 0.0


def lag1(x):
    x = np.asarray(x, dtype=float)
    if len(x) < 3:
        return float('nan')
    x = x - x.mean()
    den = float((x * x).sum())
    return float((x[:-1] * x[1:]).sum() / den) if den > 0 else 0.0


def linfit(x, y):
    """Least squares y = a + b x: returns dict(b, a, se_b, r2, n, rms)."""
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    n = len(x)
    if n < 2 or float(np.ptp(x)) == 0:
        return None
    xm, ym = x.mean(), y.mean()
    sxx = float(((x - xm) ** 2).sum())
    b = float(((x - xm) * (y - ym)).sum() / sxx)
    a = float(ym - b * xm)
    res = y - (a + b * x)
    sse = float((res ** 2).sum())
    sst = float(((y - ym) ** 2).sum())
    r2 = 1 - sse / sst if sst > 0 else 1.0
    se = math.sqrt(sse / (n - 2) / sxx) if n > 2 else float('inf')
    return {'b': b, 'a': a, 'se_b': se, 'r2': r2, 'n': n, 'rms': math.sqrt(sse / n)}


def median(x):
    x = [v for v in x if v is not None]
    return float(np.median(x)) if x else None

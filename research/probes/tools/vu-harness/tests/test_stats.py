"""Statistics and fits (DESIGN.md section 14): KS statistic and p-value against known values, the W1 bootstrap
critical values of sims/w1crit.py (within Monte Carlo error) and their reproducibility with a seed, fits on exact
synthetic curves."""

import math
import os
import sys
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'py'))

from mhlib import fits, stats  # noqa: E402


class KS(unittest.TestCase):
    def test_known_statistic(self):
        # sample 0.1..0.9 against U(0,1): D = max(i/n - x, x - (i-1)/n) = 0.1 (n = 9, x_i = i/10)
        x = np.arange(1, 10) / 10.0
        self.assertAlmostEqual(stats.ks_one(x, lambda v: v), 0.1, places=6)

    def test_known_pvalue(self):
        # Kolmogorov: P(K > 1.0131) = 0.2563 (series); D = 0.1, n = 100 with the Stephens correction
        self.assertAlmostEqual(stats.ks_pvalue(0.1, 100), 0.2563, places=3)
        self.assertAlmostEqual(stats.kolmogorov_sf(1.36), 0.0494, places=3)

    def test_w1_crit_matches_sim(self):
        sim = {100: (0.217, 0.216), 150: (0.181, 0.180), 200: (0.160, 0.161)}
        for n, (a, b) in sim.items():
            self.assertAlmostEqual(stats.w1_crit('A', n), a, delta=0.012)
            self.assertAlmostEqual(stats.w1_crit('B', n), b, delta=0.012)

    def test_w1_crit_reproducible(self):
        stats._CRIT.clear()
        c1 = stats.w1_crit('A', 120, seed=5)
        stats._CRIT.clear()
        self.assertEqual(c1, stats.w1_crit('A', 120, seed=5))

    def test_w1_power(self):
        rng = np.random.default_rng(3)
        ok = 0
        for model in 'AB':
            for _ in range(20):
                P = stats.draw_disc(rng, model, 1, 200)[0]
                dA, _ = stats.w1_stat(P, 'A')
                dB, _ = stats.w1_stat(P, 'B')
                accA, accB = dA <= stats.w1_crit('A', 200), dB <= stats.w1_crit('B', 200)
                ok += (accA and not accB) if model == 'A' else (accB and not accA)
        self.assertGreaterEqual(ok, 36)

    def test_outlier_screen(self):
        rng = np.random.default_rng(4)
        P = stats.draw_disc(rng, 'A', 1, 200)[0]
        P[0] = P.mean(0) + [10.0, 0.0]
        _, dropped = stats.w1_stat(P, 'A')
        self.assertEqual(dropped, 1)

    def test_spearman_lag1_linfit(self):
        self.assertAlmostEqual(stats.spearman([1, 2, 3, 4], [10, 20, 30, 40]), 1.0)
        self.assertAlmostEqual(stats.spearman([1, 2, 3, 4], [4, 3, 2, 1]), -1.0)
        x = np.array([1.0, -1.0] * 50)
        self.assertLess(stats.lag1(x), -0.9)
        # within-sequence pairs only: two ramps joined end to start would add a large negative boundary pair
        r, n = stats.lag1_within([np.arange(10.0), np.arange(10.0)])
        self.assertEqual(n, 18)
        self.assertGreater(r, 0.6)
        self.assertGreater(r, stats.lag1(np.concatenate([np.arange(10.0), np.arange(10.0)])))
        self.assertEqual(stats.lag1_within([[1.0, 2.0]])[1], 0)
        f = stats.linfit([0, 1, 2, 3], [1, 3, 5, 7])
        self.assertAlmostEqual(f['b'], 2.0)
        self.assertAlmostEqual(f['r2'], 1.0)


class Fits(unittest.TestCase):
    def test_recovery(self):
        t = np.arange(0, 30) / 30.0
        p = np.maximum(0, 2.0 - 8.0 * t)
        lin = fits.fit_linear_to_zero(t, p)
        self.assertAlmostEqual(lin['a'], 8.0, places=6)
        self.assertLess(lin['rms'], 1e-9)
        e = 2.0 * np.exp(-6.0 * t)
        ex = fits.fit_exp(t, e)
        self.assertAlmostEqual(ex['b'], 6.0, places=4)
        self.assertLess(fits.fit_exp(t, p)['rms'], 1.0)
        self.assertGreater(fits.fit_exp(t, p)['rms'], 10 * lin['rms'])

    def test_motor(self):
        dt = 1 / 30.0
        v = [0.0]
        for _ in range(60):
            v.append(v[-1] + 0.35 * (3.6 - v[-1]))
        t = np.arange(len(v)) * dt
        f = fits.fit_motor(t[1:], np.array(v[1:]), 3.6, 0.0, dt)
        self.assertLess(f['exponential']['rms'], 1e-6)
        self.assertAlmostEqual(fits.gain_from_tau(f['exponential']['tau']), 0.35, places=4)
        lin = np.minimum(3.6, 12.0 * np.maximum(0, t - dt))
        f2 = fits.fit_motor(t, lin, 3.6, 0.0, dt)
        self.assertLess(f2['linear']['rms'], 1e-6)

    def test_fall_and_falloff(self):
        t = np.arange(0, 20) / 30.0
        y = 10 - 0.5 * 9.81 * t * t + 0.1 * t
        self.assertAlmostEqual(fits.fit_fall(t, y)['g'], 9.81, places=6)
        d = np.array([5, 10, 20, 30, 40, 50, 60, 75.0])
        k = (25, 18.4, 8, 50)
        dmg = fits.falloff_model(d, k) * 0.9
        r = fits.fit_falloff(d, dmg, np.ones(len(d)), k)
        self.assertLess(r['rms'], 1e-9)
        self.assertAlmostEqual(r['scales'][1], 0.9)
        best, rms, _ = fits.free_end_knot(d, fits.falloff_model(d, (25, 18.4, 8, 42.0)), np.ones(len(d)), k, 30, 60)
        self.assertAlmostEqual(best, 42.0)


if __name__ == '__main__':
    unittest.main()

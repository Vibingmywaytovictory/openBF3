"""synth.py -> extract.py -> analyze.py returns the generating variant for both (all) variants of every rule
(DESIGN.md section 14), 'inconclusive' when n is too small, and survives malformed logs and damaged run folders."""

import contextlib
import io
import json
import os
import random
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'py'))

import analyze  # noqa: E402
import extract  # noqa: E402
import synth  # noqa: E402
from mhlib.rundata import Run  # noqa: E402

# (rule, variant) -> (result item, expected variant)
EXPECT = {
    ('W1', 'area'): ('W1', 'area'), ('W1', 'radius'): ('W1', 'radius'), ('W1', 'small'): ('W1', 'inconclusive'),
    ('W2', '15'): ('W2', '15'), ('W2', '8'): ('W2', '8'),
    ('W2', 'immediate'): ('W2-minrise', 'immediate'), ('W2', 'ramp'): ('W2-minrise', 'ramp'),
    ('W3', 'linear'): ('W3', 'linear'), ('W3', 'exp'): ('W3', 'exp'),
    ('W3', 'correlated'): ('W3-yaw', 'correlated'), ('W3', 'none'): ('W3-yaw', 'none'),
    ('W4', 'release'): ('W4', 'release'), ('W4', 'recovered'): ('W4', 'recovered'),
    ('W4', 'spreadmin'): ('W4', 'spreadmin'), ('W4', 'timer:0.3'): ('W4', 'timer:0.3'),
    ('W5', 'linear'): ('W5 M16A3', 'linear'), ('W5', 'smooth'): ('W5 M16A3', 'smooth'),
    ('W5', '150'): ('W5 M98B end knot', '150'), ('W5', '120'): ('W5 M98B end knot', '120'),
    ('S1', '10'): ('S1', '10'), ('S1', '16'): ('S1', '16'),
    ('S1', 'abort'): ('S1-supp', 'abort'), ('S1', 'no-abort'): ('S1-supp', 'no-abort'),
    ('S3', 'fraction'): ('S3', 'fraction'), ('S3', 'linear'): ('S3', 'linear'),
    ('S3', 'g9.81'): ('S3-gravity', 'g=9.81'), ('S3', 'g15'): ('S3-gravity', 'g=15.00'),
    ('M1', 'A'): ('M1', 'A'), ('M1', 'B'): ('M1', 'B'),
    ('M2', 'model'): ('M2', 'model'), ('M2', 'linear_n'): ('M2', 'linear_n'),
    ('M2', 'lose_get'): ('M2-flip', 'lose_get'), ('M2', 'direct'): ('M2-flip', 'direct'),
    ('T1', 'none'): ('T1', 'none'), ('T1', 'decay'): ('T1', 'decay'), ('T1', 'recovery'): ('T1', 'recovery'),
}


def pipeline(tmp, rule, variant, mangle=None, seed=1):
    logs = os.path.join(tmp, 'logs')
    out = os.path.join(tmp, 'out')
    path = synth.generate(rule, variant, logs, seed=seed)
    if mangle:
        mangle(path)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = extract.main(['--session', '20261016', '--server-logs', logs, '--out', out])
    runs = [Run.load(os.path.join(out, r)) for r in sorted(os.listdir(out))]
    return code, analyze.analyze(runs), out


def variant_of(results, item):
    for r in results:
        if r.item == item:
            return r.variant
    return None


class SynthRules(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mh-synth-')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_every_variant(self):
        for (rule, variant), (item, want) in sorted(EXPECT.items()):
            with self.subTest(rule=rule, variant=variant):
                d = os.path.join(self.tmp, rule + variant.replace(':', '_'))
                code, res, _ = pipeline(d, rule, variant)
                self.assertEqual(code, 0)
                got = variant_of(res, item)
                self.assertEqual(got, want, '%s %s -> %s' % (rule, variant,
                                                             [r.text()[:300] for r in res if r.item == item]))

    def test_every_variant_listed(self):
        for rule, vs in synth.VARIANTS.items():
            for v in vs:
                self.assertIn((rule, v), EXPECT)

    def test_malformed_log_lines(self):
        """garbage, truncated rows and 5 % dropped rows: extract counts them, the verdict stays."""
        def mangle(path):
            with open(path, encoding='utf-8') as f:
                lines = f.read().splitlines()
            rnd = random.Random(7)
            out = []
            for ln in lines:
                if 'MH1' in ln and '"e":"meta"' not in ln and '"e":"mark"' not in ln and rnd.random() < 0.05:
                    continue
                out.append(ln)
                if rnd.random() < 0.002:
                    out.append(ln[:len(ln) // 2])
                if rnd.random() < 0.002:
                    out.append('[x] [VeniceEXT] [mharness] MH1 {"v":1,"run":NaN}')
            out.insert(3, '\x00\x01 garbage line')
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(out) + '\n')
        for rule, variant, item in (('W3', 'exp', 'W3'), ('M1', 'B', 'M1'), ('S1', '16', 'S1')):
            with self.subTest(rule=rule, variant=variant):
                code, res, _ = pipeline(os.path.join(self.tmp, 'm' + rule), rule, variant, mangle)
                self.assertIn(code, (0, 2))
                self.assertEqual(variant_of(res, item), variant)

    def test_damaged_run_folder(self):
        _, _, out = pipeline(os.path.join(self.tmp, 'd'), 'M2', 'model')
        run = os.path.join(out, sorted(os.listdir(out))[0])
        with open(os.path.join(run, 'meta.json'), 'w', encoding='utf-8') as f:
            f.write('{broken')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = analyze.main([run, os.path.join(self.tmp, 'does-not-exist')])
        self.assertEqual(code, 1)
        self.assertIn('cannot read', buf.getvalue())

    def test_rows_missing_fields_do_not_crash(self):
        """rows that pass the schema but are semantically odd (no hits, no gs, empty windows) give no-data verdicts."""
        _, _, out = pipeline(os.path.join(self.tmp, 'e'), 'W1', 'area')
        run = os.path.join(out, sorted(os.listdir(out))[0])
        p = os.path.join(run, 'raw.jsonl')
        with open(p, encoding='ascii') as f:
            rows = [json.loads(x) for x in f]
        rows = [r for r in rows if r['e'] not in ('hit', 'gs')]
        with open(p, 'w', encoding='ascii', newline='\n') as f:
            f.write(''.join(json.dumps(r) + '\n' for r in rows))
        res = analyze.analyze([Run.load(run)])
        self.assertEqual(variant_of(res, 'W1'), 'inconclusive')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(analyze.main([run, '--out', os.path.join(self.tmp, 'sum')]), 0)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, 'sum', 'summary.json')))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, 'sum', 'sources-draft.md')))


if __name__ == '__main__':
    unittest.main()

"""extract.py on tiny fixture logs (DESIGN.md sections 9 and 14): formats, malformed lines, PII guard, run split,
idempotence, refusals, gzip threshold, dry-run resume hints, exit codes."""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'py'))

import extract  # noqa: E402
from mhlib import logparse, runs, schema  # noqa: E402

RUN = '20261016-WA-30-213000'
SRUN = '20261016-S-30-210000'


def row(e, q, run=RUN, sc='WA', r='S', k=None, **payload):
    d = {'v': 1, 'run': run, 'sc': sc, 'r': r, 'k': q if k is None else k, 't': q / 30.0, 'ms': 1000000 + q, 'q': q,
         'e': e}
    d.update(payload)
    return d


def line(d, prefix='[21:30:00.000] [info] [VeniceEXT] [mharness] '):
    return prefix + 'MH1 ' + json.dumps(d, separators=(',', ':'))


def good_rows():
    out = [row('meta', 1, run=SRUN, sc='S', hv='0.2.0', lvl='Levels/MP_Subway/MP_Subway', mode='ConquestSmall0', rnd=0,
               rpm=0, hz=30.0, freq='regular', vars={'vars.soldierHealth': '100'}, mods=['mharness'], build=20682)]
    out.append(row('meta', 2, hv='0.2.0', lvl='Levels/MP_Subway/MP_Subway', mode='ConquestSmall0', rnd=0, rpm=800, hz=30,
                   freq='regular', vars={}, mods=['mharness']))
    out.append(row('mark', 3, lbl='cfg-start', cfg='M16A3-stand-hip', i=1, n=17))
    out.append(row('shot', 4, id=3, wid=1, kind='bullet', uid=7001, pos=[1.5, 1.6, 45.0], dir=[0.0, 0.0, 1.0]))
    out.append(row('hit', 5, uid=7001, pos=[0.01, 1.62, 60.0], n=[0, 0, -1], bone=-1, part=-1, mat=[516, 2, 5]))
    out.append(row('mark', 6, lbl='cfg-done', cfg='M16A3-stand-hip', i=1, n=17))
    out.append(row('mark', 7, lbl='cfg-done', cfg='M16A3-stand-ads', i=2, n=17))
    return out


class ParseTests(unittest.TestCase):
    def test_limits(self):
        for text, reason in [('{"a":NaN}', 'nan'), ('{"a":1,"a":2}', 'dup_key'), ('[1]', 'not_object'),
                             ('{"a":' * 10 + '1' + '}' * 10, 'too_deep'), ('{"a":', 'json'),
                             ('{' + ','.join('"k%d":1' % i for i in range(70)) + '}', 'too_many_keys'),
                             ('{"a":Infinity}', 'nan')]:
            with self.assertRaises(logparse.Malformed) as cm:
                logparse.parse_json(text)
            self.assertEqual(cm.exception.reason, reason, text[:40])

    def test_long_line_and_utf8(self):
        data = (b'x' * (100 * 1024) + b'\n' + line(good_rows()[2]).encode() + b'\n' + b'\xff\xfe MH1 {"a":1}\n')
        counts = {}
        rows, _ = logparse.scan(io.BytesIO(data), counts)
        self.assertEqual(counts.get('too_long'), 1)
        self.assertEqual(counts.get('bad_utf8'), 1)
        self.assertEqual(len(rows), 2)
        self.assertIsNotNone(rows[0][2])

    def test_header(self):
        data = b'[21:00:00] Build 20682\n[21:00:00] VeniceEXT engine v1.15.0\n'
        _, h = logparse.scan(io.BytesIO(data), {})
        self.assertEqual(h, {'build': 20682, 'vext': '1.15.0'})


class SchemaTests(unittest.TestCase):
    def setUp(self):
        self.s = schema.Schema()

    def test_good(self):
        for r in good_rows():
            ok, why = self.s.validate(r)
            self.assertIsNone(why, r)

    def test_bad(self):
        base = good_rows()[3]
        cases = [
            (dict(base, extra=1), 'unknown_key'),
            (dict((k, v) for k, v in base.items() if k != 'q'), 'envelope'),
            (dict(base, e='nope'), 'unknown_e'),
            (dict(base, id='3'), 'bad_type'),
            (dict(base, pos=[1, 2]), 'bad_type'),
            (dict(base, uid=True), 'bad_type'),
            (dict(base, kind='laser'), 'bad_enum'),
            (dict(base, td='6E1D7C2A4B3F4A8E9C1D2F5B8E7A9C40'), 'pii_suspect'),
            (dict(base, td='6E1D7C2A-4B3F-4A8E-9C1D-2F5B8E7A9C40'), 'pii_suspect'),
            (dict(base, td='10.0.0.12'), 'pii_suspect'),
            (dict(base, td='bad\u00e9label'), 'bad_type'),
            (dict(base, run='2026-WA'), 'envelope'),
            (dict(base, t=float('inf')), 'envelope'),
        ]
        for r, want in cases:
            ok, why = self.s.validate(r)
            self.assertEqual(why, want, r)
        m = row('mark', 9, lbl='user:x', x={'ip': '1'})
        self.assertEqual(self.s.validate(m)[1], 'pii_suspect')
        g = row('gs', 9, id=1, wid=1, src='srv', da=None, dmin=0.1, rec=[0, 0, 0, 0], disp=[0, 0], ra=0, rr=0, seed=1,
                ifs=0, fs=True, fir=False, tsls=0, mov=False, jmp=False, jt=0, pose=0, zoom=False)
        self.assertEqual(self.s.validate(g)[1], 'non_finite')
        g2 = dict(g, da=1.0, lag=None)
        out, why = self.s.validate(g2)
        self.assertIsNone(why)
        self.assertNotIn('lag', out)


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mh-extract-')
        self.slog = os.path.join(self.tmp, 'srvlogs')
        self.clog = os.path.join(self.tmp, 'clilogs')
        os.makedirs(self.slog)
        os.makedirs(self.clog)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_logs(self, extra_server=(), n_fill=0):
        lines = ['[21:00:00.000] [info] Build 20682', '[21:00:00.001] [info] VeniceEXT engine v1.15.0',
                 '[21:00:01.000] [info] mharness: join with the client console: connect <guid> <pw>']
        rows = good_rows()
        for r in rows[:3]:
            lines.append(line(r))
        lines.append('[21:00:02.000] [info] [VeniceEXT] something else MH1 is mentioned here')
        for r in rows[3:]:
            lines.append(line(r))
        q = 10
        for i in range(n_fill):
            lines.append(line(row('ev', q, name='fill', id=i)))
            q += 1
        lines.extend(extra_server)
        with open(os.path.join(self.slog, 'vu_2026-10-16_21-00-00.log'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        crows = [row('fov', 1, r='C', zoom=0, vfov=55.0, hfov=86.0, w=2560, h=1440),
                 row('gs', 3, r='C', id=0, wid=1, src='pred', da=0.5, dmin=0.5, rec=[0, 0, 0, 0], disp=[0, 0], ra=0,
                     rr=0, seed=1, ifs=0, fs=True, fir=False, tsls=1.5, mov=False, jmp=False, jt=0, pose=0, zoom=False)]
        with open(os.path.join(self.clog, 'client.log'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(line(r, prefix='[21:30:00] [VeniceEXT] [mharness] ') for r in crows) + '\n')
            f.write('unrelated.txt line\n')
        with open(os.path.join(self.clog, 'notes.txt'), 'w', encoding='utf-8') as f:
            f.write(line(row('ev', 99, name='ignored')) + '\n')

    def run_extract(self, *extra):
        out = os.path.join(self.tmp, 'out')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = extract.main(['--session', '20261016', '--server-logs', self.slog, '--client-logs', self.clog,
                                 '--out', out] + list(extra))
        return code, buf.getvalue(), out

    def test_split_and_idempotent(self):
        self.write_logs()
        code, text, out = self.run_extract()
        self.assertEqual(code, 0, text)
        self.assertEqual(sorted(os.listdir(out)), sorted([RUN, SRUN]))
        rows, meta = runs.read_run(os.path.join(out, RUN))
        self.assertEqual(meta['vu_build'], 20682)
        self.assertEqual(meta['veniceext'], '1.15.0')
        self.assertEqual(meta['counts'].get('fov'), 1)
        self.assertEqual([r['r'] for r in rows][:6], ['C', 'C', 'S', 'S', 'S', 'S'])
        self.assertEqual(sorted(s['name'] for s in meta['sources']), ['client.log', 'vu_2026-10-16_21-00-00.log'])
        self.assertNotIn('notes.txt', json.dumps(meta))
        with open(os.path.join(out, RUN, 'raw.jsonl'), 'rb') as f:
            first = f.read()
        with open(os.path.join(out, RUN, 'meta.json'), 'rb') as f:
            first_meta = f.read()
        code, text, _ = self.run_extract()
        self.assertIn('exists, skipped', text)
        code, text, _ = self.run_extract('--force')
        with open(os.path.join(out, RUN, 'raw.jsonl'), 'rb') as f:
            self.assertEqual(f.read(), first)
        with open(os.path.join(out, RUN, 'meta.json'), 'rb') as f:
            self.assertEqual(f.read(), first_meta)
        self.assertNotIn(self.tmp.encode(), first_meta)

    def test_malformed_and_exit2(self):
        bad = [
            '[x] [VeniceEXT] [mharness] MH1 {"v":1,"run":"' + RUN + '","sc":"WA"',     # truncated -> no_match
            '[x] [VeniceEXT] [mharness] MH1 ' + json.dumps(row('ev', 50, name='x', a={'k': float('nan')})),
            '[x] [VeniceEXT] [mharness] MH1 {"v":1,"v":1}',
            line(row('ev', 51, name='x', id='str')),
            line(row('ev', 52, name='x', bogus=1)),
            line(row('cmd', 53, who=1, verb='run', ok=True, args='192.168.1.20')),
            line(row('zzz', 54)),
            'MH1 ' + json.dumps(row('ev', 55, name='bare_ok')),
        ]
        self.write_logs(extra_server=bad)
        code, text, out = self.run_extract()
        self.assertEqual(code, 2, text)
        rows, meta = runs.read_run(os.path.join(out, RUN))
        mal = meta['malformed']
        for reason in ('nan', 'bad_type', 'unknown_key', 'pii_suspect', 'unknown_e'):
            self.assertGreaterEqual(mal.get(reason, 0), 1, (reason, mal))
        self.assertIn('no_match', text)
        self.assertIn('dup_key', text)
        self.assertTrue(any(r.get('name') == 'bare_ok' for r in rows))

    def test_one_percent_rule(self):
        # 1 malformed row among > 100 good ones -> exit 0
        self.write_logs(extra_server=[line(row('ev', 900, name='x', bogus=1))], n_fill=150)
        code, text, _ = self.run_extract()
        self.assertEqual(code, 0, text)

    def test_gaps_and_dry_run(self):
        self.write_logs(extra_server=[line(row('ev', 40, name='after_gap'))])
        code, text, out = self.run_extract('--dry-run')
        self.assertEqual(code, 0)
        self.assertFalse(os.path.exists(out))
        self.assertIn('WA last cfg-done 2/17 -> !mh run WA from=3, queue item WA:all:from=3', text)
        self.assertIn("'S': 32", text)

    def test_gzip(self):
        self.write_logs(n_fill=50)
        old = runs.GZIP_ABOVE
        runs.GZIP_ABOVE = 1000
        try:
            code, text, out = self.run_extract()
        finally:
            runs.GZIP_ABOVE = old
        self.assertTrue(os.path.exists(os.path.join(out, RUN, 'raw.jsonl.gz')))
        rows, meta = runs.read_run(os.path.join(out, RUN))
        self.assertEqual(meta['raw']['file'], 'raw.jsonl.gz')
        self.assertGreater(len(rows), 50)

    def test_refusals(self):
        self.write_logs()
        for args in (['--server-logs', r'C:\Program Files\EA Games\Battlefield 3'],
                     ['--server-logs', r'C:\Users\olku\Documents\Battlefield 3\Server\logs'],
                     ['--server-logs', r'C:\Users\olku\Documents\Battlefield 3\Server']):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = extract.main(['--session', '20261016', '--out', os.path.join(self.tmp, 'o')] + args)
            self.assertEqual(code, 1)
            self.assertIn('refused input', buf.getvalue())
        for out in (os.path.join(self.slog, 'x'), r'C:\Program Files\EA Games\Battlefield 3\m'):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = extract.main(['--session', '20261016', '--server-logs', self.slog, '--out', out])
            self.assertEqual(code, 1)
            self.assertIn('refused output', buf.getvalue())
        # a sibling of the normal instance whose name merely starts with Server is not refused by the path rule
        self.assertIsNone(extract.refused_input(r'C:\Users\olku\Documents\Battlefield 3\ServerMeasure\logs'))

    def test_session_range_cross_midnight(self):
        # a run id carries its START date: overnight and morning runs of an evening session are 20261017-*
        late = '20261017-SB-30-003000'
        self.write_logs(extra_server=[line(row('ev', 61, run=late, sc='SB', name='after_midnight'))])
        code, text, out = self.run_extract()
        self.assertEqual(code, 0, text)
        self.assertNotIn(late, os.listdir(out))
        self.assertIn('WARNING 1 rows of runs outside --session 20261016', text)
        self.assertIn('20261017 1 rows', text)
        buf = io.StringIO()
        out2 = os.path.join(self.tmp, 'out2')
        with contextlib.redirect_stdout(buf):
            code = extract.main(['--session', '20261016..20261017', '--server-logs', self.slog, '--client-logs',
                                 self.clog, '--out', out2])
        self.assertEqual(code, 0, buf.getvalue())
        self.assertEqual(sorted(os.listdir(out2)), sorted([RUN, SRUN, late]))
        self.assertNotIn('WARNING', buf.getvalue())

    def test_session_parse(self):
        self.assertEqual(extract.parse_session('20261016'), [('20261016', '20261016')])
        self.assertEqual(extract.parse_session('20261016..20261017,20261020'),
                         [('20261016', '20261017'), ('20261020', '20261020')])
        for bad in ('2026101', '20261017..20261016', '20261332', 'x', '', '20261016..'):
            self.assertIsNone(extract.parse_session(bad), bad)

    def test_session_filter_and_manifests(self):
        self.write_logs(extra_server=[line(row('ev', 60, run='20261015-WA-30-213000', name='old'))])
        mb = os.path.join(self.tmp, 'mb.csv')
        with open(mb, 'w', encoding='utf-8') as f:
            f.write('"Path","Dir","Length","LastWriteUtc","CreationUtc","Sha256"\n".","True","","a","b",""\n')
        code, text, out = self.run_extract('--manifest-before', mb, '--manifest-after', mb)
        self.assertEqual(sorted(os.listdir(out)), sorted([RUN, SRUN]))
        self.assertIn('install unchanged: 1 items', text)
        _, meta = runs.read_run(os.path.join(out, RUN))
        self.assertEqual(meta['manifests']['compare'], 'install unchanged: 1 items')


if __name__ == '__main__':
    unittest.main()

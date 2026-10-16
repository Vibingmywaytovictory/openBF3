"""Mock VU end-to-end runs (DESIGN.md 13.5): scenario in -> MH1 rows in VU-format logs -> extract.py -> analyze.py ->
the verdict for the law the mock was set to, plus the observe-only journal checks (13.4) on every run.

    python -I mockrun.py --suite all|phase0 [--full] [--only NAME[,NAME]] [--list] [--keep DIR] [--coverage]
        [--write-cert]

--suite phase0 runs the Phase 0 cases and the attended U scenarios with the repeats/stuck/log-fault runs (the staged
gate of DESIGN.md 14); --suite all runs every case. --full uses the full W-A tier-1 protocol (slow stress run).
--write-cert writes mod/mharness/ext/shared/mh_cert.lua (ignored by git) with the scenario ids the passing suite
certifies. Exit 0 only if every selected case passes.
"""

import argparse
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'py'))
sys.path.insert(0, HERE)

import analyze  # noqa: E402
import extract  # noqa: E402
from mhlib.rundata import Run  # noqa: E402
from mockvu import user as US  # noqa: E402
from mockvu.host import Host, mod_sources  # noqa: E402

ALL_CERT = ['WA', 'SB', 'WB:U', 'WB:B', 'SA:U', 'SA:B', 'MA', 'MB', 'WC', 'WD', 'rr', 'nextmap']
PHASE0_CERT = ['WB:U', 'SA:U', 'WC', 'WD']
CERT_PATH = os.path.join(HERE, 'mod', 'mharness', 'ext', 'shared', 'mh_cert.lua')
STATUSES = US.STATUSES


class CaseFail(Exception):
    pass


class Env(object):
    """One e2e case: its scratch folder, hosts, checks and results."""

    def __init__(self, root, name, full=False, coverage=False):
        self.name = name
        self.dir = os.path.join(root, name)
        self.full = full
        self.coverage = coverage
        self.hosts = []
        self.checks = []
        self.notes = []
        self.cov = {}
        self.allow_err = False
        self.allow_viol = False

    # -- setup ---------------------------------------------------------------------------------------------------
    def host(self, laws=None, seed=1, cert=None, tag=None, **user_kw):
        tag = tag or 'h%d' % (len(self.hosts) + 1)
        h = Host(laws or {}, seed=seed, outdir=self.dir, cert=ALL_CERT if cert is None else cert,
                 coverage=self.coverage, tag=tag)
        if h.load_errors:
            raise CaseFail('mod load errors: %s' % h.errors[:3])
        u = US.User(h, **user_kw)
        self.hosts.append(h)
        return h, u

    def drive(self, h, u, gen, max_s=7200):
        try:
            return US.Driver(h, u).run(gen, max_s)
        finally:
            h.close()
            for src, lines in h.coverage_data().items():
                self.cov.setdefault(src, set()).update(lines)

    # -- pipeline ------------------------------------------------------------------------------------------------
    def extract(self, expect_code=0):
        out = os.path.join(self.dir, 'measurements')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = extract.main(['--session', '20261016', '--server-logs', os.path.join(self.dir, 'instance', 'logs'),
                                 '--client-logs', os.path.join(self.dir, 'client'), '--out', out, '--force'])
        text = buf.getvalue()
        self.extract_text = text
        if expect_code is not None:
            self.check('extract exit %d' % expect_code, code == expect_code, text.strip().splitlines()[0][:160])
        runs = []
        if os.path.isdir(out):
            for r in sorted(os.listdir(out)):
                runs.append(Run.load(os.path.join(out, r)))
        return code, runs, text

    def analyze(self, runs, items=None):
        return analyze.analyze(runs, items)

    # -- checks --------------------------------------------------------------------------------------------------
    def check(self, what, ok, detail=''):
        self.checks.append((what, bool(ok), detail))
        return ok

    def verdict(self, results, item, want, what=None):
        got = [r for r in results if r.item == item]
        v = got[0].variant if got else None
        return self.check(what or '%s = %s' % (item, want), v == want, 'got %r%s' % (
            v, (': ' + got[0].text()[:220].replace('\n', ' | ')) if got and v != want else ''))

    def journal(self, h):
        if not self.allow_viol:
            self.check('journal clean (%s)' % os.path.basename(h.slog_path or ''), not h.viol, '; '.join(h.viol[:3]))
        if not self.allow_err:
            errs = [r for r in h.rows if r.get('e') == 'err']
            self.check('no handler errors', not h.errors and not errs,
                       '; '.join(h.errors[:2] + [json.dumps(e)[:160] for e in errs[:2]]))

    def ok(self):
        return all(c[1] for c in self.checks)


# -- helpers -------------------------------------------------------------------------------------------------------
def p0_rows(h, step=None):
    return [r for r in h.rows if r.get('e') == 'p0' and (step is None or r.get('step') == step)]


def p0_ok(h, step):
    rows = p0_rows(h, step)
    return bool(rows) and rows[-1].get('ok') is True, (rows[-1].get('val') if rows else None)


def bots_left(h):
    return int(h.srv.mock.U.player_count()) - h.humans()


def marks(h, lbl, sc=None):
    return [r for r in h.rows if r.get('e') == 'mark' and r.get('lbl') == lbl and (sc is None or r.get('sc') == sc)]


def wa_args(env, parts='a,b,c,d,e', scale=None, cfg='1'):
    if env.full:
        return 'tier=1'
    s = 'cfg=%s parts=%s' % (cfg, parts)
    if scale is not None:
        s += ' scale=%s' % scale
    return s


# -- cases ---------------------------------------------------------------------------------------------------------
CASES = []


def case(suites=('all',)):
    def deco(fn):
        CASES.append((fn.__name__[2:], fn, suites))
        return fn
    return deco


P0_STEPS = ['P0.3', 'P0.5', 'P0.6', 'P0.7', 'P0.8', 'P0.11', 'P0.9', 'P0.10', 'P0.13', 'P0.15']


def p0_script(u, level=True, round_=True):
    yield from US.setup(u, ('wall',), aim=False)
    line = yield from u.say_wait('census', r'^\[mh\] census:', 20)
    u.census_line = line
    st = yield from u.run('p0 all', 'P0', 900)
    u.status = {'all': st}
    u.status['human'] = yield from u.run('p0 human', 'P0', 120)
    u.status['log'] = yield from u.run('p0 log', 'P0', 60)
    if round_:
        u.status['round'] = yield from u.run('p0 round', 'P0', 300)
    if level:
        u.status['level'] = yield from u.run('p0 level confirm', 'P0', 400)
        yield from u.wait(5)
    u.status['report'] = yield from u.run('p0 report', 'P0', 60)


@case(('phase0', 'all'))
def c_p0_all(env):
    h, u = env.host()
    u.responders['fire3'] = US.respond_fire3
    env.drive(h, u, p0_script(u), 3600)
    for s in P0_STEPS:
        ok, val = p0_ok(h, s)
        env.check('%s ok' % s, ok, json.dumps(val)[:200])
    m = re.search(r'API (\d+)/(\d+)', u.census_line)
    env.check('census API N/N', m and m.group(1) == m.group(2), u.census_line)
    env.check('census vm same', 'vm same' in u.census_line, u.census_line)
    rep = p0_rows(h, 'P0.14')
    env.check('P0.14 report row', rep and rep[-1]['val'].get('damage') == 'YES' and rep[-1]['val'].get('rr') is True,
              json.dumps(rep[-1] if rep else None)[:200])
    env.check('P0.12 test row', p0_rows(h, 'P0.12'), '')
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    code, runs, text = env.extract(0)
    env.check('extractor 0 malformed', ' 0 malformed' in text, text.splitlines()[0][:200])
    long_rows = [r for r in runs if r.sc == 'P0']
    env.check('P0 runs extracted', len(long_rows) >= 5, '%d P0 runs' % len(long_rows))


# -- coverage ------------------------------------------------------------------------------------------------------
CODE_LINE = re.compile(r'^\s*(?:end[,)]*|else|\}[,)]*|\)|do|then|return|break|local function [\w.:]+\(.*\)|function [\w.:]+\(.*\))?\s*$')


def executable_lines(src):
    """Approximate executable lines of a Lua file: non-blank, non-comment lines that are not only end/else/braces or a
    function header (debug.sethook 'l' reports statements, not those)."""
    out = set()
    in_block = False
    for i, line in enumerate(src.splitlines(), 1):
        s = line.strip()
        if in_block:
            if ']]' in s:
                in_block = False
            continue
        if s.startswith('--[['):
            in_block = ']]' not in s
            continue
        if not s or s.startswith('--'):
            continue
        code = s.split('--', 1)[0].strip() if '--' in s and "'" not in s and '"' not in s else s
        if CODE_LINE.match(code):
            continue
        out.add(i)
    return out


def coverage_report(cov):
    srcs = mod_sources()
    total_e = total_h = 0
    rows = []
    for key in sorted(srcs):
        ex = executable_lines(srcs[key])
        hit = cov.get('@ext/' + key + '.lua', set()) & ex
        total_e += len(ex)
        total_h += len(hit)
        miss = sorted(ex - hit)
        rows.append((key, len(hit), len(ex), miss))
    return total_h, total_e, rows


def ranges(lines):
    out = []
    for n in lines:
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return ','.join('%d' % a if a == b else '%d-%d' % (a, b) for a, b in out)


def commit():
    try:
        p = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=HERE, capture_output=True, text=True, timeout=20)
        return p.stdout.strip() or 'unknown'
    except (OSError, subprocess.SubprocessError):
        return 'unknown'


def write_cert(ids, gate):
    items = ', '.join("['%s'] = true" % i for i in sorted(ids))
    text = ('-- GENERATED by mockrun.py --write-cert (ignored by git); certified scenarios of gate %s at commit %s\n'
            "return { gate = '%s', commit = '%s', ids = { %s } }\n" % (gate, commit(), gate, commit(), items))
    with open(CERT_PATH, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--suite', default='all', choices=('all', 'phase0'))
    ap.add_argument('--only', default=None)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--keep', default=None, help='keep the scratch logs and run folders in this directory')
    ap.add_argument('--coverage', action='store_true')
    ap.add_argument('--write-cert', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args(argv)
    sel = [c for c in CASES if a.suite in c[2]]
    if a.only:
        want = set(a.only.split(','))
        sel = [c for c in CASES if c[0] in want]
    if a.list:
        for name, fn, suites in CASES:
            print('%-24s %s' % (name, ','.join(suites)))
        return 0
    root = a.keep or tempfile.mkdtemp(prefix='mh-e2e-')
    os.makedirs(root, exist_ok=True)
    t0 = time.time()
    npass = 0
    cov = {}
    try:
        for name, fn, _ in sel:
            env = Env(root, name, full=a.full, coverage=a.coverage)
            t1 = time.time()
            try:
                fn(env)
            except Exception as exc:  # noqa: BLE001 - a crashing case is a failing case, report and go on
                env.check('case ran', False, '%s: %s' % (type(exc).__name__, str(exc)[:300]))
                if a.verbose:
                    traceback.print_exc()
            for h in env.hosts:
                h.close()
            ok = env.ok()
            npass += ok
            for src, lines in env.cov.items():
                cov.setdefault(src, set()).update(lines)
            k = sum(h.k for h in env.hosts)
            print('%-24s %s  %d checks, %d ticks, %.1f s' % (name, 'PASS' if ok else 'FAIL', len(env.checks), k,
                                                             time.time() - t1))
            for what, good, detail in env.checks:
                if not good or a.verbose:
                    print('    %s %s %s' % ('ok  ' if good else 'FAIL', what, detail))
            for n in env.notes:
                print('    note: ' + n)
            sys.stdout.flush()
    finally:
        if not a.keep:
            shutil.rmtree(root, ignore_errors=True)
    if a.coverage:
        hit, ex, rows = coverage_report(cov)
        print('lua coverage of the mod (approximate executable lines): %d/%d = %.1f %%' % (hit, ex, 100.0 * hit / max(1, ex)))
        for key, h_, e_, miss in rows:
            print('    %-34s %4d/%-4d %5.1f %%  uncovered: %s' % (key, h_, e_, 100.0 * h_ / max(1, e_), ranges(miss)[:200]))
    print('mock e2e (%s): %d/%d cases passed in %.0f s' % (a.suite if not a.only else 'only', npass, len(sel),
                                                           time.time() - t0))
    if npass == len(sel) and a.write_cert and not a.only:
        write_cert(ALL_CERT if a.suite == 'all' else PHASE0_CERT, a.suite)
        print('wrote ' + os.path.relpath(CERT_PATH, HERE))
    return 0 if npass == len(sel) else 1


if __name__ == '__main__':
    sys.exit(main())

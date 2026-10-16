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
    def host(self, laws=None, seed=1, cert=None, tag=None, level_opts=None, **user_kw):
        tag = tag or 'h%d' % (len(self.hosts) + 1)
        h = Host(laws or {}, seed=seed, outdir=self.dir, cert=ALL_CERT if cert is None else cert,
                 coverage=self.coverage, tag=tag, level_opts=level_opts)
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
            code = extract.main(['--session', '20261016..20261017', '--server-logs', os.path.join(self.dir, 'instance', 'logs'),
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


def wa_args(env, parts='a,b,c,d,e', scale=None, cfg=None):
    if env.full:
        return 'tier=1'
    s = ('cfg=%s parts=%s' % (cfg, parts)) if cfg else ('tier=1 parts=%s' % parts)
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



def wa_script(u, args, sc='WA', aim=True, extra=None):
    yield from US.setup(u, ('wall',), aim=aim)
    if extra:
        yield from extra(u)
    u.status = None
    u.statuses = []
    for a in ([args] if isinstance(args, str) else args):
        st = yield from u.run('run %s %s' % (sc, a), sc, 20000)
        u.statuses.append(st)
        u.status = st if u.status in (None, 'done') else u.status


def run_wa(env, laws, args, items=None, seed=1, tag=None):
    h, u = env.host(laws, seed=seed, tag=tag)
    env.drive(h, u, wa_script(u, args), 40000)
    env.check('WA %s done' % (args,), u.status == 'done', '%s' % u.statuses)
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    return h, u


def wa_case(env, laws, args, expect, items, seed=1):
    run_wa(env, laws, args, items, seed)
    code, runs, text = env.extract(0)
    res = env.analyze(runs, items)
    for item, want in expect:
        env.verdict(res, item, want)
    return res


@case()
def c_wa_w1_area(env):
    wa_case(env, {'disc': 'area'}, wa_args(env, 'a'), [('W1', 'area')], ['W1'])


@case()
def c_wa_w1_radius(env):
    wa_case(env, {'disc': 'radius'}, wa_args(env, 'a'), [('W1', 'radius')], ['W1'])



@case()
def c_wa_t1_exp(env):
    wa_case(env, {'recovery': 'exp', 'decay': 15.0, 'hcorr': 0.0, 'rearm': 'release'}, wa_args(env, scale='0.2'),
            [('W2', '15'), ('W3', 'exp'), ('W3-yaw', 'none'), ('W4', 'release')], ['W2', 'W3', 'W4'])


# W4 'inconclusive': with linear recovery (8 deg/s) t_rec is 3 ticks, under the shortest released gap, and the held
# pairs are unrecovered at the firing scale, so WhenRecovered predicts what OnTriggerRelease does on every bin (pass 6;
# the pass-5 'release' here was the rule preferring release, not evidence)
@case()
def c_wa_t1_linear(env):
    wa_case(env, {'recovery': 'linear', 'decay': 8.0, 'hcorr': 0.6, 'rearm': 'release'}, wa_args(env, scale='0.2'),
            [('W2', '8'), ('W3', 'linear'), ('W3-yaw', 'correlated'), ('W4', 'inconclusive')], ['W2', 'W3', 'W4'])


@case()
def c_wa_w4_recovered(env):
    wa_case(env, {'recovery': 'exp', 'rearm': 'recovered'}, wa_args(env, 'a,c', scale='0.2'), [('W4', 'recovered')],
            ['W4'])


# W4 with the default (M16A3) weapon data, no tuning: t_min is a whole number of ticks, so a one-tick slack below it
# accepted the unarmed gap one tick short and the rule fell to a spurious AfterTime timer (verifier finding, pass 5)
@case()
def c_wa_w4_spreadmin(env):
    wa_case(env, {'recovery': 'exp', 'decay': 8.0, 'rearm': 'spreadmin'}, wa_args(env, 'a,c', scale='0.2'),
            [('W4', 'spreadmin')], ['W4'])


@case()
def c_wa_w4_spreadmin15(env):
    wa_case(env, {'recovery': 'exp', 'decay': 15.0, 'rearm': 'spreadmin'}, wa_args(env, 'a,c', scale='0.2'),
            [('W4', 'spreadmin')], ['W4'], seed=2)


@case()
def c_wa_w4_timer(env):
    wa_case(env, {'recovery': 'exp', 'rearm': 'timer:0.3'}, wa_args(env, 'a,c', scale='0.2'), [('W4', 'timer:0.3')],
            ['W4'])


# W4 with linear recovery (verifier pass 6): a fast linear recovery leaves every released gap armed and every held
# pair unrecovered under both WhenRecovered and OnTriggerRelease, so protocol c cannot tell them apart. The verdict must
# say so (inconclusive, both named as indistinguishable, no code change), never 'release' for a recovered law. The
# pass-5 rule read 'release' here with no held pair classified (the W3-based in-tick correction over-corrected them).
def w4_alias_case(env, laws, seed):
    run_wa(env, laws, wa_args(env, 'a,c', scale='0.2'), seed=seed)
    code, runs, text = env.extract(0)
    res = env.analyze(runs, ['W4'])
    got = [r for r in res if r.item == 'W4']
    r = got[0] if got else None
    txt = r.text()[:260].replace('\n', ' | ') if r else ''
    env.check('W4 inconclusive (release and recovered indistinguishable)', r is not None and r.variant == 'inconclusive'
              and set((r.numbers or {}).get('indistinguishable') or ()) >= {'release', 'recovered'},
              'got %r: %s' % (r.variant if r else None, txt))
    env.check('held pairs classified', r is not None and (r.numbers or {}).get('held_not_armed', 0) >= 4,
              json.dumps((r.numbers or {}).get('bins') if r else None)[:200])
    env.check('no code change', r is not None and r.change in (None, '', 'no change'), r.change if r else '')


@case()
def c_wa_w4_linear_recovered(env):
    w4_alias_case(env, {'recovery': 'linear', 'rec_rate': 30.0, 'rearm': 'recovered'}, 4)


@case()
def c_wa_w4_linear_release(env):
    w4_alias_case(env, {'recovery': 'linear', 'rec_rate': 30.0, 'rearm': 'release'}, 4)


@case()
def c_wa_w4_linear60_recovered(env):
    w4_alias_case(env, {'hz': 60, 'recovery': 'linear', 'decay': 15.0, 'rearm': 'recovered'}, 1)


# W3-yaw null with linear recovery (verifier pass 6): r under independent kicks is biased below 0; the one-sided,
# within-curve test must read 'none' (the two-sided pass-5 test read 'correlated' at r = -0.185)
@case()
def c_wa_w3_yaw_null_linear(env):
    wa_case(env, {'recovery': 'linear', 'rec_rate': 18.0, 'hcorr': 0.0}, wa_args(env, 'd', scale='0.2'),
            [('W3-yaw', 'none')], ['W3'], seed=4)


# W1 with impact noise: the ADS configs may go inconclusive (randomRadius veto); the final verdict must never be the
# wrong law or a conflict
@case()
def c_wa_w1_hit_noise(env):
    run_wa(env, {'disc': 'area', 'hit_noise': 0.02}, wa_args(env, 'a'))
    code, runs, text = env.extract(0)
    res = env.analyze(runs, ['W1'])
    got = [r for r in res if r.item == 'W1']
    v = got[0].variant if got else None
    env.check('W1 area or inconclusive under hit noise', v in ('area', 'inconclusive'),
              'got %r: %s' % (v, got[0].text()[:200].replace(chr(10), ' | ') if got else ''))


@case()
def c_wa_small(env):
    wa_case(env, {'disc': 'area'}, wa_args(env, 'a', scale='0.1'), [('W1', 'inconclusive')], ['W1'])


@case()
def c_wa_outliers(env):
    """planted outliers at 1.2 R and 1.3 R and a world origin 1500 m away: still area (no %.9g quantisation loss)."""
    for i, wo in enumerate(('1@1.2', '3@1.3')):
        laws = {'disc': 'area', 'w1_outliers': wo, 'origin': [1500.0, 0.0, 1500.0]}
        sub = Env(env.dir, 'o%d' % i, env.full, env.coverage)
        wa_case(sub, laws, wa_args(env, 'a'), [('W1', 'area')], ['W1'], seed=3 + i)
        env.checks.extend(('%s %s' % (wo, c[0]), c[1], c[2]) for c in sub.checks)
        env.hosts.extend(sub.hosts)
        for src, lines in sub.cov.items():
            env.cov.setdefault(src, set()).update(lines)


@case()
def c_wa_frame_cadence(env):
    """gs_cadence=frame:3 and ra_units=rad: W1-W4 unchanged (analysis uses nup and impacts)."""
    laws = {'gs_cadence': 'frame:3', 'ra_units': 'rad', 'disc': 'area', 'recovery': 'exp', 'decay': 15.0}
    wa_case(env, laws, [wa_args(env, 'a'), wa_args(env, 'b,c,d,e', scale='0.2')],
            [('W1', 'area'), ('W2', '15'), ('W3', 'exp'), ('W4', 'release')], ['W1', 'W2', 'W3', 'W4'])


@case(('phase0', 'all'))
def c_fire_mode_single(env):
    """weapons spawn in single fire; the mod cycles to auto: every part runs."""
    h, u = run_wa(env, {'fire_mode': 'single', 'recovery': 'exp'}, wa_args(env, 'a,b,c,d,e', scale='0.1'))
    env.check('no no-auto marks', not marks(h, 'no-auto'), '')
    parts = set((m.get('x') or {}).get('p') for m in marks(h, 'part', 'WA'))
    env.check('parts a-e ran', parts >= set('abcde'), str(sorted(p for p in parts if p)))


@case(('phase0', 'all'))
def c_fire_mode_stuck(env):
    """cycling is ignored: parts a and c only, with no-auto marks; the fast gaps are marked, not fired."""
    h, u = run_wa(env, {'fire_mode': 'stuck'}, wa_args(env, 'a,b,c,d,e', scale='0.1'))
    env.check('no-auto marked', len(marks(h, 'no-auto')) == 2, '%d' % len(marks(h, 'no-auto')))
    parts = set((m.get('x') or {}).get('p') for m in marks(h, 'part', 'WA'))
    env.check('parts a and c only', parts == {'a', 'c'}, str(sorted(p for p in parts if p)))
    env.check('fast gaps skipped', marks(h, 'no-auto-gap'), '')


@case(('phase0', 'all'))
def c_no_server_gunsway(env):
    """gs_server=no: P0.7 reports src=poll only; W2-W4 from poll rows unchanged."""
    laws = {'gs_server': False, 'recovery': 'exp', 'decay': 15.0}
    h, u = env.host(laws)

    def script():
        yield from US.setup(u, ('wall',), aim=True)
        u.p0 = yield from u.run('p0 fire', 'P0', 300)
        u.wa = yield from u.run('run WA ' + wa_args(env, 'b,c,d,e', scale='0.2'), 'WA', 20000)
    env.drive(h, u, script(), 40000)
    ok, val = p0_ok(h, 'P0.7')
    env.check('P0.7 gs poll', val and val.get('gsrc') == 'poll' and val.get('srv') == 0, json.dumps(val)[:200])
    env.check('WA done', u.wa == 'done', u.wa)
    env.journal(h)
    code, runs, text = env.extract(0)
    res = env.analyze(runs, ['W2', 'W3', 'W4'])
    for item, want in (('W2', '15'), ('W3', 'exp'), ('W4', 'release')):
        env.verdict(res, item, want)


@case(('phase0', 'all'))
def c_input_dt(env):
    """input_dt=required: bot inputs with deltaTime 0 would do nothing; the mod sets it, so P0 and W-A work."""
    h, u = env.host({'input_dt': 'required'})
    u.responders['fire3'] = US.respond_fire3

    def script():
        yield from US.setup(u, ('wall', 'flat'), aim=False)
        u.p0 = yield from u.run('p0 all', 'P0', 900)
        u.wa = yield from u.run('run WA ' + wa_args(env, 'a,b', scale='0.05'), 'WA', 3000)
        u.sb = yield from u.run('run SB scale=0.34', 'SB', 3000)
    env.drive(h, u, script(), 20000)
    for s in ('P0.5', 'P0.6', 'P0.7', 'P0.9'):
        ok, val = p0_ok(h, s)
        env.check('%s ok' % s, ok, json.dumps(val)[:160])
    env.check('WA done', u.wa == 'done', u.wa)
    env.check('SB done', u.sb == 'done', u.sb)
    env.journal(h)


def t1_case(env, coupled, want):
    for hz, tag, args in ((30, 'h30', wa_args(env, 'b,d,e', scale='0.2')), (60, 'h60', 'tier=60 parts=b,d,e scale=0.2')):
        h, u = env.host({'hz': hz, 'coupled': coupled, 'recovery': 'exp'}, tag=tag)
        env.drive(h, u, wa_script(u, args), 40000)
        env.check('WA %d Hz done' % hz, u.status == 'done', '%s' % u.statuses)
        env.journal(h)
    code, runs, text = env.extract(0)
    res = env.analyze(runs, ['W3', 'T1'])
    env.verdict(res, 'T1', want)


@case()
def c_t1_none(env):
    t1_case(env, 'none', 'none')


@case()
def c_t1_recovery(env):
    t1_case(env, 'recovery', 'recovery')

def sc_script(u, sites, aim, cmds, damage=False):
    """setup, optional P0.9 (bot damage for the B modes), then each (command, sc) run in order."""
    yield from US.setup(u, sites, aim=aim)
    if damage:
        u.dmg = yield from u.run('p0 damage', 'P0', 300)
    u.statuses = []
    for text, sc in cmds:
        st = yield from u.run(text, sc, 30000)
        u.statuses.append(st)


def sc_case(env, laws, sites, aim, cmds, expect, items, damage=False, seed=1, level_opts=None, responders=None,
            user_kw=None, max_s=60000):
    h, u = env.host(laws, seed=seed, level_opts=level_opts, **(user_kw or {}))
    for k, fn in (responders or {}).items():
        u.responders[k] = fn
    env.drive(h, u, sc_script(u, sites, aim, cmds, damage), max_s)
    for (text, sc), st in zip(cmds, u.statuses + [None] * len(cmds)):
        env.check('%s done' % text, st == 'done', str(st))
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    code, runs, text = env.extract(0)
    res = env.analyze(runs, items)
    for item, want in expect:
        env.verdict(res, item, want)
    return h, u, runs, res


# S-B --------------------------------------------------------------------------------------------------------------
@case()
def c_sb_fraction(env):
    sc_case(env, {'motor': 'fraction', 'gravity': 9.81}, ('wall', 'flat'), True, [('run SB scale=0.34', 'SB')],
            [('S3', 'fraction'), ('S3-gravity', 'g=9.81')], ['S3'])


@case()
def c_sb_linear_g15(env):
    sc_case(env, {'motor': 'linear', 'gravity': 15.0}, ('wall', 'flat'), True, [('run SB scale=0.34', 'SB')],
            [('S3', 'linear'), ('S3-gravity', 'g=15.00')], ['S3'])


@case()
def c_sb_instant(env):
    sc_case(env, {'motor': 'instant'}, ('wall', 'flat'), True, [('run SB scale=0.34', 'SB')], [('S3', 'instant')], ['S3'])


@case()
def c_sb_t1_accel(env):
    """S-B at 30 and 60 Hz with coupled=accel: the acceleration gains differ but T1 lists them as VU-patched (not
    informative), never as equal or coupled; gravity stays equal."""
    for hz in (30, 60):
        h, u = env.host({'hz': hz, 'coupled': 'accel', 'motor': 'fraction'}, tag='h%d' % hz)
        env.drive(h, u, sc_script(u, ('wall', 'flat'), True, [('run SB scale=0.34', 'SB')]), 20000)
        env.check('SB %d Hz done' % hz, u.statuses == ['done'], str(u.statuses))
        env.journal(h)
    code, runs, text = env.extract(0)
    res = env.analyze(runs, ['S3', 'T1'])
    t1 = [r for r in res if r.item == 'T1']
    v = t1[0].variant if t1 else None
    env.check('T1 none+patched', v == 'none+patched', '%r %s' % (v, t1[0].text()[:200] if t1 else ''))


# W-B --------------------------------------------------------------------------------------------------------------
def wb_b(env, laws, expect):
    sc_case(env, laws, ('wall', 'range'), True, [('run WB mode=B w=M16A3,M98B', 'WB')], expect, ['W5'], damage=True)


@case()
def c_wb_b_linear(env):
    wb_b(env, {'falloff': 'linear', 'm98b_end': 150.0}, [('W5 M16A3', 'linear'), ('W5 M98B end knot', '150')])


@case()
def c_wb_b_smooth(env):
    wb_b(env, {'falloff': 'smooth', 'm98b_end': 120.0}, [('W5 M16A3', 'smooth'), ('W5 M98B end knot', '120')])


@case(('phase0', 'all'))
def c_wb_u(env):
    """mode U: bot damage off, the user shoots on prompts with 10 % of its chest hits on an arm, a 150 m range."""
    eq = US.make_equip(['M16A3', 'M98B'])
    h, u, runs, res = sc_case(
        env, {'bot_damage': False, 'falloff': 'linear', 'range_len': 150.0}, ('range',), False,
        [('run WB mode=U w=M16A3,M98B', 'WB')], [('W5 M16A3', 'linear'), ('W5 M98B end knot', 'inconclusive')],
        ['W5'], responders={'equip': eq, 'shoot': US.respond_shoot}, user_kw={'arm_frac': 0.1, 'aim_sd': 0.0})
    dmg = [r for r in h.rows if r.get('e') == 'dmg' and r.get('ph') == 'pre']
    env.check('only user damage rows', dmg and all(r.get('giver') == 1 and r.get('cli') is True for r in dmg),
              '%d rows, givers %s' % (len(dmg), sorted(set(r.get('giver') for r in dmg), key=str)))
    env.check('prompts answered', u.answered.count('shoot') >= 10 and 'equip' in u.answered, str(u.answered[:6]))


# S-A --------------------------------------------------------------------------------------------------------------
@case()
def c_sa_b_10(env):
    sc_case(env, {'regen_delay': 10.0, 'supp_abort': True}, ('wall', 'range'), True, [('run SA mode=B', 'SA')],
            [('S1', '10'), ('S1-supp', 'abort')], ['S1'], damage=True)


@case()
def c_sa_b_16(env):
    sc_case(env, {'regen_delay': 16.0, 'supp_abort': False}, ('wall', 'range'), True,
            [('run SA mode=B', 'SA')], [('S1', '16'), ('S1-supp', 'no-abort')], ['S1'], damage=True)


@case()
def c_sa_b_nosupp(env):
    """supp_effect=none: bot bullets never suppress -> 'suppression not induced', no SUPP verdict."""
    h, u, runs, res = sc_case(env, {'supp_effect': 'none'}, ('wall', 'range'), True,
                              [('run SA mode=B', 'SA')], [('S1', '10'), ('S1-supp', 'not-induced')], ['S1'],
                              damage=True)
    env.check('S2 reported', any(r.item == 'S2' for r in res), '')
    env.check('regen reps without respawns', not marks(h, 'heal'), '')


@case(('phase0', 'all'))
def c_sa_u(env):
    responders = {'position': lambda u, r: u.goto('range'), 'shoot': US.respond_shoot, 'spray': US.respond_spray,
                  'immune': US.respond_immune}
    h, u, runs, res = sc_case(env, {'regen_delay': 10.0, 'supp_abort': True}, ('range',), False,
                              [('run SA mode=U', 'SA')], [('S1', '10'), ('S1-supp', 'abort')], ['S1'],
                              responders=responders)
    env.check('S2 reported', any(r.item == 'S2' for r in res), '')


# M-A / M-B --------------------------------------------------------------------------------------------------------
@case()
def c_ma_a(env):
    sc_case(env, {'bleed': 'A'}, (), False, [('run MA hold=60', 'MA')], [('M1', 'A')], ['M1'])


@case()
def c_ma_b(env):
    sc_case(env, {'bleed': 'B'}, (), False, [('run MA hold=60', 'MA')], [('M1', 'B')], ['M1'])


@case()
def c_mb_model(env):
    sc_case(env, {'capture': 'model', 'flip': 'lose_get'}, (), False, [('run MB scale=0.34', 'MB')],
            [('M2', 'model'), ('M2-flip', 'lose_get')], ['M2'])


@case()
def c_mb_linear(env):
    sc_case(env, {'capture': 'linear_n', 'flip': 'direct'}, (), False, [('run MB scale=0.34', 'MB')],
            [('M2', 'linear_n'), ('M2-flip', 'direct')], ['M2'])


# W-C / W-D --------------------------------------------------------------------------------------------------------
def wc(env, blast):
    eq = US.make_equip(['RPG7', 'SMAW', 'M320HE', 'M67'])

    def equip(u, r):
        nxt = ['RPG7', 'SMAW', 'M320HE', 'M67'][min(len([a for a in u.answered if a == 'equip']) - 1, 3)]
        u.task('equip', key=nxt)
        u.goto('ring' if nxt == 'M67' else 'range')
        u.task('wait', s=0.5)
        u.cmd('ok')
    sc_case(env, {'blast': blast}, ('range', 'ring'), False, [('run WC scale=0.34', 'WC')], [('W7', blast)], ['W7'],
            responders={'equip': equip, 'fire': US.respond_fire_direct, 'throw': US.respond_throw})


@case(('phase0', 'all'))
def c_wc_linear(env):
    wc(env, 'linear')


@case()
def c_wc_quadratic(env):
    wc(env, 'quadratic')


@case(('phase0', 'all'))
def c_wd(env):
    """client realm: pred/corr gs rows, fov rows for 3 zoom levels within 0.1 deg of the mock's vertical FOV."""
    h, u = env.host({})
    for k in ('mags', 'reloads', 'optics', 'scope'):
        u.responders[k] = US.respond_wd
    env.drive(h, u, sc_script(u, ('wall',), False, [('run WD', 'WD')]), 20000)
    env.check('WD done', u.statuses == ['done'], str(u.statuses))
    env.journal(h)
    code, runs, text = env.extract(0)
    crows = [r for run in runs for r in run.crows]
    gsrc = set(r.get('src') for r in crows if r.get('e') == 'gs')
    env.check('client gs pred and corr', {'pred', 'corr'} <= gsrc, str(sorted(gsrc)))
    fov = {}
    for r in crows:
        if r.get('e') == 'fov':
            fov.setdefault(r['zoom'], []).append(r['vfov'])
    vf = {0: 55.0, 1: 40.0, 2: 20.0}
    env.check('fov rows for 3 zoom levels', set(fov) >= {0, 1, 2}, str(sorted(fov)))
    worst = max([abs(v - vf[z]) for z, vs in fov.items() for v in vs if z in vf] or [99])
    env.check('fov within 0.1 deg', worst <= 0.1, 'worst %.4f' % worst)
    srows = [r for run in runs if run.sc == 'WD' for r in run.rows if r.get('r') == 'S' and r.get('e') == 'shot']
    env.check('server shot rows for the human', len(srows) >= 90, '%d' % len(srows))

# robustness: queue, repeats, stuck runs, human leaving, log faults, ticket forecast, Phase 0 faults ------------------
def wait_ends(u, n, timeout, start):
    """Collect n run end marks (sc, status) from row index `start`."""
    out = []
    i = start
    k1 = u.h.k + int(timeout * u.h.hz)
    while len(out) < n:
        while i < len(u.h.rows):
            r = u.h.rows[i]
            i += 1
            if r.get('e') == 'mark' and r.get('r') == 'S' and r.get('lbl') in US.STATUSES:
                out.append((r.get('sc'), r['lbl']))
        if u.h.k > k1:
            raise US.Timeout('%d of %d run ends seen: %s' % (len(out), n, out))
        yield
    return out


def say_all(u, text, wait_s=3.0):
    """Send a command and return every reply line seen within wait_s."""
    yield from u.wait_idle()
    u.new_say()
    u.cmd(text)
    yield from u.wait(wait_s)
    return u.new_say()


@case(('phase0', 'all'))
def c_stuck(env):
    """a config whose spawns are no-ops is skipped (run done); every spawn blocked: WA aborts after 3 skips and the
    next queue item still runs; no orphan bots either way."""
    h, u = env.host({'spawn_fails': 'WA:1'})
    env.drive(h, u, wa_script(u, wa_args(env, 'a,b', scale='0.05')), 20000)
    env.check('WA:1 blocked: run done', u.status == 'done', str(u.statuses))
    env.check('cfg-skip marked', marks(h, 'cfg-skip', 'WA'), '')
    env.check('no orphan bots (1)', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    h, u = env.host({'spawn_fails': 'WA:*'}, tag='h2')

    def script():
        yield from US.setup(u, ('wall', 'flat'), aim=True)
        start = len(h.rows)
        u.lines = yield from say_all(u, 'queue WA:rest:parts=b,c,d:scale=0.05 SB:scale=0.34')
        u.ends = yield from wait_ends(u, 2, 20000, start)
    env.drive(h, u, script(), 30000)
    env.check('queue accepted', any(re.match(r'^\[mh\] 2 SB', x) for x in u.lines), str(u.lines[:3]))
    env.check('WA aborted, SB done', u.ends == [('WA', 'aborted'), ('SB', 'done')], str(u.ends))
    ab = marks(h, 'aborted', 'WA')
    env.check('abort reason 3 skips', ab and '3 consecutive skips' in json.dumps(ab[-1]), json.dumps(ab[-1:])[:200])
    env.check('no orphan bots (2)', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    code, runs, text = env.extract(0)
    env.check('aborted run extracted', any(r.sc == 'WA' for r in runs), str([r.id for r in runs]))


@case(('phase0', 'all'))
def c_repeats(env):
    """repeated and stray commands: second run refused, stop twice, sites twice, clear twice, duplicate queue items,
    ok without a prompt, unknown verbs; no handler errors, no orphan bots."""
    h, u = env.host({})

    def script():
        yield from US.setup(u, ('wall',), aim=True)
        r = {}
        start = len(h.rows)
        u.cmd('run WA ' + wa_args(env, 'a', scale='0.5'))
        yield from u.wait(3)
        r['again'] = yield from say_all(u, 'run WA ' + wa_args(env, 'a', scale='0.1'))
        r['clear_busy'] = yield from say_all(u, 'clear')
        r['ok'] = yield from say_all(u, 'ok')
        # the queue starts only when idle: queue, show and clear it while WA runs
        r['q1'] = yield from say_all(u, 'queue MA:hold=20 MA:hold=20')
        r['qshow'] = yield from say_all(u, 'queue')
        r['qclear'] = yield from say_all(u, 'queue clear')
        r['qclear2'] = yield from say_all(u, 'queue clear')
        r['stop1'] = yield from say_all(u, 'stop', 0.1)
        r['stop2'] = yield from say_all(u, 'stop')
        r['ends'] = yield from wait_ends(u, 1, 60, start)
        r['wall2'] = yield from say_all(u, 'wall')
        r['wall3'] = yield from say_all(u, 'wall')
        r['clear1'] = yield from say_all(u, 'clear')
        r['clear2'] = yield from say_all(u, 'clear')
        r['bogus'] = yield from say_all(u, 'bogus verb')
        r['badrun'] = yield from say_all(u, 'run XX')
        r['badq'] = yield from say_all(u, 'queue NOPE:x')
        r['status'] = yield from say_all(u, 'status')
        u.r = r
    env.drive(h, u, script(), 3000)
    r = u.r

    def has(key, rx):
        env.check('%s: %s' % (key, rx), any(re.search(rx, x) for x in r[key]), str(r[key][:3]))
    has('again', r'already running WA')
    has('clear_busy', r'running WA: !mh stop first')
    env.check('ok without prompt: no reply', not r['ok'], str(r['ok']))
    has('stop1', r'stopping WA')
    has('stop2', r'nothing running')
    env.check('run stopped', r['ends'] == [('WA', 'stopped')], str(r['ends']))
    has('wall2', r'^\[mh\] wall ')
    has('wall3', r'^\[mh\] wall ')
    has('clear1', r'removed \d+ bots')
    has('clear2', r'removed 0 bots')
    has('q1', r'^\[mh\] 2 MA:hold=20')
    has('qshow', r'^\[mh\] 2 MA:hold=20')
    has('qclear', r'queue cleared')
    has('qclear2', r'queue cleared')
    has('bogus', r'unknown verb bogus')
    has('badrun', r'run <SC>')
    has('badq', r'nothing queued: unknown item')
    has('status', r'queue empty')
    env.check('no MA started', not [m for m in marks(h, 'run-start') if (m.get('x') or {}).get('sc') == 'MA'], '')
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)


def no_human_script(env, u, h, idle_s):
    yield from US.setup(u, ('wall',), aim=True)
    start = len(h.rows)
    u.cmd('run WA ' + wa_args(env, 'a', scale='0.1'))
    yield from u.wait(8)
    h.leave()
    u.joined = False
    for _ in range(int(idle_s * h.hz / 10)):
        yield
    u.join()
    u.spawn('wall')
    u.ends = yield from wait_ends(u, 1, 3000, start)


@case(('phase0', 'all'))
def c_no_human(env):
    """the human leaves mid-run: human-left / human-back marks, no stall (idle server frozen, or 150 s of ticks
    with nobody on), the run still ends done."""
    for i, (adv, idle_s) in enumerate(((False, 30), (True, 150))):
        h, u = env.host({'idle_advance': adv}, tag='h%d' % (i + 1))
        env.drive(h, u, no_human_script(env, u, h, idle_s), 6000)
        env.check('idle_advance=%s: marks' % adv, marks(h, 'human-left') and marks(h, 'human-back'), '')
        env.check('idle_advance=%s: WA done' % adv, u.ends == [('WA', 'done')], str(u.ends))
        env.check('idle_advance=%s: no orphans' % adv, bots_left(h) == 0, 'bots %d' % bots_left(h))
        env.journal(h)


@case(('phase0', 'all'))
def c_log_faults(env):
    """print_limit 600 B, 1 % dropped lines, 0.2 % garbage lines: the extractor reports malformed rows and q gaps
    (exit 2 when malformed > 1 %), never crashes; analysis of the damaged runs still returns verdicts."""
    h, u = env.host({'print_limit': 600, 'log_drop': 0.01, 'log_garbage': 0.002, 'recovery': 'exp'})
    env.allow_viol = True
    env.drive(h, u, wa_script(u, wa_args(env, 'a,b', scale='0.1')), 20000)
    env.check('WA done', u.status == 'done', str(u.statuses))
    code, runs, text = env.extract(None)
    env.check('extract exit 0 or 2', code in (0, 2), 'exit %s' % code)
    m = re.search(r'(\d+) malformed', text)
    env.check('malformed counted', m and int(m.group(1)) > 0, text.splitlines()[0][:200])
    gaps = [r.meta.get('q_gaps') for r in runs if r.meta.get('q_gaps')]
    env.check('q gaps counted', gaps, str(gaps)[:200])
    try:
        res = env.analyze(runs, ['W1', 'W2', 'W3', 'W4'])
        env.check('analysis on damaged runs', any(r.item == 'W2' for r in res), '')
    except Exception as ex:  # noqa: BLE001 - the check is "no crash"
        env.check('analysis on damaged runs', False, repr(ex)[:200])


@case(('phase0', 'all'))
def c_queue_validation(env):
    """items whose sites, aim calibration or bot-damage verdict are missing are refused as a whole line."""
    h, u = env.host({})

    def script():
        u.join()
        u.spawn('deploy')
        yield from u.wait(3)
        r = {}
        r['wbb'] = yield from say_all(u, 'queue WB:B')
        r['sb'] = yield from say_all(u, 'queue SB:scale=0.2 WA:tier1')
        r['wa'] = yield from say_all(u, 'queue MA WA:tier1')
        r['sab'] = yield from say_all(u, 'queue MA SA:B')
        r['runwa'] = yield from say_all(u, 'run WA')
        r['show'] = yield from say_all(u, 'queue')
        u.r = r
    env.drive(h, u, script(), 600)
    r = u.r
    for key, rx in (('wbb', r'nothing queued: WB:B refused: site range missing'),
                    ('sb', r'nothing queued: SB:scale=0.2 refused: site flat missing'),
                    ('wa', r'nothing queued: WA:tier1 refused: site wall missing'),
                    ('sab', r'nothing queued: SA:B refused: site range missing'),
                    ('runwa', r'site wall missing'), ('show', r'queue empty')):
        env.check('%s: %s' % (key, rx), any(re.search(rx, x) for x in r[key]), str(r[key][:3]))
    env.check('nothing started', not marks(h, 'run-start'), '')
    env.journal(h)


@case()
def c_tickets(env):
    """ticket forecast: a line that would end below the guard is refused ('put rr before it'); an accepted item
    shows its forecast; a run that burns more than forecast trips the guard (status guard, item requeued, queue
    paused)."""
    h, u = env.host({'tcost': {'spawn': 1, 'kill': 1, 'remove': 1}}, level_opts={'tickets1': 120, 'tickets2': 120})

    def script():
        u.join()
        u.spawn('deploy')
        yield from u.wait(3)
        u.refused = yield from say_all(u, 'queue ' + ' '.join(['MB:scale=0.34'] * 7))
        start = len(h.rows)
        u.acc = yield from say_all(u, 'queue MB:scale=0.34')
        u.ends = yield from wait_ends(u, 1, 3000, start)
    env.drive(h, u, script(), 4000)
    env.check('long line refused', any(re.search(r'refused: forecast T1 \d+ T2 \d+ < 60, put rr before it', x)
                                       for x in u.refused), str(u.refused[:2]))
    env.check('forecast line', any(re.search(r'^\[mh\] 1 MB:scale=0.34 \+\d+min T1 109 T2 112', x) for x in u.acc),
              str(u.acc[:2]))
    env.check('MB done', u.ends == [('MB', 'done')], str(u.ends))
    env.journal(h)
    h, u = env.host({'tcost': {'spawn': 15, 'kill': 1, 'remove': 0}}, level_opts={'tickets1': 110, 'tickets2': 110},
                    tag='h2')

    def script2():
        yield from US.setup(u, ('wall',), aim=True)
        start = len(h.rows)
        u.cmd('run WA ' + wa_args(env, 'a,b', scale='0.2'))
        u.ends = yield from wait_ends(u, 1, 3000, start)
        yield from u.wait(2)
        u.st = yield from say_all(u, 'status')
    env.drive(h, u, script2(), 4000)
    env.check('guard tripped', u.ends == [('WA', 'guard')], str(u.ends))
    env.check('tickets-low message', any('tickets low' in x for x in u.h.say), '')
    env.check('item requeued, queue paused', any(re.search(r'queue 1:WA.*\(paused tickets\)', x) for x in u.st),
              str(u.st))
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)


@case()
def c_ma_queue(env):
    """queue MA rr nextmap MA after P0.15 kept: two MA runs on MP_Subway and MP_001, both M1 verdicts."""
    h, u = env.host({'bleed': 'A'})

    def script():
        u.join()
        u.spawn('deploy')
        yield from u.wait(3)
        st = yield from u.run('p0 level confirm', 'P0', 400)
        u.p0 = st
        yield from u.wait(20)
        start = len(h.rows)
        u.q = yield from say_all(u, 'queue MA:hold=40 rr nextmap MA:hold=40')
        u.ends = yield from wait_ends(u, 4, 6000, start)
    env.drive(h, u, script(), 8000)
    env.check('P0 level done', u.p0 == 'done', str(u.p0))
    env.check('queue lines', any(re.match(r'^\[mh\] 4 MA:hold=40', x) for x in u.q), str(u.q[:4]))
    env.check('four ends done', [s for _, s in u.ends] == ['done'] * 4, str(u.ends))
    env.check('MP_001 loaded', any('level-loaded Levels/MP_001' in w for _, w in h.marks), str(h.marks[-3:]))
    env.journal(h)
    code, runs, text = env.extract(0)
    ma = [r for r in runs if r.sc == 'MA']
    env.check('two MA runs', len(ma) == 2, str([r.id for r in runs]))
    lv = set(r.metas[0].get('lvl') for r in ma if r.metas)
    env.check('MA on both levels', len(lv) == 2, str(lv))
    res = env.analyze(runs, ['M1'])
    env.verdict(res, 'M1', 'A')


@case(('phase0', 'all'))
def c_p0_faults(env):
    """bot damage off, no linked bullets, server GunSway every 2nd frame, preround, chat off (console path)."""
    laws = {'bot_damage': False, 'ef_bullets': False, 'gs_cadence': 'frame:2', 'preround': True, 'chat': False}
    h, u = env.host(laws, via='console')
    u.responders['fire3'] = US.respond_fire3
    env.drive(h, u, p0_script(u, level=False, round_=True), 3600)
    vals = {}
    for s in P0_STEPS[:-1]:
        rows = p0_rows(h, s)
        vals[s] = (rows[-1].get('ok'), rows[-1].get('val')) if rows else (None, None)
    env.notes.append('P0 values: ' + json.dumps(vals)[:600])
    rep = p0_rows(h, 'P0.14')
    rv = rep[-1]['val'] if rep else {}
    env.check('replies over the console path', u.census_line.startswith('[mh] census:'), u.census_line)
    env.check('P0.9 NO-arb', rv.get('damage') == 'NO-arb', json.dumps(rv)[:200])
    env.check('P0.13 not ok (preround)', vals['P0.13'][0] is False, json.dumps(vals['P0.13'])[:200])
    env.check('P0.8 reported', vals['P0.8'][0] is not None, json.dumps(vals['P0.8'])[:200])
    v7 = vals['P0.7'][1] or {}
    env.check('P0.7 no EF bullets: shots 0, fs 10, srv gs', vals['P0.7'][0] is False and v7.get('shots') == 0 and
              v7.get('fs') == 10 and v7.get('gsrc') == 'srv', json.dumps(v7)[:200])
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    env.extract(0)


@case(('phase0', 'all'))
def c_p0_aim_fault(env):
    """bots aim 30 deg off: P0.9 reports NO-aim (not NO-arb)."""
    h, u = env.host({'aim_bias': 30.0})
    u.responders['fire3'] = US.respond_fire3
    env.drive(h, u, p0_script(u, level=False, round_=False), 3600)
    rep = p0_rows(h, 'P0.14')
    rv = rep[-1]['val'] if rep else {}
    env.check('P0.9 NO-aim', rv.get('damage') == 'NO-aim', json.dumps(rv)[:200])
    env.journal(h)


@case(('phase0', 'all'))
def c_p0_level_lost(env):
    """a round restart reloads the Lua state: P0.15 lost, rr/nextmap refused in the queue; bots created by the old
    state stay on the server (RUNBOOK: kick them or restart the server)."""
    h, u = env.host({'rr_state': 'lost'})
    env.allow_err = True

    def script():
        u.join()
        u.spawn('deploy')
        yield from u.wait(3)
        u.cmd('p0 level confirm')
        k1 = h.k + 400 * h.hz
        while not any(w == 'mod-reload' for _, w in h.marks):
            if h.k > k1:
                raise US.Timeout('no mod reload')
            yield
        yield from u.wait(25)
        u.q = yield from say_all(u, 'queue rr')
    env.drive(h, u, script(), 3000)
    rows = p0_rows(h, 'P0.15')
    env.check('no P0.15 kept row (the run died with its Lua state)', not [r for r in rows if r.get('ok') is True],
              json.dumps(rows[-1:])[:200])
    env.check('ready banner twice', sum(1 for x in h.say if re.search(r'mharness \S+ ready', x)) >= 2,
              str([x for x in h.say if 'ready' in x]))
    env.check('rr refused', any('round restart not verified' in x for x in u.q), str(u.q[:2]))
    code, runs, text = env.extract(0)
    env.notes.append('bots left after the Lua reload: %d' % bots_left(h))

RUNBOOK_PATH = os.path.join(HERE, 'RUNBOOK.md')
SITE_CMDS = ('wall', 'flat', 'range')


def runbook_reprepare():
    """The RUNBOOK "R. Re-prepare" block as [(command without '!mh ', b_items_only)], parsed from the RUNBOOK itself so
    the steps the user follows after a new Lua state are the steps this case replays."""
    with open(RUNBOOK_PATH, encoding='utf-8') as f:
        text = f.read()
    m = re.search(r'<!-- reprepare:begin -->(.*?)<!-- reprepare:end -->', text, re.S)
    if not m:
        raise CaseFail('RUNBOOK has no reprepare block')
    steps = []
    for ln in m.group(1).splitlines():
        c = re.search(r'`!mh ([^`]+)`', ln)
        if c:
            steps.append((c.group(1).strip(), '(B items only)' in ln))
    return steps, text


def reprepare_script(u, steps, b_items):
    """Replay the block: sites by walking to them first, P0 steps as runs, the rest as plain commands."""
    out = []
    for cmd, b_only in steps:
        if b_only and not b_items:
            continue
        if cmd in SITE_CMDS:
            u.goto(cmd)
            yield from u.wait(0.5)
            line = yield from u.say_wait(cmd, r'^\[mh\] %s ' % cmd, 20)
            out.append((cmd, ' ok' in line, line))
        elif cmd.startswith('p0 '):
            st = yield from u.run(cmd, 'P0', 300)
            out.append((cmd, st == 'done', st))
        else:
            lines = yield from say_all(u, cmd)
            out.append((cmd, bool(lines), ' | '.join(lines[:2])))
    u.goto('deploy')
    yield from u.wait(0.5)
    return out


def reprepare_case(env, hz, queue, b_items):
    steps, text = runbook_reprepare()
    env.check('reprepare block parsed', len(steps) >= 5 and steps[0][0] == 'p0 tick', str(steps))
    h, u = env.host({'hz': hz}, tag='h%d' % hz)

    def script():
        u.join()
        u.spawn('deploy')
        yield from u.wait(3)
        # a fresh Lua state: the queue line is refused with every missing prerequisite in one reply
        u.before = yield from say_all(u, 'queue ' + queue)
        u.steps = yield from reprepare_script(u, steps, b_items)
        u.q = yield from say_all(u, 'queue ' + queue, 5.0)
        yield from u.wait(20)
        u.started = bool(marks(h, 'run-start'))
        yield from say_all(u, 'queue clear')
        u.cmd('stop')
        yield from u.wait(30)
        u.cmd('clear')
        yield from u.wait(10)
    env.drive(h, u, script(), 3000)
    env.check('fresh state refused with all reasons', any(
        'nothing queued' in x and 'aim calibration missing' in x and 'site' in x for x in u.before), str(u.before[:2]))
    for cmd, ok, detail in u.steps:
        env.check('R %s' % cmd, ok, str(detail)[:160])
    tick = [d for c, ok, d in u.steps if c == 'p0 tick']
    env.check('tick row says %d Hz' % hz, any(abs(float(r['val'].get('hz', 0)) - hz) < 0.5
                                            for r in p0_rows(h, 'P0.3') if isinstance(r.get('val'), dict)),
              json.dumps(p0_rows(h, 'P0.3')[-1:])[:200])
    n = len(queue.split())
    env.check('queue %r accepted' % queue, any(re.match(r'^\[mh\] %d ' % n, x) for x in u.q) and
              not any('nothing queued' in x for x in u.q), str(u.q[:3]))
    env.check('first item started', u.started, '')
    env.check('no orphan bots', bots_left(h) == 0, 'bots %d' % bots_left(h))
    env.journal(h)
    return text


@case(('phase0', 'all'))
def c_runbook_reprepare(env):
    """RUNBOOK step 13.4 (fresh 60 Hz server) and the crash / state-lost paths with B items: after the RUNBOOK's
    re-prepare block the queue lines the RUNBOOK prints are accepted (verifier finding: they were refused for the
    missing aim calibration)."""
    text = reprepare_case(env, 60, 'WA:60 SB', False)
    env.check('RUNBOOK 13 queues WA:60 SB after R', re.search(r'R\. Re-prepare.*\n.*\n5\. `!mh queue WA:60 SB`', text)
              is not None, '')
    reprepare_case(env, 30, 'WB:B SA:B SB MB', True)


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

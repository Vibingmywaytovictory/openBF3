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


@case()
def c_wa_t1_linear(env):
    wa_case(env, {'recovery': 'linear', 'decay': 8.0, 'hcorr': 0.6, 'rearm': 'release'}, wa_args(env, scale='0.2'),
            [('W2', '8'), ('W3', 'linear'), ('W3-yaw', 'correlated'), ('W4', 'release')], ['W2', 'W3', 'W4'])


@case()
def c_wa_w4_recovered(env):
    wa_case(env, {'recovery': 'exp', 'rearm': 'recovered'}, wa_args(env, 'a,c', scale='0.2'), [('W4', 'recovered')],
            ['W4'])


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


@case()
def c_fire_mode_single(env):
    """weapons spawn in single fire; the mod cycles to auto: every part runs."""
    h, u = run_wa(env, {'fire_mode': 'single', 'recovery': 'exp'}, wa_args(env, 'a,b,c,d,e', scale='0.1'))
    env.check('no no-auto marks', not marks(h, 'no-auto'), '')
    parts = set((m.get('x') or {}).get('p') for m in marks(h, 'part', 'WA'))
    env.check('parts a-e ran', parts >= set('abcde'), str(sorted(p for p in parts if p)))


@case()
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

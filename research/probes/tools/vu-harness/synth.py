"""Synthetic MH1 logs for both variants of every analysis rule (DESIGN.md section 14, tests/test_synth_rules.py).

    python -I synth.py --rule W1 --variant area --out <logdir> [--seed 1] [--n 200] [--hz 30]
    python -I synth.py --list

The generators are an independent re-implementation of the mechanics the rules decide between (not the Lua mock):
disc A/B, decay 15/8 deg/s and min rise, recovery linear/exp and correlated yaw, first-shot re-arm laws, falloff
linear/smoothstep and the M98B end knot, regeneration delay and suppression, motor laws and gravity, bleed rules,
capture laws and flips, and tick coupling. Each writes one log file in the VU server format (header lines, MH1 rows
with the full envelope) that extract.py reads like a real log.
"""

import argparse
import json
import math
import os
import sys

import numpy as np

VARIANTS = {
    'W1': ['area', 'radius', 'small'],
    'W2': ['15', '8', 'immediate', 'ramp'],
    'W3': ['linear', 'exp', 'correlated', 'none'],
    'W4': ['release', 'recovered', 'spreadmin', 'timer:0.3'],
    'W5': ['linear', 'smooth', '150', '120'],
    'S1': ['10', '16', 'abort', 'no-abort'],
    'S3': ['fraction', 'linear', 'g9.81', 'g15'],
    'M1': ['A', 'B'],
    'M2': ['model', 'linear_n', 'lose_get', 'direct'],
    'T1': ['none', 'decay', 'recovery'],
}

M16 = {'rpm': 800, 'inc': 0.1, 'hip': (2.5, 7.0), 'ads': (0.2, 3.0), 'up': 0.26, 'hmin': -0.4, 'hmax': 0.1, 'fsrm': 2.5}


class Log(object):
    """Writes MH1 lines with a consistent envelope (q per realm, k run-relative, t = k/hz)."""

    def __init__(self):
        self.lines = ['[21:00:00.000] [info] Build 20682', '[21:00:00.001] [info] VeniceEXT engine v1.15.0']
        self.q = 0
        self.ms0 = 2000000

    def start(self, sc, hz, hhmmss):
        self.run = '20261016-%s-%d-%s' % (sc, hz, hhmmss)
        self.sc, self.hz, self.k = sc, hz, 0
        self.row('meta', hv='0.2.0', lvl='Levels/MP_Subway/MP_Subway', mode='ConquestSmall0', rnd=0, rpm=0,
                 hz=float(hz), freq='regular' if hz == 30 else 'high60', vars={}, mods=['mharness'], build=20682)

    def row(self, e, **p):
        self.q += 1
        d = {'v': 1, 'run': self.run, 'sc': self.sc, 'r': 'S', 'k': self.k, 't': round(self.k / self.hz, 6),
             'ms': self.ms0 + int(self.k * 1000 / self.hz), 'q': self.q, 'e': e}
        d.update(p)
        self.lines.append('[21:%02d:%02d.000] [info] [VeniceEXT] [mharness] MH1 %s' % (
            (self.k // (60 * self.hz)) % 60, (self.k // self.hz) % 60, json.dumps(d, separators=(',', ':'))))

    def mark(self, lbl, **p):
        self.row('mark', lbl=lbl, **p)

    def tick(self, n=1):
        self.k += n

    def write(self, path):
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(self.lines) + '\n')


# ---------------------------------------------------------------------------------------------------------------------
# weapon model

class Gun(object):
    def __init__(self, rng, hz, laws, zoom=False, w=M16):
        self.rng, self.hz, self.L, self.w = rng, hz, laws, w
        self.dt = 1.0 / hz
        self.dmin, self.dmax = w['ads'] if zoom else w['hip']
        self.da, self.p, self.y = self.dmin, 0.0, 0.0
        self.tsls, self.shots, self.released, self.last_kick, self.h = 10.0, 0, True, 1.0, None
        self.interval = 60.0 / w['rpm']

    def step(self):
        dt = self.dt
        self.tsls += dt
        firing = self.tsls < self.interval + 1e-9
        rate = self.L.get('rec_rate', 8.0) * (0.1 if firing else 1.0)
        coupled = self.L.get('coupled')
        if self.L.get('recovery', 'linear') == 'exp':
            f = math.exp(-rate / 30.0) if coupled == 'recovery' else math.exp(-rate * dt)
            self.p *= f
            self.y *= f
            if abs(self.p) < 1e-6:
                self.p = 0.0
        else:
            m = rate / 30.0 if coupled == 'recovery' else rate * dt
            self.p = max(0.0, self.p - m)
            self.y = math.copysign(max(0.0, abs(self.y) - m), self.y)
        # the decrease starts once no shot was fired for two intervals (mock/synth assumption, DESIGN 16)
        over = self.tsls - 2 * self.interval
        dec = self.L.get('decay', 15.0)
        if over > 0 and self.da > self.dmin:
            amt = dec / 30.0 * min(1.0, over / dt) if coupled == 'decay' else dec * min(dt, over)
            self.da = max(self.dmin, self.da - amt)

    def armed(self):
        rule = self.L.get('rearm', 'release')
        if self.shots == 0:
            return True
        if rule == 'release':
            return self.released
        if rule == 'recovered':
            return abs(self.p) <= 0.05 * self.last_kick
        if rule == 'spreadmin':
            return self.da <= self.dmin + 1e-9
        return self.tsls >= float(rule.split(':')[1]) - 1e-9

    def shoot(self):
        w = self.w
        armed = self.armed()
        kick = w['up'] * (w['fsrm'] if armed else 1.0)
        self.p += kick
        self.last_kick = kick
        mu, sig = (w['hmin'] + w['hmax']) / 2, (w['hmax'] - w['hmin']) / math.sqrt(12)
        c = self.L.get('hcorr', 0.0)
        z = self.rng.normal() * sig
        self.h = mu + z if self.h is None else mu + c * (self.h - mu) + math.sqrt(1 - c * c) * z
        self.y += self.h
        u = self.rng.random()
        r = self.da * (math.sqrt(u) if self.L.get('disc', 'area') == 'area' else u)
        phi = self.rng.random() * 2 * math.pi
        self.da = min(self.dmax, self.da + self.L.get('inc', w['inc']))
        self.tsls = 0.0
        self.shots += 1
        self.released = False
        return r, phi

    def gs(self, sid, fs, zoom, r=0.0, phi=0.0):
        return dict(id=sid, wid=1, src='srv', da=self.da, dmin=self.dmin, rec=[self.p, self.y, 0, 0], disp=[0, 0],
                    ra=math.degrees(phi), rr=r, seed=self.shots, ifs=0, fs=fs, fir=self.tsls < self.interval,
                    tsls=self.tsls, mov=False, jmp=False, jt=0, pose=0, zoom=zoom, nup=1, dt=self.dt)


class Shooter(object):
    """Drives a Gun tick by tick with shot schedules and writes gs/shot/hit rows."""

    def __init__(self, log, gun, sid, zoom, dist, every='tick'):
        self.log, self.g, self.sid, self.zoom, self.D = log, gun, sid, zoom, dist
        self.every = every
        self.uid = 7000
        self.acc = 0.0

    def tick(self, shoot=False, emit=True):
        self.g.step()
        r = phi = 0.0
        if shoot:
            r, phi = self.g.shoot()
            self.shot_rows(r, phi)
        if emit:
            self.log.row('gs', **self.g.gs(self.sid, shoot, self.zoom, r, phi))
        self.log.tick()

    def shot_rows(self, r, phi):
        self.uid += 1
        rr = math.radians(r)
        d = np.array([math.cos(phi) * math.sin(rr), math.sin(phi) * math.sin(rr), math.cos(rr)])
        o = np.array([0.0, 1.6, 0.25])
        hit = o + d * ((self.D - o[2]) / d[2])
        self.log.row('shot', id=self.sid, wid=1, kind='bullet', uid=self.uid, pos=[float(x) for x in o],
                     dir=[float(x) for x in d], td='BulletEntityData')
        self.log.row('hit', uid=self.uid, pos=[float(x) for x in hit], n=[0.0, 0.0, -1.0], bone=-1, part=-1)

    def wait(self, s, emit=True):
        for _ in range(int(round(s * self.g.hz))):
            self.tick(emit=emit)

    def single(self, emit_quiet=True):
        # fire1: the trigger is released after the shot
        self.tick(shoot=True)
        self.g.released = True

    def auto(self, n):
        """Held trigger: n shots at the rpm (tick-quantised accumulator), then release."""
        self.tick(shoot=True)
        self.acc = 0.0
        shots = 1
        while shots < n:
            self.acc += 1.0 / self.g.hz
            if self.acc + 1e-9 >= self.g.interval:
                self.acc -= self.g.interval
                self.tick(shoot=True)
                shots += 1
            else:
                self.tick()
        self.g.released = True


def wa_config(log, rng, i, n, label, laws, parts, zoom, counts, hz):
    gun = Gun(rng, hz, laws, zoom)
    sh = Shooter(log, gun, 3, zoom, 25.0 if zoom else 10.0)
    log.mark('cfg-start', cfg=label, i=i, n=n, x={'w': 'M16A3', 'stance': 'stand', 'zoom': zoom,
                                                    'proto': laws.get('proto', 'full'), 'sway': False})
    sh.wait(1.0)
    if 'a' in parts:
        log.mark('part', cfg=label, x={'p': 'a'})
        for u in range(counts.get('a', 200)):
            log.mark('u', cfg=label, i=u + 1, x={'p': 'a'})
            sh.single()
            # by default only the ticks around each single shot are logged (the mod logs every tick)
            full = counts.get('a_full', False)
            sh.tick()
            sh.wait(1.5, emit=full)
            sh.tick()
    if 'b' in parts:
        log.mark('part', cfg=label, x={'p': 'b'})
        for u in range(counts.get('b', 40)):
            log.mark('u', cfg=label, i=u + 1, x={'p': 'b'})
            sh.auto(5)
            sh.wait(1.5)
    if 'c' in parts:
        log.mark('part', cfg=label, x={'p': 'c'})
        u = 0
        for g in (0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5):
            for _ in range(counts.get('c', 6)):
                u += 1
                log.mark('u', cfg=label, i=u, x={'p': 'c', 'g': g})
                if g <= 0.08:
                    sh.auto(2)
                else:
                    sh.single()
                    sh.wait(g)
                    sh.single()
                sh.wait(2.5)
    if 'd' in parts:
        log.mark('part', cfg=label, x={'p': 'd'})
        for u in range(counts.get('d', 10)):
            log.mark('u', cfg=label, i=u + 1, x={'p': 'd'})
            sh.auto(31)
            sh.wait(4.0)
    if 'e' in parts:
        log.mark('part', cfg=label, x={'p': 'e'})
        ne = counts.get('e', 10)
        for u in range(2 * ne):
            log.mark('u', cfg=label, i=u + 1, x={'p': 'e', 'nb': 10 if u < ne else 20})
            sh.auto(10 if u < ne else 20)
            sh.wait(4.0)
    log.mark('cfg-done', cfg=label, i=i, n=n)


def gen_wa(log, rng, laws, parts, counts, hz=30, hhmmss='213000', labels=None):
    log.start('WA', hz, hhmmss)
    cfgs = labels or [('M16A3-stand-ads', True), ('M16A3-stand-hip', False)]
    for i, (label, zoom) in enumerate(cfgs):
        wa_config(log, rng, i + 1, len(cfgs), label, laws, parts, zoom, counts, hz)


# ---------------------------------------------------------------------------------------------------------------------
# S-B (motor, gravity, min rise)

def gen_sb(log, rng, motor='fraction', gravity=9.81, min_rise=None, hz=30):
    log.start('SB', hz, '010000')
    sid = 4
    dt = 1.0 / hz
    y0 = 0.0
    state = {'v': 0.0, 'y': y0, 'vy': 0.0}

    def hp_row(alive=True, hp=100.0):
        log.row('hp', id=sid, hp=hp, pose=0, pos=[0.0, state['y'], 0.0], alive=alive, vel=[0.0, state['vy'], state['v']])

    def motor_tick(target):
        v = state['v']
        if motor == 'fraction':
            v = v + 0.35 * (target - v) if hz == 30 else v + (1 - 0.65 ** (30 * dt)) * (target - v)
        elif motor == 'linear':
            a = 12.0 * dt
            v = min(target, v + a) if target > v else max(target, v - a)
        else:
            v = target
        state['v'] = v

    if min_rise is None:
        log.mark('cfg-start', cfg='accel', i=1, n=7)
        for pose, vs in ((0, 3.6), (1, 2.4), (2, 1.0)):
            for r in range(3):
                log.mark('step', cfg='accel-%d' % pose, i=r + 1, k0=log.k, x={'a': 0, 'l': 1.0, 'on': 3.0})
                hp_row()
                log.tick()
                for _ in range(int(3.0 * hz)):
                    motor_tick(vs)
                    hp_row()
                    log.tick()
                log.mark('step-off', cfg='accel-%d' % pose, i=r + 1, k0=log.k)
                for _ in range(int(3.0 * hz)):
                    motor_tick(0.0)
                    hp_row()
                    log.tick()
        log.mark('cfg-done', cfg='accel', i=1, n=7)
        log.mark('cfg-start', cfg='drop', i=5, n=7)
        for r, h in enumerate((5, 8, 10, 12)):
            log.mark('drop', cfg='drop', i=r + 1, k0=log.k, x={'h': h, 'hp0': 100})
            state['y'], state['vy'] = float(h), 0.0
            hp_row()
            log.tick()
            while state['y'] > 0:
                state['vy'] -= gravity * dt
                state['y'] = max(0.0, state['y'] + state['vy'] * dt)
                hp_row()
                log.tick()
            state['vy'] = 0.0
            for _ in range(int(2 * hz)):
                hp_row()
                log.tick()
            log.mark('drop-end', cfg='drop', i=r + 1, k0=log.k, x={'h': h, 'hp1': 100, 'dead': False})
        log.mark('cfg-done', cfg='drop', i=5, n=7)
    else:
        log.mark('cfg-start', cfg='minrise', i=7, n=7)
        for zoom, (dmin, add) in ((False, (2.5, 1.0)), (True, (0.2, 0.3))):
            for r in range(3):
                da = dmin
                cfg = 'minrise-ads' if zoom else 'minrise-hip'
                log.mark('step', cfg=cfg, i=r + 1, k0=log.k, x={'a': 0, 'l': 1.0, 'on': 3.0})
                for t in range(int(3 * hz)):
                    mov = t >= 1
                    dm = dmin + (add if mov else 0.0)
                    if da < dm:
                        da = dm if min_rise == 'immediate' else min(dm, da + 15.0 * dt)
                    log.row('gs', id=sid, wid=1, src='srv', da=da, dmin=dm, rec=[0, 0, 0, 0], disp=[0, 0], ra=0, rr=0,
                            seed=0, ifs=0, fs=False, fir=False, tsls=5.0, mov=mov, jmp=False, jt=0, pose=0, zoom=zoom)
                    log.tick()
                log.mark('step-off', cfg=cfg, i=r + 1, k0=log.k)
                log.tick(int(hz))
        log.mark('cfg-done', cfg='minrise', i=7, n=7)


# ---------------------------------------------------------------------------------------------------------------------
# W-B falloff

def gen_wb(log, rng, shape='linear', m98b_end=150.0):
    log.start('WB', 30, '220000')
    W = {'M16A3': (25, 18.4, 8, 50), 'M98B': (95, 59, 15, m98b_end)}
    D = (5, 8, 10, 15, 20, 25, 30, 40, 50, 60, 75, 100, 125, 150, 175)
    T = 6
    for i, (w, kn) in enumerate(W.items()):
        label = 'wb-' + w
        log.mark('cfg-start', cfg=label, i=i + 1, n=2, x={'w': w, 'mode': 'B', 'clear': 180.0})
        log.mark('part', cfg=label, x={'p': 'chest'})
        for d in D:
            log.mark('dist', cfg=label, x={'d': d, 'zone': 'chest', 'mode': 'B'})
            for j in range(3):
                bone, mult = (3, 0.9) if rng.random() < 0.2 else (2, 1.0)
                s = min(1.0, max(0.0, (d - kn[2]) / (kn[3] - kn[2])))
                if shape == 'smooth':
                    s = s * s * (3 - 2 * s)
                dmg = (kn[0] + (kn[1] - kn[0]) * s) * mult
                dist = d + float(rng.normal(0, 0.02))
                fxd = d if not (d == 40 and j == 0) else d + 3.0
                log.row('dmg', victim=T, ph='pre', dmg=dmg, hp=100.0, bone=bone, bul=True, exp=False, cli=False,
                        giver=3, wid=1, dist=dist, fxd=float(fxd), pos=[0, 1.3, d], org=[0, 1.6, 0])
                log.row('dmg', victim=T, ph='post', dmg=dmg, hp=max(0.0, 100 - dmg))
                log.tick(30)
        log.mark('cfg-done', cfg=label, i=i + 1, n=2)


# ---------------------------------------------------------------------------------------------------------------------
# S-A regeneration

def gen_sa(log, rng, delay=10.0, abort=True):
    log.start('SA', 30, '230000')
    T = 6
    hz = 30
    state = {'hp': 100.0, 'k_last': None}

    def run_window(seconds, regen_from_k):
        for _ in range(int(seconds * hz)):
            if regen_from_k is not None and log.k >= regen_from_k[0] and state['hp'] < 100:
                state['hp'] = min(100.0, state['hp'] + 5.0 / hz)
            log.row('hp', id=T, hp=state['hp'], pose=0, pos=[0, 0, 8], alive=True)
            log.tick()

    def hit():
        log.row('dmg', victim=T, ph='pre', dmg=25.0, hp=state['hp'], bul=True, giver=3, dist=8.0, fxd=8.0)
        state['hp'] -= 25.0
        log.row('dmg', victim=T, ph='post', dmg=25.0, hp=state['hp'])

    log.mark('cfg-start', cfg='regen', i=1, n=3)
    for r in range(4):
        state['hp'] = 100.0
        log.mark('rep', cfg='regen', i=r + 1, k0=log.k)
        hit()
        kd = log.k
        log.mark('hit', cfg='regen', i=r + 1, k0=log.k)
        run_window(40, [kd + int(round(delay * hz))])
    log.mark('cfg-done', cfg='regen', i=1, n=3)
    log.mark('cfg-start', cfg='supp', i=2, n=3)
    for r in range(3):
        state['hp'] = 100.0
        log.mark('rep', cfg='supp', i=r + 1, k0=log.k)
        hit()
        kd = log.k
        log.mark('hit', cfg='supp', i=r + 1, k0=log.k)
        reg = [kd + int(round(delay * hz))]
        run_window(4, reg)
        log.mark('spray', cfg='supp', i=r + 1, k0=log.k)
        for j in range(10):
            log.row('ev', name='Player:SuppressedEnemy', id=3, id2=T)
            if abort:
                reg[0] = log.k + int(round(delay * hz))
            run_window(0.1, reg)
        run_window(34, reg)
        log.mark('supp-seen', cfg='supp', i=r + 1, x={'ev': 10})
    log.mark('cfg-done', cfg='supp', i=2, n=3)


# ---------------------------------------------------------------------------------------------------------------------
# M-A bleed and M-B capture

def gen_ma(log, rng, rule='A'):
    log.start('MA', 30, '030000')
    tk = {1: 350.0, 2: 350.0}
    hz = 30
    plan = [(0, 0), (1, 0), (2, 0), (3, 0), (2, 1), (1, 1)]
    for i, (a, b) in enumerate(plan):
        label = '%d-%d' % (a, b)
        log.mark('cfg-start', cfg=label, i=i + 1, n=len(plan), x={'a': a, 'b': b})
        cav = {1: 60.0 * a, 2: 60.0 * b}
        log.tick(5 * hz)
        k0 = log.k
        for _ in range(150 * hz):
            for team in (1, 2):
                own, en = cav[team], cav[3 - team]
                if rule == 'A':
                    rate = 15.0 if (en > 100 and en > own) else 0.0
                else:
                    rate = 7.5 * max(0.0, (en - own) / 60.0)
                tk[team] -= rate / 60.0 / hz
            log.row('tk', tm=[[1, tk[1], tk[1], cav[1]], [2, tk[2], tk[2], cav[2]]])
            log.tick()
        log.mark('hold', cfg=label, i=i + 1, k0=k0, k1=log.k - 1, own=[1] * a + [2] * b + [0] * (3 - a - b))
        log.mark('cfg-done', cfg=label, i=i + 1, n=len(plan))


def gen_mb(log, rng, capture='model', flip='lose_get'):
    log.start('MB', 30, '040000')
    hz = 30
    cp = 1

    def half(n):
        return 20.0 / n if capture == 'linear_n' else 20.0 / min(1 + 0.34 * (n - 1), 4)

    def series(cfg, n, frm):
        log.mark('series', cfg=cfg, i=n, k0=log.k, x={'rep': 1, 'cp': cp, 'n': n, 'from': frm})
        team, loc, ctrl = (2, 100.0, True) if frm == 2 else (0, 0.0, False)
        log.row('cp', cp=cp, team=team, loc=loc, ctrl=ctrl, n1=0, n2=0)
        log.tick(10)
        inside = 0
        for t in range(int(120 * hz)):
            if inside < n and t % 3 == 0:
                inside += 1
            rate = 100.0 / half(inside) / hz if inside == n else 0.0
            if team == 2 and rate:
                if flip == 'direct':
                    team, loc, ctrl = 1, 0.0, False
                else:
                    loc -= rate
                    if loc <= 0:
                        team, loc, ctrl = 0, 0.0, False
            elif rate:
                loc = min(100.0, loc + rate)
                if loc >= 100:
                    team, ctrl = 1, True
            log.row('cp', cp=cp, team=team, loc=loc, ctrl=ctrl, n1=inside, n2=0)
            log.tick()
            if ctrl and team == 1:
                break
        log.mark('series-end', cfg=cfg, i=n, k0=log.k, x={'rep': 1, 'ok': True})
        log.tick(60)

    log.mark('cfg-start', cfg='N', i=1, n=4)
    for n in range(1, 6):
        series('N', n, 0)
    log.mark('cfg-done', cfg='N', i=1, n=4)
    log.mark('cfg-start', cfg='FLIP', i=2, n=4)
    for n in (1, 2, 4):
        series('FLIP', n, 2)
    log.mark('cfg-done', cfg='FLIP', i=2, n=4)


# ---------------------------------------------------------------------------------------------------------------------

def generate(rule, variant, outdir, seed=1, n=None, hz=30):
    """Write one synthetic log for (rule, variant) into outdir; returns its path."""
    if rule not in VARIANTS or variant not in VARIANTS[rule]:
        raise ValueError('unknown rule/variant %s %s' % (rule, variant))
    rng = np.random.default_rng([seed, sum(map(ord, rule + variant))])
    log = Log()
    if rule == 'W1':
        nshots = n or (40 if variant == 'small' else 200)
        laws = {'disc': 'radius' if variant == 'radius' else 'area', 'proto': 'short' if variant == 'small' else 'full'}
        gen_wa(log, rng, laws, 'a', {'a': nshots}, hz)
    elif rule == 'W2':
        if variant in ('15', '8'):
            gen_wa(log, rng, {'decay': float(variant), 'recovery': 'exp'}, 'bde', {'b': 20, 'd': 4, 'e': 4}, hz)
        else:
            gen_sb(log, rng, min_rise=variant, hz=hz)
    elif rule == 'W3':
        laws = {'recovery': 'exp' if variant == 'exp' else 'linear', 'hcorr': 0.6 if variant == 'correlated' else 0.0}
        gen_wa(log, rng, laws, 'bde', {'b': 10, 'd': 10, 'e': 6}, hz)
    elif rule == 'W4':
        laws = {'rearm': variant, 'recovery': 'exp', 'rec_rate': 8.0}
        if variant == 'spreadmin':
            laws.update({'inc': 4.5, 'rec_rate': 30.0})
        if variant == 'timer:0.3':
            laws.update({'rec_rate': 30.0})
        gen_wa(log, rng, laws, 'ac', {'a': 30, 'c': 6, 'a_full': True}, hz, labels=[('M16A3-stand-hip', False)])
    elif rule == 'W5':
        gen_wb(log, rng, shape='smooth' if variant == 'smooth' else 'linear',
               m98b_end=120.0 if variant == '120' else 150.0)
    elif rule == 'S1':
        gen_sa(log, rng, delay=16.0 if variant == '16' else 10.0, abort=(variant != 'no-abort'))
    elif rule == 'S3':
        gen_sb(log, rng, motor='linear' if variant == 'linear' else 'fraction',
               gravity=15.0 if variant == 'g15' else 9.81, hz=hz)
    elif rule == 'M1':
        gen_ma(log, rng, variant)
    elif rule == 'M2':
        gen_mb(log, rng, capture='linear_n' if variant == 'linear_n' else 'model',
               flip='direct' if variant == 'direct' else 'lose_get')
    elif rule == 'T1':
        for h, stamp in ((30, '213000'), (60, '063000')):
            laws = {'recovery': 'exp', 'coupled': variant}
            gen_wa(log, rng, laws, 'bde', {'b': 10, 'd': 4, 'e': 4}, h, stamp)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, 'vu_synth_%s_%s.log' % (rule, variant.replace(':', '_').replace('.', '_')))
    log.write(path)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description='synthetic MH1 logs for the analysis rules')
    ap.add_argument('--rule')
    ap.add_argument('--variant')
    ap.add_argument('--out')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--n', type=int, default=None)
    ap.add_argument('--hz', type=int, default=30)
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args(argv)
    if a.list or not a.rule:
        for r, vs in VARIANTS.items():
            print('%s: %s' % (r, ' '.join(vs)))
        return 0
    if not a.out:
        print('synth: --out is required')
        return 1
    print(generate(a.rule, a.variant, a.out, a.seed, a.n, a.hz))
    return 0


if __name__ == '__main__':
    sys.exit(main())

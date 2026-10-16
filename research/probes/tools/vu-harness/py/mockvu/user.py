"""Scripted user for the mock VU runtime (DESIGN.md 13.6).

A Python agent with a soldier in the mock world. It joins, spawns, walks (teleports) to the RUNBOOK sites, types chat
(`!mh ...`) or console (`mh ...`) commands, reads the replies the mod prints to its client log and the `prompt` rows,
and answers them: equips the prompted weapon, aims at the prompted zone of the target bot and fires, throws grenades,
sprays beside a target, fires magazines and zooms for W-D, and confirms with `!mh ok`.

Scripts are generators: every `yield` lets the host advance one chunk of ticks (Driver.run). They mirror the RUNBOOK
order (sites first, then `!mh p0 ...`, then scenarios).
"""

import re

from . import host as H

STATUSES = ('done', 'aborted', 'stopped', 'stalled', 'guard')


class Timeout(Exception):
    pass


class User(object):
    def __init__(self, host, team=1, via='chat', aim_sd=0.0, arm_frac=0.0, seed=5):
        self.h = host
        self.U = host.srv.mock.U
        self.team = team
        self.via = via          # 'chat' or 'console' (chat=off: only the console path works)
        self.aim_sd = aim_sd
        self.arm_frac = arm_frac
        self.i_say = 0
        self.i_rows = 0
        self.spot = 'deploy'
        self.key = None
        self.auto_respawn = True
        self.dead_since = None
        self.responders = {}
        self.answered = []
        self.sent = []
        self.joined = False
        import random
        self.rnd = random.Random(seed)

    # -- primitives --------------------------------------------------------------------------------------------
    def spots(self):
        return self.h.level_list[0 if 'MP_Subway' in self.h.level_name else 1]['spots']

    def task(self, op, **kw):
        self.U.push(self.h.srv.table(dict(op=op, **kw)))

    def join(self):
        self.h.join(self.team)
        self.joined = True

    def spawn(self, spot=None, key=None):
        spot = spot or self.spot
        s = self.spots()[spot]
        self.spot = spot
        if key:
            self.key = key
        self.U.spawn(self.h.srv.table(s['pos']), self.h.srv.table(s['dir']), self.key)

    def goto(self, spot):
        s = self.spots()[spot]
        self.spot = spot
        self.task('move', p=s['pos'], dir=s['dir'])

    def state(self):
        return H.to_py(self.U.state())

    def cmd(self, text):
        """Send '!mh <text>' by chat or 'mh <text>' on the console."""
        self.sent.append(text)
        if self.via == 'console':
            self.h.console('mh ' + text)
        else:
            self.task('chat', text='!mh ' + text)

    def new_say(self):
        out = self.h.say[self.i_say:]
        self.i_say = len(self.h.say)
        return out

    def poll(self):
        """Called once per chunk: respawn when dead, answer prompt rows."""
        if not self.joined:
            return
        st = self.state()
        if not st['alive'] and self.auto_respawn and self.h.srv.mock.level_state == 'loaded':
            if self.dead_since is None:
                self.dead_since = self.h.k
            elif self.h.k - self.dead_since > self.h.hz:
                self.spawn()
                self.dead_since = None
        else:
            self.dead_since = None
        rows = self.h.rows
        while self.i_rows < len(rows):
            r = rows[self.i_rows]
            self.i_rows += 1
            if r.get('e') == 'prompt' and r.get('r') == 'S':
                fn = self.responders.get(r.get('kind')) or self.responders.get('*')
                self.answered.append(r.get('kind'))
                if fn:
                    fn(self, r)

    # -- waits (generators) ------------------------------------------------------------------------------------
    def wait(self, s):
        k1 = self.h.k + int(s * self.h.hz)
        while self.h.k < k1:
            yield

    def wait_idle(self, timeout=60):
        k1 = self.h.k + int(timeout * self.h.hz)
        while not self.U.idle():
            if self.h.k > k1:
                raise Timeout('user tasks not done')
            yield

    def wait_text(self, pattern, timeout=30):
        rx = re.compile(pattern)
        k1 = self.h.k + int(timeout * self.h.hz)
        seen = []
        while True:
            for line in self.new_say():
                seen.append(line)
                if rx.search(line):
                    return line
            if self.h.k > k1:
                raise Timeout('no reply matching %r (last: %r)' % (pattern, seen[-3:]))
            yield

    def wait_rows(self, pred, timeout=60, start=None):
        i = len(self.h.rows) if start is None else start
        k1 = self.h.k + int(timeout * self.h.hz)
        while True:
            while i < len(self.h.rows):
                r = self.h.rows[i]
                i += 1
                if pred(r):
                    return r
            if self.h.k > k1:
                raise Timeout('row not seen')
            yield

    def run(self, text, sc, timeout=3600):
        """Send a run command and wait for the run's end mark; returns its status."""
        start = len(self.h.rows)
        yield from self.wait_idle()
        self.cmd(text)
        r = yield from self.wait_rows(lambda r: r.get('e') == 'mark' and r.get('sc') == sc and
                                      r.get('lbl') in STATUSES and r.get('r') == 'S', timeout, start)
        return r['lbl']

    def say_wait(self, text, pattern, timeout=30):
        yield from self.wait_idle()
        self.new_say()
        self.cmd(text)
        line = yield from self.wait_text(pattern, timeout)
        return line

    # -- aim helpers ---------------------------------------------------------------------------------------------
    def target(self, sid, zone='chest'):
        p = H.to_py(self.U.bot_point(sid, zone))
        return p

    def shoot(self, sid, zone='chest', n=1, gap=0.6):
        arm = zone == 'chest' and self.rnd.random() < self.arm_frac
        p = self.target(sid, 'arm' if arm else zone)
        if p is None:
            return False
        self.task('tap', n=n, gap=gap, target=p, sd=self.aim_sd, arm=arm)
        return True


class Driver(object):
    """Runs a script generator against a host, polling the user once per chunk."""

    def __init__(self, host, user, chunk=10):
        self.h = host
        self.u = user
        self.chunk = chunk

    def run(self, gen, max_s=7200):
        kmax = self.h.k + int(max_s * self.h.hz)
        while True:
            self.u.poll()
            try:
                next(gen)
            except StopIteration as stop:
                return stop.value
            self.h.chunk(self.chunk)
            if self.h.k > kmax:
                raise Timeout('script exceeded %d s' % max_s)


# -- responders -----------------------------------------------------------------------------------------------------
WEAPON_OF_WID_NAME = {
    'Weapons/M16A4/U_M16A4': 'M16A3', 'Weapons/Model98B/U_M98B': 'M98B', 'Weapons/AK74M/U_AK74M': 'AK74M',
    'Weapons/M9/U_M9': 'M9', 'Weapons/Remington870/U_870': '870MCS',
}


def wid_names(host):
    out = {}
    for r in host.rows:
        if r.get('e') == 'dict' and r.get('kind') == 'wpn':
            out[r['id']] = r['name']
    return out


def respond_fire3(u, r):
    s = u.spots()['wall']
    p = [s['pos'][0], 1.6, s['pos'][2] + 15.0]
    u.task('move', p=s['pos'], dir=s['dir'])
    u.task('tap', n=3, gap=0.5, target=p, sd=0.0)


def respond_ok(u, r):
    u.cmd('ok')


def make_equip(order):
    """W-B U / W-C equip prompts: the next weapon of `order` (prompts carry wid only for W-B)."""
    state = {'i': 0}

    def fn(u, r):
        key = order[min(state['i'], len(order) - 1)]
        state['i'] += 1
        u.task('equip', key=key)
        u.key = key
        if key in ('M16A3', 'M98B', 'AK74M', 'M9', '870MCS'):
            u.goto('range')
            u.task('zoom', on=True)
        u.task('wait', s=0.5)
        u.cmd('ok')
    return fn


def respond_shoot(u, r):
    tid = r.get('id')
    zone = r.get('zone') or 'chest'
    u.task('wait', s=0.3)
    if not u.shoot(tid, zone, n=1):
        u.task('wait', s=1.0)


def respond_fire_direct(u, r):
    tid = r.get('id')
    p = u.target(tid, 'chest')
    if p is None:
        return
    if r.get('zone') == 'offset':
        p = [p[0], 0.05, p[2] + 2.0]
    u.task('reload')
    u.task('wait', s=3.2)
    u.task('fire', n=1, target=p, timeout=5)


def respond_throw(u, r):
    ring = None
    for x in reversed(u.h.rows):
        if x.get('e') == 'site' and x.get('name') == 'ring':
            ring = x['pos']
            break
    if ring is None:
        return
    u.task('throw', p=ring)


def respond_spray(u, r):
    tid = None
    for x in reversed(u.h.rows):
        if x.get('e') == 'prompt' and x.get('kind') == 'shoot':
            tid = x.get('id')
            break
    p = u.target(tid, 'chest') if tid else None
    if p is None:
        return
    p = [p[0], p[1], p[2] + 2.0]
    u.task('fire', n=25, target=p, timeout=5)


def respond_immune(u, r):
    tid = None
    for x in reversed(u.h.rows):
        if x.get('e') == 'prompt' and x.get('id'):
            tid = x.get('id')
            break
    for _ in range(12):
        p = u.target(tid, 'chest') if tid else None
        if p:
            u.task('tap', n=1, gap=0.1, target=p)
        u.task('wait', s=5.0)


def respond_wd(u, r):
    kind = r.get('kind')
    s = u.spots()['wall']
    p = [s['pos'][0], 1.6, s['pos'][2] + 15.0]
    if kind == 'mags':
        u.goto('wall')
        for _ in range(3):
            u.task('fire', n=31, target=p, timeout=6)
            u.task('reload')
            u.task('wait', s=3.0)
    elif kind == 'reloads':
        for n in (31, 15, 3):
            u.task('fire', n=n, target=p, timeout=5)
            u.task('reload')
            u.task('wait', s=3.0)
    elif kind == 'optics':
        u.task('zoom', on=True, level=1)
        u.task('wait', s=3.0)
        u.task('zoom', on=False, level=0)
        u.task('wait', s=1.0)
    elif kind == 'scope':
        u.task('equip', key='M98B')
        for lv in (1, 2):
            u.task('zoom', on=True, level=lv)
            u.task('wait', s=3.0)
        u.task('zoom', on=False, level=0)
        u.task('wait', s=1.0)
        u.task('equip', key='M16A3')
    u.cmd('ok')


# -- scripts (RUNBOOK order) -----------------------------------------------------------------------------------------
def setup(u, sites=('wall',), aim=False):
    """Join, spawn, record the sites (RUNBOOK steps 4-6) and calibrate the aim (P0.5)."""
    u.join()
    u.spawn('deploy')
    yield from u.wait(3.0)
    for name in sites:
        u.goto(name)
        yield from u.wait(0.5)
        line = yield from u.say_wait(name, r'^\[mh\] %s ' % name, 20)
        if ' ok' not in line and name != 'ring':
            raise Timeout('site %s not ok: %s' % (name, line))
    if aim:
        u.goto('wall')
        yield from u.wait(0.5)
        st = yield from u.run('p0 aim', 'P0', 300)
        if st != 'done':
            raise Timeout('p0 aim ' + st)
    u.goto('deploy' if 'wall' not in sites else 'wall')
    yield from u.wait(0.5)

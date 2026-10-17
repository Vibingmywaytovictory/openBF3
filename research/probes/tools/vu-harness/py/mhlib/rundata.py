"""Indexed access to one extracted run (research/measurements/<run>/) for the analysis rules.

Rows are kept in (r, q) order. Time is k/hz everywhere (DESIGN.md 3.3). Config windows run from a cfg-start mark to
the matching cfg-done / cfg-skip (or the next cfg-start / the end of the run).
"""

import os

from . import runs


class Window(object):
    def __init__(self, label, i, n, x, q0, q1, done):
        self.label, self.i, self.n, self.x = label, i, n, x or {}
        self.q0, self.q1, self.done = q0, q1, done

    def __repr__(self):
        return 'Window(%s %s/%s q%d-%d %s)' % (self.label, self.i, self.n, self.q0, self.q1, self.done)


class Run(object):
    def __init__(self, rows, meta, path=None):
        self.meta = meta or {}
        self.path = path
        self.id = self.meta.get('run') or (rows[0]['run'] if rows else '')
        parts = self.id.split('-')
        self.sc = parts[1] if len(parts) >= 4 else self.meta.get('sc')
        self.hz = float(parts[2]) if len(parts) >= 4 else float(self.meta.get('hz') or 30)
        self.rows = [r for r in rows if r.get('r') == 'S']
        self.crows = [r for r in rows if r.get('r') == 'C']
        self.by_e = {}
        for r in self.rows:
            self.by_e.setdefault(r['e'], []).append(r)
        self.metas = self.by_e.get('meta', [])

    @classmethod
    def load(cls, path):
        rows, meta = runs.read_run(path)
        return cls(rows, meta, path)

    @classmethod
    def load_many(cls, paths):
        return [cls.load(p) for p in paths if os.path.isdir(p)]

    def t(self, row):
        return row['k'] / self.hz

    def of(self, e, q0=None, q1=None, **match):
        out = []
        for r in self.by_e.get(e, []):
            if q0 is not None and r['q'] < q0:
                continue
            if q1 is not None and r['q'] > q1:
                continue
            ok = True
            for k, v in match.items():
                if r.get(k) != v:
                    ok = False
                    break
            if ok:
                out.append(r)
        return out

    def marks(self, lbl=None, q0=None, q1=None, cfg=None):
        out = []
        for r in self.of('mark', q0, q1):
            if lbl is not None and r.get('lbl') != lbl:
                continue
            if cfg is not None and r.get('cfg') != cfg:
                continue
            out.append(r)
        return out

    def windows(self):
        ms = self.of('mark')
        out = []
        cur = None
        for m in ms:
            lbl = m.get('lbl')
            if lbl == 'cfg-start':
                if cur is not None:
                    out.append(Window(cur['cfg'], cur.get('i'), cur.get('n'), cur.get('x'), cur['q'], m['q'], 'open'))
                cur = m
            elif lbl in ('cfg-done', 'cfg-skip') and cur is not None:
                out.append(Window(cur.get('cfg'), cur.get('i'), cur.get('n'), cur.get('x'), cur['q'], m['q'],
                                  'done' if lbl == 'cfg-done' else 'skip'))
                cur = None
        if cur is not None:
            last = self.rows[-1]['q'] if self.rows else cur['q']
            out.append(Window(cur.get('cfg'), cur.get('i'), cur.get('n'), cur.get('x'), cur['q'], last, 'open'))
        return out

    def meta_args(self):
        for m in self.metas:
            if isinstance(m.get('args'), dict):
                return m['args']
        return {}

    def dict_names(self, kind):
        out = {}
        for r in self.of('dict'):
            if r.get('kind') == kind:
                out[r['id']] = r.get('name')
        return out

    def series(self, e, id_key, ident, q0=None, q1=None):
        """Rows of type e with row[id_key] == ident, as a k -> last row dict."""
        out = {}
        for r in self.of(e, q0, q1):
            if r.get(id_key) == ident:
                out[r['k']] = r
        return out


def p0_values(runs_list):
    """{step: val} of the last p0 row per step across runs (P0 run first if given)."""
    out = {}
    for run in runs_list:
        for r in run.of('p0'):
            out[r['step']] = dict(r.get('val') or {}, ok=r.get('ok'))
    return out

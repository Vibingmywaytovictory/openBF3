"""Row validation against schema/rows_v1.json (DESIGN.md sections 4 and 9).

validate(row) checks the envelope, the row type, required keys, types (finite numbers, labels matching the label
pattern, enums) and the PII guard: any string (key or value) that looks like a GUID or an IPv4 address, or a
forbidden key anywhere, rejects the row as 'pii_suspect'; a key not in the schema rejects it as 'unknown_key'.
Optional keys whose value is null (the mod prints non-finite numbers as null) are removed and counted.
"""

import json
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA_PATH = os.path.join(HERE, '..', '..', 'schema', 'rows_v1.json')

ENVELOPE = ('v', 'run', 'sc', 'r', 'k', 't', 'ms', 'q', 'e')


class Schema(object):
    def __init__(self, path=SCHEMA_PATH):
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        self.d = d
        self.rows = d['rows']
        self.label = re.compile(d['label_pattern'])
        self.run = re.compile(d['run_pattern'])
        self.scen = set(d['scenarios'])
        self.realms = set(d['realms'])
        self.enums = d.get('enums', {})
        g = d['pii_guard']
        self.pii = [re.compile(p) for p in g['guid_patterns']] + [re.compile(g['ipv4_pattern'])]
        self.forbidden = set(k.lower() for k in g['forbidden_keys'])

    # -- PII --------------------------------------------------------------------------------------------------------
    def _pii_str(self, s):
        for p in self.pii:
            if p.search(s):
                return True
        return False

    def pii_suspect(self, v, depth=0):
        if depth > 8:
            return True
        if isinstance(v, str):
            return self._pii_str(v)
        if isinstance(v, dict):
            for k, x in v.items():
                if k.lower() in self.forbidden or self._pii_str(k) or self.pii_suspect(x, depth + 1):
                    return True
        elif isinstance(v, list):
            for x in v:
                if self.pii_suspect(x, depth + 1):
                    return True
        return False

    # -- types --------------------------------------------------------------------------------------------------------
    @staticmethod
    def _int(v):
        return isinstance(v, int) and not isinstance(v, bool)

    @staticmethod
    def _num(v):
        if isinstance(v, bool):
            return False
        if isinstance(v, int):
            return True
        return isinstance(v, float) and math.isfinite(v)

    def _lab(self, v):
        return isinstance(v, str) and self.label.match(v) is not None

    def _scalar(self, v):
        return v is None or isinstance(v, bool) or self._num(v) or self._lab(v)

    def check(self, t, v):
        """True if v has schema type t."""
        if t == 'i':
            return self._int(v)
        if t == 'n':
            return self._num(v)
        if t == 'b':
            return isinstance(v, bool)
        if t == 's':
            return self._lab(v)
        if t in ('v3', 'q4', 'a2', 'a4'):
            n = {'v3': 3, 'q4': 4, 'a2': 2, 'a4': 4}[t]
            return isinstance(v, list) and len(v) == n and all(self._num(x) for x in v)
        if t == 'ia':
            return isinstance(v, list) and len(v) <= 256 and all(self._int(x) for x in v)
        if t == 'sa':
            return isinstance(v, list) and len(v) <= 256 and all(self._lab(x) for x in v)
        if t == 'o':
            if not isinstance(v, dict) or len(v) > 64:
                return False
            for k, x in v.items():
                if not self._lab(k):
                    return False
                if isinstance(x, list):
                    if len(x) > 64 or not all(self._scalar(y) and y is not None for y in x):
                        return False
                elif not self._scalar(x):
                    return False
            return True
        if t == 'oo':
            return isinstance(v, dict) and len(v) <= 64 and all(self._lab(k) and self.check('o', x) for k, x in v.items())
        if t == 'tm':
            return isinstance(v, list) and len(v) <= 8 and all(
                isinstance(r, list) and len(r) == 4 and all(self._num(x) for x in r) for r in v)
        return False

    # -- rows ----------------------------------------------------------------------------------------------------------
    def validate(self, row, counts=None):
        """Return (row, None) with null optionals removed, or (None, reason)."""
        for k in ENVELOPE:
            if k not in row:
                return None, 'envelope'
        if row['v'] != 1 or not self._int(row['v']):
            return None, 'envelope'
        if not isinstance(row['run'], str) or not (self.run.match(row['run']) or row['run'] == 'none'):
            return None, 'envelope'
        if row['sc'] not in self.scen or row['r'] not in self.realms:
            return None, 'envelope'
        if not (self._int(row['k']) and self._int(row['ms']) and self._int(row['q']) and self._num(row['t'])):
            return None, 'envelope'
        if row['k'] < 0 or row['q'] < 0:
            return None, 'envelope'
        e = row['e']
        if not isinstance(e, str) or e not in self.rows:
            return None, 'unknown_e'
        if self.pii_suspect(row):
            return None, 'pii_suspect'
        spec = self.rows[e]
        req, opt = spec.get('req', {}), spec.get('opt', {})
        out = {}
        for k, v in row.items():
            if k in ENVELOPE:
                out[k] = v
                continue
            if k in req:
                t = req[k]
            elif k in opt:
                t = opt[k]
                if v is None:
                    if counts is not None:
                        counts['null_value'] = counts.get('null_value', 0) + 1
                    continue
            else:
                return None, 'unknown_key'
            if not self.check(t, v):
                if t == 'n' and v is None:
                    return None, 'non_finite'
                return None, 'bad_type'
            en = self.enums.get('%s.%s' % (e, k))
            if en is not None and v not in en:
                return None, 'bad_enum'
            out[k] = v
        for k in req:
            if k not in out:
                return None, 'missing_key'
        return out, None

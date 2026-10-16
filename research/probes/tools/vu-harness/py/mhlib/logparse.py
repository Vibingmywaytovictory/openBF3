"""Bounded, defensive VU log line reader (DESIGN.md section 9).

Lines are read as bytes with a 64 KiB cap (longer lines are counted 'too_long' and skipped to the next newline),
decoded as UTF-8 with replacement (counted 'bad_utf8'), matched against the MH1 regex, and parsed with json.loads
under limits: no NaN/Infinity, no duplicate keys, at most 64 keys per object, depth at most 6. Nothing is ever
evaluated. Only the first 400 lines of a file are scanned for the VU build and the VeniceEXT version.
"""

import json
import re

MAX_LINE = 64 * 1024
HEADER_LINES = 400
MAX_KEYS = 64
MAX_DEPTH = 6

RE_ROW = re.compile(r'\[VeniceEXT\] \[mharness\] MH1 (\{.*\})\s*$')
RE_BARE = re.compile(r'^MH1 (\{.*\})\s*$')
RE_BUILD = re.compile(r'Build (\d+)')
RE_VEXT = re.compile(r'VeniceEXT engine v([\d.]+)')
HINT = 'MH1 {'


class Malformed(Exception):
    """A row that cannot be used; .reason is the counter name."""

    def __init__(self, reason):
        Exception.__init__(self, reason)
        self.reason = reason


def _no_constant(name):
    raise Malformed('nan')


def _pairs(pairs):
    if len(pairs) > MAX_KEYS:
        raise Malformed('too_many_keys')
    out = {}
    for k, v in pairs:
        if k in out:
            raise Malformed('dup_key')
        out[k] = v
    return out


def _depth(v, d=0):
    if d > MAX_DEPTH:
        raise Malformed('too_deep')
    if isinstance(v, dict):
        for x in v.values():
            _depth(x, d + 1)
    elif isinstance(v, list):
        if len(v) > 4096:
            raise Malformed('too_long_array')
        for x in v:
            _depth(x, d + 1)


def parse_json(text):
    """Parse one row's JSON text under the limits; raises Malformed."""
    try:
        obj = json.loads(text, parse_constant=_no_constant, object_pairs_hook=_pairs)
    except Malformed:
        raise
    except (ValueError, RecursionError):
        raise Malformed('json')
    if not isinstance(obj, dict):
        raise Malformed('not_object')
    _depth(obj)
    return obj


def iter_lines(fobj, counts):
    """Yield (lineno, text) for each line of a binary file object; counts too_long / bad_utf8."""
    n = 0
    while True:
        b = fobj.readline(MAX_LINE + 1)
        if not b:
            return
        n += 1
        if len(b) > MAX_LINE and not b.endswith(b'\n'):
            counts['too_long'] = counts.get('too_long', 0) + 1
            while True:
                rest = fobj.readline(MAX_LINE)
                if not rest or rest.endswith(b'\n'):
                    break
            continue
        try:
            text = b.decode('utf-8')
        except UnicodeDecodeError:
            counts['bad_utf8'] = counts.get('bad_utf8', 0) + 1
            text = b.decode('utf-8', errors='replace')
        yield n, text.rstrip('\r\n')


def scan(fobj, counts):
    """Scan a log: returns (rows, header). rows = [(lineno, json_text, obj or None, reason or None)].

    counts receives: lines, mh1 (lines that look like rows), too_long, bad_utf8 and the per-line reasons.
    """
    rows = []
    header = {}
    for n, text in iter_lines(fobj, counts):
        counts['lines'] = counts.get('lines', 0) + 1
        if n <= HEADER_LINES:
            if 'build' not in header:
                m = RE_BUILD.search(text)
                if m and len(m.group(1)) <= 9:
                    header['build'] = int(m.group(1))
            if 'vext' not in header:
                m = RE_VEXT.search(text)
                if m and len(m.group(1)) <= 20:
                    header['vext'] = m.group(1)
        if HINT not in text:
            continue
        counts['mh1'] = counts.get('mh1', 0) + 1
        m = RE_ROW.search(text) or RE_BARE.search(text)
        if not m:
            rows.append((n, None, None, 'no_match'))
            continue
        js = m.group(1)
        try:
            obj = parse_json(js)
        except Malformed as exc:
            rows.append((n, js, None, exc.reason))
            continue
        rows.append((n, js, obj, None))
    return rows, header

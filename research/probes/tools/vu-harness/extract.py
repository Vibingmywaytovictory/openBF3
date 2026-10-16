"""Extract MH1 rows from VU server/client logs into research/measurements/<run>/ (DESIGN.md section 9).

    python -I extract.py --session 20261016..20261017 --server-logs <dir> [--server-logs <dir> ...] [--client-logs <dir> ...]
        --out <repo>\\research\\measurements [--since 2026-10-16T18:00] [--glob *.log]
        [--manifest-before x.csv --manifest-after y.csv] [--force] [--dry-run]

Reads only the files matching --glob (default *.log: the server log naming is a live check, L33) directly in each
log folder (no recursion). Refuses inputs under C:\\Program Files and the normal VU instance, and an output inside a
log folder, the install or the normal instance. Parsing is defensive (mhlib.logparse); rows are validated against
schema/rows_v1.json with the PII guard (mhlib.schema). Output per run: raw.jsonl sorted by (r, q) and meta.json;
deterministic and idempotent; an existing run folder is skipped unless --force. --dry-run writes nothing and prints
per run the rows, malformed counts, q gaps, trunc rows and the resume hint.
--session takes a date (YYYYMMDD), an inclusive range (YYYYMMDD..YYYYMMDD) or a comma list of either: a run id
carries the date its run STARTED (mh_clock), so an evening session with overnight and morning runs spans two dates
(RUNBOOK uses 20261016..20261017). Rows of runs outside the session are counted and their dates printed, never
dropped silently.
Exit 0; 2 if malformed > 1 % of MH1 lines or any pii_suspect; 1 on a usage/refusal error.
"""

import argparse
import csv
import datetime
import fnmatch
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'py'))

from mhlib import logparse, runs, schema  # noqa: E402

INSTALL = r'C:\Program Files\EA Games\Battlefield 3'
PROGRAM_FILES = (r'C:\Program Files', r'C:\Program Files (x86)')
NORMAL = r'C:\Users\olku\Documents\Battlefield 3\Server'
MANIFEST_HEADER = ['Path', 'Dir', 'Length', 'LastWriteUtc', 'CreationUtc', 'Sha256']


def normal_instances():
    out = [NORMAL]
    home = os.path.expanduser('~')
    out.append(os.path.join(home, 'Documents', 'Battlefield 3', 'Server'))
    return out


def norm(p):
    return os.path.normcase(os.path.realpath(os.path.abspath(p)))


def under(child, parent):
    # only the child is resolved; a refused root is never opened
    c, p = norm(child), os.path.normcase(os.path.abspath(parent)).rstrip('\\/')
    if c == p:
        return True
    if not p.endswith(os.sep):
        p += os.sep
    return c.startswith(p)


def lexically_under(child, parent):
    c = os.path.normcase(os.path.abspath(child))
    p = os.path.normcase(os.path.abspath(parent)).rstrip('\\/')
    return c == p or c.startswith(p + os.sep)


def _refused(p, roots):
    for r in roots:
        if lexically_under(p, r):
            return r
    for r in roots:
        if under(p, r):
            return r
    return None


def refused_input(d):
    return _refused(d, list(PROGRAM_FILES) + normal_instances())


def refused_output(out, logdirs):
    return _refused(out, [INSTALL] + list(PROGRAM_FILES) + normal_instances() + list(logdirs))


def list_logs(d, pattern, since):
    try:
        names = sorted(os.listdir(d))
    except OSError as exc:
        raise SystemExit('extract: cannot list %s (%s)' % (d, type(exc).__name__))
    out = []
    for n in names:
        p = os.path.join(d, n)
        if not fnmatch.fnmatch(n.lower(), pattern.lower()) or not os.path.isfile(p) or os.path.islink(p):
            continue
        if since is not None and datetime.datetime.fromtimestamp(os.path.getmtime(p)) < since:
            continue
        out.append(p)
    return out


def read_manifest(p):
    with open(p, encoding='utf-8-sig', newline='') as f:
        rd = csv.reader(f)
        rows = list(rd)
    if not rows or rows[0] != MANIFEST_HEADER:
        raise ValueError('header mismatch')
    body = rows[1:]
    if not body:
        raise ValueError('empty manifest')
    m = {}
    for r in body:
        if len(r) != len(MANIFEST_HEADER) or r[0] in m:
            raise ValueError('bad row')
        m[r[0]] = r
    return m


def compare_manifests(a, b):
    try:
        ma, mb = read_manifest(a), read_manifest(b)
    except (OSError, ValueError) as exc:
        return 'compare failed: %s' % exc
    added = sorted(set(mb) - set(ma))
    removed = sorted(set(ma) - set(mb))
    changed = sorted(k for k in set(ma) & set(mb) if ma[k] != mb[k])
    if not added and not removed and not changed:
        return 'install unchanged: %d items' % len(ma)
    return 'install CHANGED: %d added, %d removed, %d changed' % (len(added), len(removed), len(changed))


def sha_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


RE_RUN = re.compile(r'"run"\s*:\s*"([0-9A-Za-z-]{1,40})"')


def guess_run(js):
    """Best-effort run id from a malformed row's text (for attribution of the counter only)."""
    if not js:
        return None
    m = RE_RUN.search(js[:400])
    return m.group(1) if m else None


def parse_session(text):
    """'20261016', '20261016..20261017' or a comma list of these -> list of (lo, hi) date strings, or None."""
    out = []
    for part in text.split(','):
        part = part.strip()
        lo, sep, hi = part.partition('..')
        if not sep:
            hi = lo
        for d in (lo, hi):
            if not (len(d) == 8 and d.isdigit()):
                return None
            try:
                datetime.date(int(d[:4]), int(d[4:6]), int(d[6:]))
            except ValueError:
                return None
        if hi < lo:
            return None
        out.append((lo, hi))
    return out or None


def session_has(session, run):
    d = run[:8]
    return any(lo <= d <= hi for lo, hi in session)


def process_file(path, kind, sch, coll, totals, header_all, session):
    counts = {}
    with open(path, 'rb') as f:
        rows, header = logparse.scan(f, counts)
    for k, v in header.items():
        header_all.setdefault(k, v)
    for k, v in counts.items():
        totals[k] = totals.get(k, 0) + v
    items = []
    for n, js, obj, reason in rows:
        if obj is None:
            items.append((None, reason, guess_run(js)))
            continue
        row, why = sch.validate(obj, totals)
        if row is None:
            items.append((None, why, guess_run(js)))
            continue
        if session and row['run'] != 'none' and not session_has(session, row['run']):
            totals['other_session'] = totals.get('other_session', 0) + 1
            od = totals.setdefault('other_dates', {})
            d = row['run'][:8]
            od[d] = od.get(d, 0) + 1
            continue
        items.append((row, None, None))
    src = os.path.basename(path)
    coll.add_file_rows(items, src)
    return {'name': src, 'kind': kind, 'sha256': sha_file(path)}


def main(argv=None):
    ap = argparse.ArgumentParser(description='extract MH1 rows into run folders')
    ap.add_argument('--session', required=True,
                    help='YYYYMMDD, YYYYMMDD..YYYYMMDD (inclusive) or a comma list: run start dates to keep')
    ap.add_argument('--server-logs', action='append', default=[])
    ap.add_argument('--client-logs', action='append', default=[])
    ap.add_argument('--out', required=True)
    ap.add_argument('--since', default=None, help='ISO time: skip log files modified before it')
    ap.add_argument('--glob', default='*.log')
    ap.add_argument('--manifest-before')
    ap.add_argument('--manifest-after')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    session = parse_session(a.session)
    if session is None:
        print('extract: --session must be YYYYMMDD, YYYYMMDD..YYYYMMDD or a comma list of these')
        return 1
    since = None
    if a.since:
        try:
            since = datetime.datetime.fromisoformat(a.since)
        except ValueError:
            print('extract: bad --since')
            return 1
    logdirs = a.server_logs + a.client_logs
    if not logdirs:
        print('extract: no log folders given')
        return 1
    for d in logdirs:
        r = refused_input(d)
        if r:
            print('extract: refused input %s (inside %s)' % (d, r))
            return 1
        if not os.path.isdir(d):
            print('extract: not a folder: %s' % d)
            return 1
    r = refused_output(a.out, logdirs)
    if r:
        print('extract: refused output %s (inside %s)' % (a.out, r))
        return 1
    sch = schema.Schema()
    coll = runs.Collector()
    totals = {}
    header = {}
    sources = []
    for kind, dirs in (('server', a.server_logs), ('client', a.client_logs)):
        for d in dirs:
            for p in list_logs(d, a.glob, since):
                sources.append(process_file(p, kind, sch, coll, totals, header, session))
    manifests = None
    if a.manifest_before and a.manifest_after:
        manifests = {'before': os.path.basename(a.manifest_before), 'after': os.path.basename(a.manifest_after),
                     'compare': compare_manifests(a.manifest_before, a.manifest_after)}
    mh1 = totals.get('mh1', 0)
    bad = sum(sum(v.values()) for v in coll.malformed.values())
    pii = sum(v.get('pii_suspect', 0) for v in coll.malformed.values())
    too_long = totals.get('too_long', 0)
    print('extract: %d files, %d lines, %d MH1 lines, %d malformed (%s), %d too long, %d bad utf-8, %d runs' % (
        len(sources), totals.get('lines', 0), mh1, bad,
        ', '.join('%s %d' % kv for kv in sorted(merge(coll.malformed).items())) or 'none', too_long,
        totals.get('bad_utf8', 0), len(coll.runs)))
    if totals.get('other_session'):
        print('extract: WARNING %d rows of runs outside --session %s skipped (run dates: %s); widen --session to keep them'
              % (totals['other_session'], a.session,
                 ', '.join('%s %d rows' % kv for kv in sorted(totals.get('other_dates', {}).items()))))
    for run in sorted(coll.runs):
        items = coll.runs[run]
        srcs = [s for s in sources if s['name'] in coll.src_of.get(run, set())]
        meta = runs.run_meta(run, items, coll, srcs, header, manifests)
        line = '  %s: %d rows, malformed %s, q gaps %s, trunc %d' % (
            run, meta['rows'], sum(meta['malformed'].values()), meta['q_gaps'] or 0, meta['counts'].get('trunc', 0))
        if a.dry_run:
            print(line)
            for h in runs.resume_hints(run, items):
                print('    resume: ' + h)
            continue
        target = os.path.join(a.out, run)
        if os.path.isdir(target) and not a.force:
            print(line + ' (exists, skipped; --force to rewrite)')
            continue
        runs.write_run(a.out, run, items, meta)
        print(line + ' -> ' + target)
    if manifests:
        print('manifests: ' + manifests['compare'])
    if mh1 and (bad + too_long) * 100 > mh1 or pii:
        print('extract: exit 2 (malformed > 1 %% of MH1 lines or pii_suspect %d)' % pii)
        return 2
    return 0


def merge(d):
    out = {}
    for v in d.values():
        for k, n in v.items():
            out[k] = out.get(k, 0) + n
    return out


if __name__ == '__main__':
    sys.exit(main())

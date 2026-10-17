"""Split validated rows into runs and write research/measurements/<run>/ (DESIGN.md section 9).

Per run: raw.jsonl sorted by (r, q) (raw.jsonl.gz above 20 MB, written with mtime 0), meta.json with sorted keys.
Same inputs give byte-identical outputs. Also the q-gap count per realm, malformed counts, and resume hints.
"""

import gzip
import hashlib
import io
import json
import os

GZIP_ABOVE = 20 * 1024 * 1024
EXTRACTOR_VERSION = '1.0.0'


def canon(row):
    return json.dumps(row, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False)


class Collector(object):
    """Accumulates rows from several files; dedups (run, r, q)."""

    def __init__(self):
        self.runs = {}        # run -> list of (r, q, order, canon, row)
        self.seen = {}        # (run, r, q) -> canon
        self.order = 0
        self.gaps = {}        # run -> {realm: n}
        self.restarts = {}    # run -> {realm: n}
        self.malformed = {}   # run or '_' -> {reason: n}
        self.counts = {}      # global counters
        self.src_of = {}      # run -> set of source names

    def bump(self, d, key, sub, n=1):
        x = d.setdefault(key, {})
        x[sub] = x.get(sub, 0) + n

    def add_file_rows(self, rows, src=None):
        """rows: [(row, None)] or [(None, reason, run_guess)] in file order; tracks q gaps per realm."""
        last_q = {}
        for item in rows:
            row, reason, guess = item
            if row is None:
                self.bump(self.malformed, guess or '_', reason)
                continue
            run, r, q = row['run'], row['r'], row['q']
            lq = last_q.get(r)
            if lq is not None:
                if q > lq + 1:
                    self.bump(self.gaps, run, r, q - lq - 1)
                elif q <= lq:
                    self.bump(self.restarts, run, r)
            last_q[r] = q
            if run == 'none':
                self.counts['no_run'] = self.counts.get('no_run', 0) + 1
                continue
            if src is not None:
                self.src_of.setdefault(run, set()).add(src)
            c = canon(row)
            key = (run, r, q)
            old = self.seen.get(key)
            if old is not None:
                if old == c:
                    self.counts['duplicate'] = self.counts.get('duplicate', 0) + 1
                else:
                    self.bump(self.malformed, run, 'q_conflict')
                continue
            self.seen[key] = c
            self.order += 1
            self.runs.setdefault(run, []).append((r, q, self.order, c, row))


def run_meta(run, items, coll, sources, header, manifests):
    rows = [x[4] for x in items]
    counts = {}
    for row in rows:
        counts[row['e']] = counts.get(row['e'], 0) + 1
    metas = [dict((k, v) for k, v in row.items() if k not in ('v', 'run', 'r', 'q', 'ms')) for row in rows
             if row['e'] == 'meta']
    sc = run.split('-')[1] if run.count('-') >= 3 else None
    hz = int(run.split('-')[2]) if run.count('-') >= 3 else None
    build = None
    for m in metas:
        if isinstance(m.get('build'), int):
            build = m['build']
    if build is None:
        build = header.get('build')
    return {
        'run': run, 'sc': sc, 'hz': hz, 'vu_build': build, 'veniceext': header.get('vext'),
        'meta_rows': metas, 'counts': dict(sorted(counts.items())), 'rows': len(rows),
        'malformed': dict(sorted(coll.malformed.get(run, {}).items())),
        'q_gaps': dict(sorted(coll.gaps.get(run, {}).items())),
        'q_restarts': dict(sorted(coll.restarts.get(run, {}).items())),
        'sources': sources, 'extractor': EXTRACTOR_VERSION, 'manifests': manifests,
    }


def raw_bytes(items):
    items = sorted(items, key=lambda x: (x[0], x[1], x[2]))
    return ''.join(x[3] + '\n' for x in items).encode('ascii')


def write_run(outdir, run, items, meta):
    """Write <outdir>/<run>/raw.jsonl(.gz) and meta.json; returns the file names written."""
    d = os.path.join(outdir, run)
    os.makedirs(d, exist_ok=True)
    data = raw_bytes(items)
    written = []
    if len(data) > GZIP_ABOVE:
        buf = io.BytesIO()
        with gzip.GzipFile(filename='', mode='wb', fileobj=buf, mtime=0, compresslevel=6) as g:
            g.write(data)
        with open(os.path.join(d, 'raw.jsonl.gz'), 'wb') as f:
            f.write(buf.getvalue())
        written.append('raw.jsonl.gz')
        meta['raw'] = {'file': 'raw.jsonl.gz', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    else:
        with open(os.path.join(d, 'raw.jsonl'), 'wb') as f:
            f.write(data)
        written.append('raw.jsonl')
        meta['raw'] = {'file': 'raw.jsonl', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    with open(os.path.join(d, 'meta.json'), 'wb') as f:
        f.write((json.dumps(meta, sort_keys=True, indent=1, ensure_ascii=True) + '\n').encode('ascii'))
    written.append('meta.json')
    return written


def read_run(rundir):
    """Read a run folder written by write_run: (rows, meta)."""
    with open(os.path.join(rundir, 'meta.json'), encoding='utf-8') as f:
        meta = json.load(f)
    p = os.path.join(rundir, 'raw.jsonl')
    if os.path.exists(p):
        with open(p, 'rb') as f:
            data = f.read()
    else:
        with gzip.open(os.path.join(rundir, 'raw.jsonl.gz'), 'rb') as f:
            data = f.read()
    rows = [json.loads(line) for line in data.decode('ascii').splitlines() if line]
    return rows, meta


def resume_hints(run, items):
    """Last cfg-done per scenario -> '!mh run SC from=i+1' and the queue form."""
    last = {}
    for r, q, o, c, row in sorted(items, key=lambda x: (x[0], x[1], x[2])):
        if row['e'] == 'mark' and row.get('lbl') == 'cfg-done' and isinstance(row.get('i'), int):
            last[row['sc']] = (row['i'], row.get('n'))
    out = []
    for sc in sorted(last):
        i, n = last[sc]
        q = 'WA:all:from=%d' % (i + 1) if sc == 'WA' else '%s:from=%d' % (sc, i + 1)
        out.append('%s last cfg-done %d/%s -> !mh run %s from=%d, queue item %s' % (
            sc, i, n if n is not None else '?', sc, i + 1, q))
    return out

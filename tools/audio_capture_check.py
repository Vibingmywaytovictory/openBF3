"""PT1 snd (docs/design/pt1-snd.md): analyse an --audio-capture directory (mix.wav, cuts.csv, hitches.csv).

The capture is our own mixer's output in a real round (derived data under b/shots, never committed). The report:
the mix's isolated sample steps (and how many sit more than 200 ms after any start, i.e. not a sound's onset), zero
gaps inside sound (a decoder underflow), block-level drops, the cuts by kind with their steps, the cuts above
-40 dBFS that show in the mix, the loops parked more than twice, and the main thread's hitches.

usage: python -I tools/audio_capture_check.py <capture dir> [--top N]     (needs numpy)
"""
import csv
import struct
import sys
from collections import Counter, defaultdict

import numpy as np

RATE = 48000
BLOCK = 512


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            tag, ch, rate, _, _, bits = fmt
            assert tag == 3 and bits == 32, fmt
            x = np.frombuffer(body, dtype='<f4').reshape(-1, ch)
            return x, rate
        pos += 8 + size + (size & 1)
    raise ValueError('no data chunk')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    top = 12
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
    x, rate = read_wav(d + '/mix.wav')
    n = len(x)
    print(f'mix.wav: {n / rate:.1f} s, peak {np.abs(x).max():.3f}, rms {20 * np.log10(np.sqrt(np.mean(x.astype(np.float64) ** 2)) + 1e-30):.1f} dBFS')
    # first difference per channel
    dx = np.abs(np.diff(x, axis=0)).max(axis=1)
    # local scale: median |dx| over +-64 samples, via block medians of 128
    w = 128
    nb = len(dx) // w
    med = np.median(dx[:nb * w].reshape(nb, w), axis=1)
    med_s = np.repeat(med, w)
    med_s = np.concatenate([med_s, np.full(len(dx) - len(med_s), med[-1] if nb else 0)])
    # isolated spikes: |dx| > 0.01 and > 12x the local median
    spikes = np.nonzero((dx > 0.01) & (dx > 12.0 * np.maximum(med_s, 1e-6)))[0]
    on_block = np.sum(((spikes + 1) % BLOCK) == 0) if len(spikes) else 0
    print(f'isolated steps (|dx| > 0.01 and > 12x local median): {len(spikes)}, at a 512-block boundary: {on_block}')
    # zero gaps inside sound: runs of exact zero >= 64 frames with sound within 20 ms on both sides
    z = np.all(x == 0.0, axis=1)
    gaps = []
    i = 0
    zi = np.nonzero(np.diff(np.concatenate([[0], z.astype(np.int8), [0]])))[0]
    for a, b in zip(zi[0::2], zi[1::2]):
        if b - a >= 64 and a > 960 and b + 960 < n:
            before = np.sqrt(np.mean(x[a - 960:a] ** 2))
            after = np.sqrt(np.mean(x[b:b + 960] ** 2))
            if before > 1e-3 and after > 1e-3:
                gaps.append((a, b - a))
    print(f'zero gaps >= 64 frames inside sound (> -60 dBFS rms either side): {len(gaps)}' + (f'; first at {gaps[0][0] / rate:.3f} s ({gaps[0][1]} frames)' if gaps else ''))
    # block rms jumps
    nb = n // BLOCK
    br = np.sqrt(np.mean(x[:nb * BLOCK].astype(np.float64).reshape(nb, BLOCK, -1) ** 2, axis=(1, 2)))
    bdb = 20 * np.log10(br + 1e-9)
    jumps_down = np.nonzero((bdb[:-1] > -40) & (bdb[1:] - bdb[:-1] < -12))[0]
    jumps_up = np.nonzero((bdb[1:] - bdb[:-1] > 18) & (bdb[1:] > -40))[0]
    print(f'block rms drops > 12 dB (from > -40 dBFS): {len(jumps_down)}; rises > 18 dB: {len(jumps_up)} (rises are mostly shot onsets)')
    # cuts
    try:
        rows = list(csv.DictReader(open(d + '/cuts.csv')))
    except FileNotFoundError:
        rows = []
    kinds = Counter(r['kind'] for r in rows)
    print('cuts by kind:', dict(kinds))
    steps = defaultdict(list)
    for r in rows:
        steps[r['kind']].append(float(r['step']))
    for k, v in sorted(steps.items()):
        v = np.array(v)
        if k in ('callgap', 'lateblock'):
            print(f'  {k}: {len(v)}, max {v.max():.1f} ms, > 50 ms {np.sum(v > 50)}')
            continue
        print(f'  {k}: {len(v)}, > -60 dBFS {np.sum(v > 1e-3)}, > -40 dBFS {np.sum(v > 1e-2)}, > -26 dBFS {np.sum(v > 0.05)}, max {v.max():.4f}')
    # do audible cuts show in the mix? the jump at the block boundary against the local median
    vis = 0
    audible = [r for r in rows if r['kind'] in ('drop', 'pend', 'kill', 'finish', 'resume') and float(r['step']) > 1e-2]
    for r in audible:
        f = int(r['frame'])
        if 0 < f < n - 1:
            j = np.abs(x[f] - x[f - 1]).max()
            loc = med_s[min(f, len(med_s) - 1)]
            if j > 4 * max(loc, 1e-6):
                vis += 1
    print(f'cuts > -40 dBFS (drop/pend/kill/finish/resume): {len(audible)}, visible at their boundary in the mix (> 4x local median): {vis}')
    progs = Counter((r['kind'], r['program']) for r in audible)
    print('  top programs:', progs.most_common(top))
    loud = sorted(audible, key=lambda r: -float(r['step']))[:top]
    for r in loud:
        print(f"  {float(r['time_s']):8.3f} s {r['kind']:7s} step {float(r['step']):.4f} loud {float(r['loudness_db']):6.1f} dB at {float(r['distance_m']):6.1f} m  {r['program']}")
    # pend/resume flapping: the same handle pended more than twice
    ph = Counter(r['handle'] for r in rows if r['kind'] == 'pend')
    flap = [(h, c) for h, c in ph.items() if c > 2]
    print(f'loops pended more than twice (flapping): {len(flap)}; worst {sorted(flap, key=lambda t: -t[1])[:5]}')
    # isolated steps against the starts: an onset follows a start / resume block, a glitch need not
    starts = np.array(sorted(int(r['frame']) for r in rows if r['kind'] in ('start', 'resume')), dtype=np.int64)
    if len(spikes) and len(starts):
        idx = np.searchsorted(starts, spikes, side='right') - 1
        since = np.where(idx >= 0, spikes - starts[np.clip(idx, 0, None)], 10 ** 12)
        far = spikes[since >= 0.2 * rate]
        print(f'isolated steps within 11 / 200 ms after a start or resume block: {np.sum(since < 0.011 * rate)} / {np.sum(since < 0.2 * rate)} of {len(spikes)}; '
              f'more than 200 ms after any: {len(far)}')
        for f in far[:top]:
            before = np.sqrt(np.mean(x[max(0, f - 480):f] ** 2))
            after = np.sqrt(np.mean(x[f + 1:f + 481] ** 2))
            print(f'  {f / rate:9.4f} s |dx| {dx[f]:.4f} (local median {med_s[f]:.5f}), rms 10 ms before {before:.4f} after {after:.4f}')
    try:
        hs = list(csv.DictReader(open(d + '/hitches.csv')))
        g = [float(h['gap_ms']) for h in hs]
        print(f'main-thread hitches > 50 ms: {len(g)}' + (f', max {max(g):.0f} ms' if g else ''))
    except FileNotFoundError:
        pass


if __name__ == '__main__':
    sys.exit(main())

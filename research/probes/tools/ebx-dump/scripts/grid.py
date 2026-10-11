import json, sys
g = json.load(open(sys.argv[1]))['partitions'][0]
mc = json.load(open(sys.argv[2]))['partitions'][0]
pairs = {i['$guid']: i for i in mc['instances'] if i['$type'] == 'MaterialContainerPair'}
inst = {i['$guid']: i for i in g['instances']}
grid = [i for i in g['instances'] if i['$type'] == 'MaterialGridData'][0]
imap = grid['MaterialIndexMap']
def u8(x): return x & 0xFF
def row(pidx): return imap[u8(pidx)]
def dmg(a, b):
    ra, rb = row(a), row(b)
    items = grid['InteractionGrid'][ra]['Items'][rb]
    out = []
    for r in items['PhysicsPropertyProperties']:
        d = inst[r['instance']]
        out.append({k: v for k, v in d.items() if not k.startswith('$')})
    return ra, rb, out
names = dict(a.split('=') for a in sys.argv[3].split(','))  # label=guidprefix
targets = dict(a.split('=') for a in sys.argv[4].split(','))
def find(prefix): return next(p for g_, p in pairs.items() if g_.startswith(prefix))
for bl, bp in names.items():
    b = find(bp)
    print(f'## projectile {bl} pair {b["$guid"]} PhysicsPropertyIndex {b["PhysicsPropertyIndex"]} PhysicsMaterialIndex {b["PhysicsMaterialIndex"]} FlagsAndIndex {b["FlagsAndIndex"]:#x}')
    for tl, tp in targets.items():
        t = find(tp)
        print(' ', tl, t['PhysicsPropertyIndex'], dmg(b['PhysicsPropertyIndex'], t['PhysicsPropertyIndex']))

import sys, collections
src, out = sys.argv[1], sys.argv[2]
first = {}; sbs = collections.defaultdict(set); bundles = collections.defaultdict(set)
for line in open(src, encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) < 6 or f[2] != 'ebx': continue
    k = f[3].lower()
    first.setdefault(k, (f[0], f[1]))
    sbs[k].add(f[0]); bundles[k].add(f[1])
with open(out, 'w', encoding='utf-8') as w:
    w.write('partition\tfirst_superbundle\tfirst_bundle\tn_superbundles\tn_bundles\n')
    for k in sorted(first):
        w.write(f'{k}\t{first[k][0]}\t{first[k][1]}\t{len(sbs[k])}\t{len(bundles[k])}\n')

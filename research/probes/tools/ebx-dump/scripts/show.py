import json, sys, collections
f = sys.argv[1]; types = set(sys.argv[2].split(',')) if len(sys.argv) > 2 and sys.argv[2] else None
part = sys.argv[3] if len(sys.argv) > 3 else None
d = json.load(open(f))
def compact(v):
    if isinstance(v, dict):
        if '$ref' in v:
            return f"->{v['$ref'] if v['$ref']!='internal' else ''}#{v.get('type')}:{v.get('instance','')[:8]}"
        if v.get('$type') == 'Vec3': return [v['x'], v['y'], v['z']]
        if v.get('$type') == 'Vec2': return [v['x'], v['y']]
        return {k: compact(x) for k, x in v.items() if k not in ('$chain',)}
    if isinstance(v, list): return [compact(x) for x in v]
    return v
for p in d['partitions']:
    if part and p['partition'] != part: continue
    if not types:
        print(p['partition'], p['primary_type'], collections.Counter(i['$type'] for i in p['instances']).most_common()); continue
    for i in p['instances']:
        if i['$type'] in types:
            print('##', p['partition'], i['$type'], i['$guid'])
            print(json.dumps(compact(i), separators=(',', ':')))

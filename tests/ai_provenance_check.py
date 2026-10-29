import os
import re
import sys

ROW_START = '| bots |'
PAIR = re.compile(r'`([^`]+)` <- ((?:`[^`]+`(?: \+ )?)+)')
SOURCE = re.compile(r'`([^`]+)`')
ORIGINAL = re.compile(r'openBF3-original: ((?:`[^`]+`(?:, )?)+)')
NOT_PORTED = re.compile(r'`([^`]+)` \(not ported: ([^)]+)\)')
SKIP = 77


def resolve(openbf2, path):
    for base in (openbf2, os.path.join(openbf2, 'AIDLL', 'AI')):
        full = os.path.join(base, path)
        if os.path.exists(full):
            return os.path.relpath(full, openbf2).replace('\\', '/')
    return None


def main():
    root = os.path.abspath(sys.argv[1])
    openbf2 = sys.argv[2] if len(sys.argv) > 2 else ''
    failures = []
    with open(os.path.join(root, 'docs', 'architecture.md'), encoding='utf-8') as f:
        rows = [line for line in f if line.startswith(ROW_START)]
    if len(rows) != 1:
        print(f'provenance: {len(rows)} rows start with "{ROW_START}" in docs/architecture.md, expected 1')
        return 1
    row = rows[0]
    if 'GPL-3.0' not in row or 'OpenBF2' not in row:
        failures.append('the bots row does not name OpenBF2 and its GPL-3.0 licence')
    pairs = {}
    for target, sources in PAIR.findall(row):
        pairs[target] = SOURCE.findall(sources)
    originals = set()
    for group in ORIGINAL.findall(row):
        originals.update(SOURCE.findall(group))
    not_ported = {path: reason.strip() for path, reason in NOT_PORTED.findall(row)}

    ai_root = os.path.join(root, 'src', 'bf3_ai')
    files = []
    for dp, dn, fn in os.walk(ai_root):
        for name in fn:
            files.append(os.path.relpath(os.path.join(dp, name), ai_root).replace('\\', '/'))
    if not files:
        failures.append('src/bf3_ai has no files')
    for rel in sorted(files):
        if rel in pairs and rel in originals:
            failures.append(f'src/bf3_ai/{rel}: listed both as ported and as openBF3-original')
        elif rel not in pairs and rel not in originals:
            failures.append(f'src/bf3_ai/{rel}: not named in the bots row of docs/architecture.md')
    for target in sorted(set(pairs) | originals):
        if target not in files:
            failures.append(f'the bots row names src/bf3_ai/{target}, which does not exist')

    if not os.path.isdir(openbf2) or not os.path.isfile(os.path.join(openbf2, 'AIDLL', 'AI', 'Main', 'BF2AI.cpp')):
        for f in failures:
            print(f)
        if failures:
            print(f'provenance: {len(failures)} failure(s)')
            return 1
        print(f'provenance: OpenBF2 Code/BF2/AI not found at "{openbf2}"; the source paths are not checked')
        return SKIP

    ported_dirs = set()
    for target, sources in pairs.items():
        for source in sources:
            found = resolve(openbf2, source)
            if found is None:
                failures.append(f'src/bf3_ai/{target} <- {source}: no such OpenBF2 file')
            else:
                ported_dirs.add(os.path.dirname(found))
    listed = {}
    for path, reason in not_ported.items():
        found = resolve(openbf2, path)
        if found is None:
            failures.append(f'not ported: {path}: no such OpenBF2 path')
        elif not reason:
            failures.append(f'not ported: {path}: no reason')
        else:
            listed[found] = reason
    for dp, dn, fn in os.walk(openbf2):
        if not fn:
            continue
        rel = os.path.relpath(dp, openbf2).replace('\\', '/')
        if rel not in ported_dirs and rel not in listed:
            failures.append(f'OpenBF2 directory {rel}: neither ported nor listed as not ported with a reason')
    for f in failures:
        print(f)
    print(f'provenance: {len(pairs)} ported files, {len(originals)} openBF3-original, {len(listed)} not ported, '
          f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

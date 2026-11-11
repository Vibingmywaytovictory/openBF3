import os
import re
import subprocess
import sys

ROW_START = '| bots |'
PIN = 'c47317b'
EARLIER = 'ccf3099'
AI_ROOT = 'Code/BF2/AI'
PAIR = re.compile(r'`([^`]+)` <- ((?:`[^`]+`(?: \+ )?)+)(?: @([0-9a-f]+))?')
SOURCE = re.compile(r'`([^`]+)`')
ORIGINAL = re.compile(r'openBF3-original: ((?:`[^`]+`(?:, )?)+)')
NOT_PORTED = re.compile(r'`([^`]+)` \(not ported: ([^)]+)\)')
SKIP = 77


class Tree:
    def __init__(self, git, repo, ref):
        out = subprocess.run([git, '-C', repo, 'ls-tree', '-r', '--name-only', ref, AI_ROOT],
                             capture_output=True, text=True)
        self.ok = out.returncode == 0 and out.stdout.strip() != ''
        self.files = set()
        self.dirs = set()
        if not self.ok:
            return
        for line in out.stdout.splitlines():
            rel = line[len(AI_ROOT) + 1:]
            self.files.add(rel)
            self.dirs.add(os.path.dirname(rel).replace('\\', '/'))

    def resolve(self, path):
        for base in ('', 'AIDLL/AI/'):
            full = base + path
            if full.endswith('/*'):
                if full[:-2] in self.dirs:
                    return full[:-2]
            elif full in self.files or full in self.dirs:
                return full
        return None


def matches(target, rel):
    if target.endswith('/*'):
        return os.path.dirname(rel) == target[:-2]
    return rel == target


def main():
    root = os.path.abspath(sys.argv[1])
    openbf2 = sys.argv[2] if len(sys.argv) > 2 else ''
    git = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else 'git'
    failures = []
    with open(os.path.join(root, 'docs', 'architecture.md'), encoding='utf-8') as f:
        rows = [line for line in f if line.startswith(ROW_START)]
    if len(rows) != 1:
        print(f'provenance: {len(rows)} rows start with "{ROW_START}" in docs/architecture.md, expected 1')
        return 1
    row = rows[0]
    if 'GPL-3.0' not in row or 'OpenBF2' not in row:
        failures.append('the bots row does not name OpenBF2 and its GPL-3.0 licence')
    pairs = []
    for target, sources, pin in PAIR.findall(row):
        if pin and pin != PIN:
            failures.append(f'src/bf3_ai/{target} <- @{pin}: the pinned commit is {PIN}')
        pairs.append((target, SOURCE.findall(sources), pin == PIN))
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
    exact = {target for target, _, _ in pairs if not target.endswith('/*')}
    for rel in sorted(files):
        covered = [target for target, _, _ in pairs if matches(target, rel)]
        if rel in exact and rel in originals:
            failures.append(f'src/bf3_ai/{rel}: listed both as ported and as openBF3-original')
        elif not covered and rel not in originals:
            failures.append(f'src/bf3_ai/{rel}: not named in the bots row of docs/architecture.md')
    for target, _, _ in pairs:
        if not any(matches(target, rel) for rel in files):
            failures.append(f'the bots row names src/bf3_ai/{target}, which does not exist')
    for target in sorted(originals):
        if target not in files:
            failures.append(f'the bots row names src/bf3_ai/{target}, which does not exist')

    repo = ''
    if os.path.isdir(openbf2):
        out = subprocess.run([git, '-C', openbf2, 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
        repo = out.stdout.strip() if out.returncode == 0 else ''
    pinned = Tree(git, repo, PIN) if repo else None
    earlier = Tree(git, repo, EARLIER) if repo else None
    if pinned is None or not pinned.ok or not earlier.ok:
        for f in failures:
            print(f)
        if failures:
            print(f'provenance: {len(failures)} failure(s)')
            return 1
        print(f'provenance: no OpenBF2 clone with commits {PIN} and {EARLIER} at "{openbf2}"; '
              'the source paths are not checked')
        return SKIP

    ported_dirs = set()
    for target, sources, at_pin in pairs:
        tree = pinned if at_pin else earlier
        for source in sources:
            found = tree.resolve(source)
            if found is None:
                ref = PIN if at_pin else EARLIER
                failures.append(f'src/bf3_ai/{target} <- {source}: no such OpenBF2 path at {ref}')
                continue
            ported_dirs.add(found if source.endswith('/*') else os.path.dirname(found))
    listed = {}
    for path, reason in not_ported.items():
        found = pinned.resolve(path)
        if found is None:
            failures.append(f'not ported: {path}: no such OpenBF2 path at {PIN}')
        elif not reason:
            failures.append(f'not ported: {path}: no reason')
        else:
            listed[found] = reason
    for rel in sorted(pinned.dirs):
        if rel not in ported_dirs and rel not in listed:
            failures.append(f'OpenBF2 directory {rel} at {PIN}: neither ported nor listed as not ported with a reason')
    for f in failures:
        print(f)
    print(f'provenance: {len(pairs)} mappings, {len(originals)} openBF3-original, {len(listed)} not ported, '
          f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

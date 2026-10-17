r"""Run every S5 harness check in order and print one PASS/FAIL line per stage (DESIGN.md section 14).

    python -I research\probes\tools\vu-harness\check_all.py [--phase0] [--no-install] [--no-cert] [--full]
                                                                [--vudocs <dir>]

Stages: layout+json, invgen (mh_inventory.lua in sync with the API inventory), luaload (Lua 5.4 load of every mod
file), apicheck (every VU member the mod uses is in the inventory, 0 errors), unittests, mock-e2e (every scenario
against the mock VU under both law variants and the fault set), powershell (manifest/compare on a scratch tree,
setup-instance under -WhatIf and against a scratch target, and one read-only manifest of the real install that prints
only its summary line; --no-install skips that last run).
Exit 0 only if every stage passes. A tool that exits 3 is not built yet (skeleton) and counts as a failure.
When the mock e2e suite passes, mockrun.py writes mod/mharness/ext/shared/mh_cert.lua (git-ignored): the scenarios the
mod may run tonight (--phase0: Phase 0 and the attended U scenarios only; --no-cert: leave the file alone).
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
DEFAULT_VUDOCS = r'C:\Users\olku\VU-Docs'

REQUIRED = [
    'DESIGN.md', 'RUNBOOK.md', 'luaload.py', 'apicheck.py', 'extract.py', 'analyze.py', 'synth.py', 'mockrun.py',
    'schema/rows_v1.json', 'schema/api_inventory.json', 'data/weapon_constants.json',
    'ps/install-manifest.ps1', 'ps/install-compare.ps1', 'ps/setup-instance.ps1',
    'mod/mharness/mod.json', 'mod/mharness/ext/shared/__init__.lua', 'mod/mharness/ext/server/__init__.lua',
    'mod/mharness/ext/client/__init__.lua', 'py/mhlib/vudocs.py', 'py/mockvu/host.py', 'py/mockvu/laws.py',
    'tests/ps/run-ps-tests.ps1',
]


def stage_layout():
    missing = [p for p in REQUIRED if not os.path.isfile(os.path.join(HERE, *p.split('/')))]
    for p in ('schema/rows_v1.json', 'schema/api_inventory.json', 'data/weapon_constants.json', 'mod/mharness/mod.json'):
        try:
            with open(os.path.join(HERE, *p.split('/')), encoding='utf-8') as f:
                json.load(f)
        except (OSError, ValueError) as exc:
            missing.append('%s (%s)' % (p, type(exc).__name__))
    with open(os.path.join(HERE, 'mod', 'mharness', 'mod.json'), encoding='utf-8') as f:
        mod = json.load(f)
    if mod.get('HasVeniceEXT') is not True or mod.get('Dependencies', {}).get('veniceext') != '^1.15.0':
        missing.append('mod.json HasVeniceEXT/veniceext')
    return (not missing), ', '.join(missing)


def stage_invgen():
    sys.path.insert(0, os.path.join(HERE, 'py'))
    from mhlib import invgen
    ok = invgen.generate(HERE, write=False)
    return ok, 'mh_inventory.lua in sync' if ok else 'mh_inventory.lua out of sync: run py/mhlib/invgen.py'


def run(args, timeout=3600):
    p = subprocess.run(args, cwd=HERE, capture_output=True, text=True, timeout=timeout)
    lines = (p.stdout + p.stderr).strip().splitlines()
    fails = [x.strip() for x in lines if x.strip().startswith('FAIL')][:4]
    tail = [x.strip() for x in lines[-2:]]
    return p.returncode, ' | '.join(fails + tail)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-install', action='store_true', help='skip the read-only manifest of the real install')
    ap.add_argument('--with-install', action='store_true', help=argparse.SUPPRESS)  # the default since S5
    ap.add_argument('--no-cert', action='store_true', help='do not write mh_cert.lua')
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--phase0', action='store_true')
    ap.add_argument('--vudocs', default=DEFAULT_VUDOCS)
    a = ap.parse_args(argv)
    stages = [('layout+json', stage_layout), ('invgen', stage_invgen)]
    stages.append(('luaload', lambda: run([PY, '-I', 'luaload.py', '--quiet'])))
    vud = ['--vudocs', a.vudocs] if os.path.isdir(a.vudocs) else []
    stages.append(('apicheck', lambda: run([PY, '-I', 'apicheck.py'] + vud)))
    stages.append(('unittests', lambda: run([PY, '-I', '-m', 'unittest', 'discover', '-s', 'tests', '-t', '.'])))
    suite = ['--suite', 'phase0'] if a.phase0 else ['--suite', 'all']
    extra = (['--full'] if a.full else []) + ([] if a.no_cert else ['--write-cert'])
    stages.append(('mock-e2e', lambda: run([PY, '-I', 'mockrun.py'] + suite + extra)))
    ps = ['pwsh', '-NoProfile', '-File', os.path.join('tests', 'ps', 'run-ps-tests.ps1')]
    stages.append(('powershell', lambda: run(ps + ([] if a.no_install else ['-WithInstall']))))
    ok_all = True
    for name, fn in stages:
        try:
            res = fn()
        except (OSError, subprocess.SubprocessError, ImportError) as exc:
            res = (99, type(exc).__name__)
        if isinstance(res[0], bool):
            ok, detail = res
        else:
            code, detail = res
            ok = code == 0
            if code == 3:
                detail = 'NOT BUILT ' + detail
        ok_all &= ok
        print('%-12s %s  %s' % (name, 'PASS' if ok else 'FAIL', detail))
        sys.stdout.flush()
    if a.phase0:
        print('phase0 gate passed: Phase 0 and the attended items may run; the overnight queue may not'
              if ok_all else 'phase0 gate FAILED')
    else:
        print('all checks passed' if ok_all else 'checks FAILED')
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())

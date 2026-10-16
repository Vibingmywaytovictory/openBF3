"""Run every S5 harness check in order and print one PASS/FAIL line per stage (DESIGN.md section 14).

    python -I research\\probes\\tools\\vu-harness\\check_all.py [--phase0] [--with-install] [--full] [--vudocs <dir>]

Exit 0 only if every stage passes. A tool that exits 3 is not built yet (skeleton) and counts as a failure.
--phase0 is the staged gate of DESIGN.md section 14: the same stages, with the mock e2e suite limited to Phase 0 and
the attended U scenarios; it certifies only those scenarios (mh_cert.lua, written by mockrun.py once it is built).
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


def run(args, timeout=1800):
    p = subprocess.run(args, cwd=HERE, capture_output=True, text=True, timeout=timeout)
    tail = (p.stdout + p.stderr).strip().splitlines()[-3:]
    return p.returncode, ' | '.join(tail)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--with-install', action='store_true')
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--phase0', action='store_true')
    ap.add_argument('--vudocs', default=DEFAULT_VUDOCS)
    a = ap.parse_args(argv)
    stages = [('layout+json', stage_layout)]
    stages.append(('luaload', lambda: run([PY, '-I', 'luaload.py', '--quiet'])))
    vud = ['--vudocs', a.vudocs] if os.path.isdir(a.vudocs) else []
    stages.append(('apicheck', lambda: run([PY, '-I', 'apicheck.py'] + vud)))
    stages.append(('unittests', lambda: run([PY, '-I', '-m', 'unittest', 'discover', '-s', 'tests', '-t', '.'])))
    suite = ['--suite', 'phase0'] if a.phase0 else ['--suite', 'all']
    stages.append(('mock-e2e', lambda: run([PY, '-I', 'mockrun.py'] + suite + (['--full'] if a.full else []))))
    ps = ['pwsh', '-NoProfile', '-File', os.path.join('tests', 'ps', 'run-ps-tests.ps1')]
    stages.append(('powershell', lambda: run(ps + (['-WithInstall'] if a.with_install else []))))
    ok_all = True
    for name, fn in stages:
        try:
            res = fn()
        except (OSError, subprocess.SubprocessError) as exc:
            res = (99, type(exc).__name__)
        if isinstance(res[0], bool):
            ok, detail = res
        else:
            code, detail = res
            ok = code == 0
            if code == 3:
                detail = 'NOT BUILT ' + detail
        ok_all &= ok
        print('%-12s %s %s' % (name, 'PASS' if ok else 'FAIL', '' if ok else detail))
    if a.phase0:
        print('phase0 gate passed: Phase 0 and the attended items may run; the overnight queue may not'
              if ok_all else 'phase0 gate FAILED')
    else:
        print('all checks passed' if ok_all else 'checks FAILED')
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())

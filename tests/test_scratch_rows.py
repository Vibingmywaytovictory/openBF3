import json
import subprocess
import sys

FIXTURE = 'openbf3_scratch_pass'
TAG = 'OPENBF3_SCRATCH_TAG='
PASS_ROWS = {'test.scratch.before', 'test.scratch.after'}


def properties(test):
    return {p['name']: p['value'] for p in test.get('properties', [])}


def main():
    ctest, build_dir = sys.argv[1], sys.argv[2]
    out = subprocess.run([ctest, '--test-dir', build_dir, '--show-only=json-v1'], capture_output=True, text=True)
    if out.returncode != 0:
        print(f'test.scratch.rows: ctest --show-only failed ({out.returncode}): {out.stderr.strip()}')
        return 1
    tests = json.loads(out.stdout)['tests']
    missing = []
    for test in tests:
        props = properties(test)
        name = test['name']
        tags = [e for e in props.get('ENVIRONMENT', []) if e.startswith(TAG)]
        required = props.get('FIXTURES_REQUIRED', [])
        if len(tags) != 1:
            missing.append(f'{name}: no {TAG[:-1]}')
        if name not in PASS_ROWS and FIXTURE not in required:
            missing.append(f'{name}: does not require the {FIXTURE} fixture')
    for line in missing:
        print(f'test.scratch.rows: {line}')
    if missing or len(tests) == 0:
        print(f'test.scratch.rows: FAIL ({len(missing)} of {len(tests)} rows outside the scratch pass)')
        return 1
    print(f'test.scratch.rows: PASS ({len(tests)} rows carry the pass tag and the {FIXTURE} fixture)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

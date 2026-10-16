"""Decision rules W1-W5, S1, S3, M1, M2, T1 (+ reports) over extracted runs (python -I analyze.py <run-dir> ...).

SKELETON: see DESIGN.md section 10.
"""

SECTION = '10'

NOT_BUILT = 3


def main(argv=None):
    print('%s: not built yet (skeleton; DESIGN.md %s)' % (__import__('os').path.basename(__file__), SECTION))
    return NOT_BUILT


if __name__ == '__main__':
    import sys
    sys.exit(main())

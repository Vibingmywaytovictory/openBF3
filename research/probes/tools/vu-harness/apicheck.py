"""Static API / observe-only checker over the mharness mod (python -I apicheck.py [--vudocs <VU-Docs dir>]).

SKELETON: see DESIGN.md section 12.2.
"""

SECTION = '12.2'

NOT_BUILT = 3


def main(argv=None):
    print('%s: not built yet (skeleton; DESIGN.md %s)' % (__import__('os').path.basename(__file__), SECTION))
    return NOT_BUILT


if __name__ == '__main__':
    import sys
    sys.exit(main())

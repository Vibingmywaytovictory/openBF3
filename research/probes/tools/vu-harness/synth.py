"""Synthetic VU logs with known laws (both variants per rule) and malformed lines, for the analysis tests.

SKELETON: see DESIGN.md section 14.
"""

SECTION = '14'

NOT_BUILT = 3


def main(argv=None):
    print('%s: not built yet (skeleton; DESIGN.md %s)' % (__import__('os').path.basename(__file__), SECTION))
    return NOT_BUILT


if __name__ == '__main__':
    import sys
    sys.exit(main())

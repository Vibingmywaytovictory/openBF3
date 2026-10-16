"""Extract MH1 rows from VU server/client logs into research/measurements/<run>/ (python -I extract.py ...).

SKELETON: see DESIGN.md section 9.
"""

SECTION = '9'

NOT_BUILT = 3


def main(argv=None):
    print('%s: not built yet (skeleton; DESIGN.md %s)' % (__import__('os').path.basename(__file__), SECTION))
    return NOT_BUILT


if __name__ == '__main__':
    import sys
    sys.exit(main())

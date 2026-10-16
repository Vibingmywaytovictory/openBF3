"""Run one scenario in the mock VU runtime with chosen laws (python -I mockrun.py WA disc=radius ...).

SKELETON: see DESIGN.md section 13.
"""

SECTION = '13'

NOT_BUILT = 3


def main(argv=None):
    print('%s: not built yet (skeleton; DESIGN.md %s)' % (__import__('os').path.basename(__file__), SECTION))
    return NOT_BUILT


if __name__ == '__main__':
    import sys
    sys.exit(main())

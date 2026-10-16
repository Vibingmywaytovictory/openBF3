"""Result records, SOURCES.md draft rows and the code-change texts of the S5 rules (DESIGN.md section 10.3)."""

import json
import math


def fmt(x, nd=4):
    if x is None:
        return '-'
    if isinstance(x, bool):
        return 'yes' if x else 'no'
    if isinstance(x, int):
        return str(x)
    if isinstance(x, float):
        if not math.isfinite(x):
            return str(x)
        return ('%.' + str(nd) + 'g') % x
    return str(x)


def row(item, quantity, value, unit, run, build, hz, n, statistic, consumer, oracle):
    cells = ['H:' + item, quantity, fmt(value), unit, run or '-', 'VU %s' % (build if build is not None else '?'),
             '%s Hz' % fmt(hz), 'n=%s' % fmt(n), statistic, consumer, oracle]
    return '| ' + ' | '.join(cells) + ' |'


class Result(object):
    """One rule's outcome: verdict (human), variant (machine-readable, compared by the tests), numbers, change."""

    def __init__(self, item, verdict, variant=None, numbers=None, change='no change', source_rows=None, notes=None):
        self.item = item
        self.verdict = verdict
        self.variant = variant
        self.numbers = numbers or {}
        self.change = change
        self.source_rows = source_rows or []
        self.notes = notes or []

    def to_dict(self):
        return {'item': self.item, 'verdict': self.verdict, 'variant': self.variant, 'numbers': self.numbers,
                'change': self.change, 'sources': self.source_rows, 'notes': self.notes}

    def text(self):
        lines = ['%s: %s' % (self.item, self.verdict)]
        if self.numbers:
            lines.append('  numbers: ' + json.dumps(self.numbers, sort_keys=True, default=str)[:1500])
        lines.append('  change: ' + self.change)
        for s in self.source_rows:
            lines.append('  SOURCES draft: ' + s)
        for n in self.notes:
            lines.append('  note: ' + n)
        return '\n'.join(lines)


def no_data(item, why):
    return Result(item, 'no data (%s)' % why, variant=None, change='no change')


# Code locations the verdicts change (DESIGN.md section 10 table).
MECH = 'src/bf3_sim/include/bf3/sim/weapons/mechanics.h'
SOLDIER_DEF = 'src/bf3_core/include/bf3/core/gameplay/soldier_def.h'
MOTOR_CFG = 'bf3-opensource/config/motor_tuning.cfg'

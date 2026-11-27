import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import check_hygiene as h


def cpp(text):
    return h.check_comments(h.lex_comments(text)[1])


def cfg(text):
    return h.check_comments(h.hash_comments(text, True), cfg=True)


def cmake(text):
    return h.check_comments(h.hash_comments(text, False))


def expect(failures, name, found, want):
    got = [m for _, m in found]
    if want is None:
        if got:
            failures.append(f'{name}: expected no finding, got {got}')
    elif not any(want in m for m in got):
        failures.append(f'{name}: expected "{want}", got {got}')


def main():
    failures = []
    ok = [
        ('source', '// SOURCE: SoldierBodyData.Height (EBX), read in soldier_table.cpp\nfloat h = 1.8f;\n'),
        ('note', '\t// SOURCE: notes R-B4 (three 0.5 m height probes)\nconst float k = 0.5f;\n'),
        ('openbf2', '// SOURCE: OpenBF2 Code/BF2/AI/Brain.cpp @c47317b\n'),
        ('inferred', '// INFERRED: Scale is the hit damage. Settle: a note on UIDamageIndicator\nint a;\n'),
        ('stand-in two lines',
         '// STAND-IN: 250 m shadow range, no data field read.\n// Settle: SunShadowmapViewDistance\nint b;\n'),
        ('why', '// WHY: Jolt needs the body added before the constraint\nint c;\n'),
        ('two tags', '// SOURCE: VehicleConfigData.Mass\n// STAND-IN: x 10000 unit. Settle: notes R-G1\nint d;\n'),
        ('url in string', 'const char *u = "https://example.org/a//b";\n'),
        ('path in comment', '// SOURCE: notes followups/R-G1.txt\nint e;\n'),
    ]
    for name, text in ok:
        expect(failures, name, cpp(text), None)
    bad = [
        ('untagged', '// returns the soldier height\nint a;\n', 'without SOURCE'),
        ('restates', '// the loop below adds one\nint a;\n', 'without SOURCE'),
        ('block', '/* SOURCE: x */\nint a;\n', 'block comment'),
        ('trailing', 'int a = 1; // SOURCE: Foo.Bar\n', 'after code'),
        ('todo', '// WHY: TODO read the field\nint a;\n', 'banned marker'),
        ('fixme', '// STAND-IN: FIXME. Settle: x\nint a;\n', 'banned marker'),
        ('no settle', '// STAND-IN: 0.25 bounce\nint a;\n', 'Settle'),
        ('inferred no settle', '// INFERRED: frame is the soldier\n// and so on\nint a;\n', 'Settle'),
        ('three lines', '// WHY: one\n// two\n// three\nint a;\n', 'longer than 2 lines'),
        ('code', '// WHY: x\n// float y = 2.0f;\nint a;\n', 'commented-out code'),
        ('code tagged', '// WHY: return a + b;\nint a;\n', 'commented-out code'),
        ('bare code', '// int y = 2;\nint a;\n', 'without SOURCE'),
        ('disassembly', '// SOURCE: FUN_1401234ab in the client\nint a;\n', 'disassembly'),
        ('ghidra', '// SOURCE: Ghidra listing of the server\nint a;\n', 'disassembly'),
        ('c file', '// SOURCE: soldier_motor.c line 40\nint a;\n', 'disassembly'),
        ('backslash', '// SOURCE: C:\\Users\\olku\\re\\\nint a;\n', 'backslash'),
        ('long', '// WHY: ' + 'x' * 130 + '\nint a;\n', 'longer than 120'),
        ('namespace', '}  // namespace bf3::sim\n', 'after code'),
    ]
    for name, text, want in bad:
        expect(failures, name, cpp(text), want)
    expect(failures, 'cfg ok', cfg('# STAND-IN: Jolt default. Settle: notes R6-G1\ngravity = 9.81\n'), None)
    expect(failures, 'cfg untagged', cfg('# m/s^2 before the modifier\ngravity = 9.81\n'), 'without SOURCE')
    expect(failures, 'cfg commented key', cfg('# WHY: off\n# gravity = 9.81\n'), 'commented-out code')
    expect(failures, 'cfg trailing', cfg('gravity = 9.81 # STAND-IN: x. Settle: y\n'), 'after code')
    expect(failures, 'cmake ok', cmake('# WHY: the row decodes every level once\nadd_test(NAME a COMMAND b)\n'), None)
    expect(failures, 'cmake untagged', cmake('# slow row\nadd_test(NAME a COMMAND b)\n'), 'without SOURCE')
    expect(failures, 'cmake bracket', cmake('#[[ WHY: x ]]\n'), 'block comment')
    for f in failures:
        print(f)
    print(f'comment rule: {len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

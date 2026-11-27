import os
import re
import sys

CODE_EXT = ('.cpp', '.h', '.gdshader', '.gdshaderinc')
CODE_DIRS = ('src', 'tests', 'tools', 'bf3-opensource/shaders')
CMAKE_DIRS = ('src', 'tests', 'tools')
CFG_DIRS = ('bf3-opensource/config',)
MD_ALLOW_DIRS = ('docs/design', 'docs/history', 'research')
MD_ALLOW_FILES = ('README.md', 'CLAUDE.md', 'docs/architecture.md', 'docs/testing.md')
MD_ALLOW_GLOB = re.compile(r'^tools/[^/]+/README\.md$')
ARCHITECTURE_MAX_LINES = 220
IDENT = re.compile(r'[A-Za-z0-9_]')
RAW_PREFIX = ('', 'L', 'u', 'U', 'u8')
TEST_HOOKS = re.compile(r'\b(push_window_mouse|capture_for_check|run_check_and_quit)\b')
TEST_HOOK_DIR = 'src/bf3_godot'
CHECKS_GUARD = re.compile(r'#\s*if\s+OPENBF3_BUILD_CHECKS\b')

COMMENT_TAG = re.compile(r'^(SOURCE|INFERRED|STAND-IN|WHY): \S')
SETTLE_TAGS = ('INFERRED', 'STAND-IN')
SETTLE = re.compile(r'\bSettle: \S')
COMMENT_MAX_LINES = 2
COMMENT_MAX_CHARS = 120
BANNED_MARKERS = re.compile(r'\b(TODO|FIXME|XXX|HACK|NOTE|BUG)\b')
DISASSEMBLY = re.compile(
    r'\b(FUN|DAT|LAB|PTR|SUB|thunk)_[0-9A-Fa-f]{5,}\b|\b0x14[0-9A-Fa-f]{7}\b|decompil|disassembl|ghidra|pseudo-?code'
    r'|\.work\.txt|\b[A-Za-z0-9_]+\.c\b', re.IGNORECASE)
CODE_LIKE = re.compile(
    r'[;{}]\s*$|^\s*(#\s*(include|define|if|endif)\b|return\b|(if|for|while|switch)\s*\(|(const\s+)?'
    r'(auto|int|float|double|bool|uint\d+_t|int\d+_t|size_t)\s+\w+\s*=)')
CFG_ASSIGN = re.compile(r'^\s*[A-Za-z_][A-Za-z0-9_.]*\s*=\s*\S+\s*$')


def lex_comments(text):
    out = []
    comments = []
    i = 0
    n = len(text)
    line = 1
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
            out.append(c)
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            if j < 0:
                j = n
            start = text.rfind('\n', 0, i) + 1
            comments.append((line, '//', text[i + 2:j].rstrip('\r'), text[start:i].strip() == ''))
            i = j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            if j < 0:
                j = n - 2
            comments.append((line, '/*', text[i + 2:j], False))
            line += text.count('\n', i, j + 2)
            out.append(' ')
            i = j + 2
            continue
        if c == '"':
            k = i - 1
            while k >= 0 and IDENT.match(text[k]):
                k -= 1
            prefix = text[k + 1:i]
            if prefix.endswith('R') and prefix[:-1] in RAW_PREFIX:
                p = text.find('(', i)
                delim = text[i + 1:p]
                end = text.find(')' + delim + '"', p)
                end = end + len(delim) + 2
                line += text.count('\n', i, end)
                out.append(text[i:end])
                i = end
                continue
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == '\\':
                    j += 1
                if text[j] == '\n':
                    break
                j += 1
            j += 1
            out.append(text[i:j])
            i = j
            continue
        if c == "'":
            k = i - 1
            while k >= 0 and IDENT.match(text[k]):
                k -= 1
            prefix = text[k + 1:i]
            if prefix and prefix not in ('L', 'u', 'U', 'u8'):
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == '\\':
                    j += 1
                if text[j] == '\n':
                    break
                j += 1
            j += 1
            out.append(text[i:j])
            i = j
            continue
        out.append(c)
        i += 1
    return ''.join(out), comments


def hash_comments(text, inline):
    comments = []
    for i, l in enumerate(text.split('\n')):
        if inline:
            k = l.find('#')
            if k >= 0:
                comments.append((i + 1, '#', l[k + 1:].rstrip('\r'), l[:k].strip() == ''))
        elif l.lstrip().startswith('#'):
            body = l.lstrip()[1:].rstrip('\r')
            comments.append((i + 1, '#[[' if body.startswith('[[') else '#', body, True))
    return comments


def settle_missing(block, found):
    if block is not None and block['tag'] in SETTLE_TAGS and not block['settle']:
        found.append((block['line'], f'{block["tag"]} needs "Settle: <what would settle it>"'))


def check_comments(comments, cfg=False):
    found = []
    block = None
    previous = None
    for line, kind, body, own in comments:
        if kind in ('/*', '#[['):
            settle_missing(block, found)
            block = None
            found.append((line, 'block comment; use one or two own-line comments'))
            continue
        if not own:
            settle_missing(block, found)
            block = None
            found.append((line, 'comment after code; put it on its own line above'))
            continue
        text = body[1:] if body.startswith(' ') else body
        if text.rstrip().endswith('\\'):
            found.append((line, 'comment line ends in a backslash'))
        if len(text) > COMMENT_MAX_CHARS:
            found.append((line, f'comment line longer than {COMMENT_MAX_CHARS} characters'))
        if BANNED_MARKERS.search(text):
            found.append((line, 'banned marker (TODO, FIXME, XXX, HACK, NOTE, BUG)'))
        if DISASSEMBLY.search(text):
            found.append((line, 'disassembly reference; comments cite the notes, never disassembly'))
        tag = COMMENT_TAG.match(text)
        if tag:
            settle_missing(block, found)
            block = {'tag': tag.group(1), 'line': line, 'settle': bool(SETTLE.search(text)), 'lines': 1}
            rest = text[len(tag.group(1)) + 1:]
        elif block is not None and previous == line - 1:
            block['lines'] += 1
            block['settle'] = block['settle'] or bool(SETTLE.search(text))
            rest = text
            if block['lines'] > COMMENT_MAX_LINES:
                found.append((line, f'comment longer than {COMMENT_MAX_LINES} lines'))
        else:
            settle_missing(block, found)
            block = None
            previous = line
            found.append((line, 'comment without SOURCE:, INFERRED:, STAND-IN: or WHY:'))
            continue
        if CODE_LIKE.search(rest) or (cfg and CFG_ASSIGN.match(rest)):
            found.append((line, 'commented-out code'))
        previous = line
    settle_missing(block, found)
    return sorted(found)


def report(failures, root, p, found):
    if found:
        failures.append(f'{os.path.relpath(p, root)}:{found[0][0]}: {found[0][1]} ({len(found)} in file)')


def walk(root, dirs, pred):
    for d in dirs:
        base = os.path.join(root, d)
        for dp, dn, fn in os.walk(base):
            dn[:] = [x for x in dn if x not in ('extern', '.git', 'b')]
            for f in fn:
                p = os.path.join(dp, f)
                if pred(p):
                    yield p


def is_cmake(p):
    return p.endswith('CMakeLists.txt') or p.endswith('.cmake')


def check_only_sources(root):
    out = set()
    depth = 0
    for l in open(os.path.join(root, TEST_HOOK_DIR, 'CMakeLists.txt'), encoding='utf-8').read().split('\n'):
        s = l.strip()
        if s.startswith('if(OPENBF3_BUILD_CHECKS)'):
            depth = 1
        elif depth and s.startswith('if('):
            depth += 1
        elif depth and s.startswith('endif('):
            depth -= 1
        elif depth:
            out.update(f'{TEST_HOOK_DIR}/{t}' for t in s.split() if t.startswith('src/'))
    return out


def ungated_test_hooks(text):
    gates = []
    found = []
    for n, l in enumerate(text.split('\n'), 1):
        s = l.strip()
        if s.startswith('#if'):
            gates.append(bool(CHECKS_GUARD.match(s)))
        elif s.startswith(('#else', '#elif')) and gates:
            gates[-1] = False
        elif s.startswith('#endif') and gates:
            gates.pop()
        elif not any(gates) and TEST_HOOKS.search(l):
            found.append(n)
    return found


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else '.')
    failures = []
    for p in walk(root, CODE_DIRS, lambda x: x.endswith(CODE_EXT)):
        report(failures, root, p, check_comments(lex_comments(open(p, encoding='utf-8').read())[1]))
    for p in list(walk(root, CMAKE_DIRS, is_cmake)) + [os.path.join(root, 'CMakeLists.txt')]:
        report(failures, root, p, check_comments(hash_comments(open(p, encoding='utf-8').read(), False)))
    for p in walk(root, CFG_DIRS, lambda x: x.endswith('.cfg')):
        report(failures, root, p, check_comments(hash_comments(open(p, encoding='utf-8').read(), True), cfg=True))
    for dp, dn, fn in os.walk(root):
        dn[:] = [x for x in dn if x not in ('extern', '.git', 'b', '.godot')]
        for f in fn:
            if not f.endswith(('.md', '.txt', '.rst')) or f in ('CMakeLists.txt', 'VERSION.txt'):
                continue
            rel = os.path.relpath(os.path.join(dp, f), root).replace('\\', '/')
            if rel in MD_ALLOW_FILES or MD_ALLOW_GLOB.match(rel) or any(rel.startswith(d + '/') for d in MD_ALLOW_DIRS):
                continue
            failures.append(f'{rel}: prose file outside the allowed set (README.md, CLAUDE.md, docs/architecture.md, docs/testing.md, tools/<tool>/README.md, docs/design/, docs/history/, research/)')
    check_only = check_only_sources(root)
    for p in walk(root, (TEST_HOOK_DIR,), lambda x: x.endswith(CODE_EXT)):
        rel = os.path.relpath(p, root).replace('\\', '/')
        lines = [] if rel in check_only else ungated_test_hooks(open(p, encoding='utf-8').read())
        if lines:
            failures.append(f'{rel}:{lines[0]}: test hook outside #if OPENBF3_BUILD_CHECKS ({len(lines)} in file)')
    arch = os.path.join(root, 'docs', 'architecture.md')
    if os.path.exists(arch):
        count = len(open(arch, encoding='utf-8').read().split('\n'))
        if count > ARCHITECTURE_MAX_LINES:
            failures.append(f'docs/architecture.md: {count} lines, the cap is {ARCHITECTURE_MAX_LINES}')
    for f in failures:
        print(f)
    print(f'hygiene: {len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

import os
import re
import sys

CODE_EXT = ('.cpp', '.h', '.gdshader', '.gdshaderinc')
CODE_DIRS = ('src', 'tests', 'tools', 'bf3-opensource/shaders')
CMAKE_DIRS = ('src', 'tests', 'tools')
MD_ALLOW_DIRS = ('docs/design', 'docs/history', 'research')
MD_ALLOW_FILES = ('README.md', 'CLAUDE.md', 'docs/architecture.md', 'docs/testing.md')
MD_ALLOW_GLOB = re.compile(r'^tools/[^/]+/README\.md$')
ARCHITECTURE_MAX_LINES = 220
IDENT = re.compile(r'[A-Za-z0-9_]')
RAW_PREFIX = ('', 'L', 'u', 'U', 'u8')


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
            comments.append(line)
            i = j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            if j < 0:
                j = n - 2
            comments.append(line)
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


def tidy(text):
    lines = [l.rstrip() for l in text.split('\n')]
    result = []
    for l in lines:
        if l == '' and result and result[-1] == '':
            continue
        result.append(l)
    while result and result[0] == '':
        result.pop(0)
    while result and result[-1] == '':
        result.pop()
    return '\n'.join(result) + '\n'


def strip_cmake(text):
    kept = []
    for l in text.split('\n'):
        if l.lstrip().startswith('#') and not l.lstrip().startswith('#[['):
            continue
        kept.append(l)
    return tidy('\n'.join(kept))


def cmake_comments(text):
    return [i + 1 for i, l in enumerate(text.split('\n')) if l.lstrip().startswith('#')]


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


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else '.')
    strip = '--strip' in sys.argv
    failures = []
    for p in walk(root, CODE_DIRS, lambda x: x.endswith(CODE_EXT)):
        text = open(p, encoding='utf-8').read()
        stripped, comments = lex_comments(text)
        if comments:
            if strip:
                open(p, 'w', encoding='utf-8', newline='\n').write(tidy(stripped))
            else:
                failures.append(f'{os.path.relpath(p, root)}:{comments[0]}: comment ({len(comments)} in file)')
    for p in walk(root, CMAKE_DIRS, is_cmake):
        text = open(p, encoding='utf-8').read()
        lines = cmake_comments(text)
        if lines:
            if strip:
                open(p, 'w', encoding='utf-8', newline='\n').write(strip_cmake(text))
            else:
                failures.append(f'{os.path.relpath(p, root)}:{lines[0]}: comment ({len(lines)} in file)')
    for p in ('CMakeLists.txt',):
        full = os.path.join(root, p)
        text = open(full, encoding='utf-8').read()
        lines = cmake_comments(text)
        if lines:
            if strip:
                open(full, 'w', encoding='utf-8', newline='\n').write(strip_cmake(text))
            else:
                failures.append(f'{p}:{lines[0]}: comment ({len(lines)} in file)')
    if strip:
        return 0
    for dp, dn, fn in os.walk(root):
        dn[:] = [x for x in dn if x not in ('extern', '.git', 'b', '.godot')]
        for f in fn:
            if not f.endswith(('.md', '.txt', '.rst')) or f in ('CMakeLists.txt', 'VERSION.txt'):
                continue
            rel = os.path.relpath(os.path.join(dp, f), root).replace('\\', '/')
            if rel in MD_ALLOW_FILES or MD_ALLOW_GLOB.match(rel) or any(rel.startswith(d + '/') for d in MD_ALLOW_DIRS):
                continue
            failures.append(f'{rel}: prose file outside the allowed set (README.md, CLAUDE.md, docs/architecture.md, docs/testing.md, tools/<tool>/README.md, docs/design/, docs/history/, research/)')
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

"""A Lua 5.4 lexer for apicheck.py (DESIGN.md 12.2): names, keywords, numerals (decimal, hex, hex floats with
binary exponents), short strings with every escape, long strings and long comments of any level, operators.

Tokens are (kind, value, line) with kind in name | keyword | number | string | op. Comments are dropped; string
values are decoded, so string contents and comments never look like code to the rules.
"""

import re

KEYWORDS = frozenset('and break do else elseif end false for function goto if in local nil not or repeat return then '
                     'true until while'.split())

_OPS = ['...', '..', '==', '~=', '<=', '>=', '<<', '>>', '//', '::', '+', '-', '*', '/', '%', '^', '#', '&', '~', '|',
        '<', '>', '=', '(', ')', '{', '}', '[', ']', ';', ':', ',', '.']

_NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
_HEX = re.compile(r'0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?[0-9]+)?')
_DEC = re.compile(r'(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?')
_LONG_OPEN = re.compile(r'\[(=*)\[')


class LexError(ValueError):
    pass


def _long_bracket(src, i, line):
    m = _LONG_OPEN.match(src, i)
    if not m:
        return None
    close = ']' + m.group(1) + ']'
    j = src.find(close, m.end())
    if j < 0:
        raise LexError('line %d: unfinished long string/comment' % line)
    body = src[m.end():j]
    if body.startswith('\r\n'):
        body = body[2:]
    elif body.startswith('\n') or body.startswith('\r'):
        body = body[1:]
    return body, j + len(close), src.count('\n', i, j + len(close))


def _short_string(src, i, line):
    q = src[i]
    j = i + 1
    out = []
    n = len(src)
    while True:
        if j >= n:
            raise LexError('line %d: unfinished string' % line)
        c = src[j]
        if c == q:
            return ''.join(out), j + 1, line
        if c == '\n':
            raise LexError('line %d: unfinished string' % line)
        if c != '\\':
            out.append(c)
            j += 1
            continue
        j += 1
        if j >= n:
            raise LexError('line %d: unfinished string' % line)
        e = src[j]
        simple = {'a': '\a', 'b': '\b', 'f': '\f', 'n': '\n', 'r': '\r', 't': '\t', 'v': '\v', '\\': '\\', '"': '"',
                  "'": "'"}
        if e in simple:
            out.append(simple[e])
            j += 1
        elif e == '\n':
            out.append('\n')
            line += 1
            j += 1
        elif e == 'x':
            h = src[j + 1:j + 3]
            if not re.fullmatch(r'[0-9a-fA-F]{2}', h):
                raise LexError('line %d: bad \\x escape' % line)
            out.append(chr(int(h, 16)))
            j += 3
        elif e == 'z':
            j += 1
            while j < n and src[j] in ' \t\r\n\f\v':
                if src[j] == '\n':
                    line += 1
                j += 1
        elif e == 'u':
            m = re.match(r'u\{([0-9a-fA-F]+)\}', src[j:])
            if not m:
                raise LexError('line %d: bad \\u escape' % line)
            out.append(chr(min(int(m.group(1), 16), 0x10FFFF)))
            j += m.end()
        elif e.isdigit():
            m = re.match(r'[0-9]{1,3}', src[j:])
            v = int(m.group(0))
            if v > 255:
                raise LexError('line %d: decimal escape too large' % line)
            out.append(chr(v))
            j += m.end()
        else:
            raise LexError('line %d: invalid escape \\%s' % (line, e))


def tokenize(src):
    """Return the token list of a Lua 5.4 chunk; raises LexError on malformed input."""
    toks = []
    i = 0
    n = len(src)
    line = 1
    if src.startswith('#'):
        i = src.find('\n')
        i = n if i < 0 else i
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if c in ' \t\r\f\v':
            i += 1
            continue
        if src.startswith('--', i):
            lb = _long_bracket(src, i + 2, line)
            if lb is not None:
                _body, i, nl = lb
                line += nl
                continue
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if c.isalpha() or c == '_':
            m = _NAME.match(src, i)
            w = m.group(0)
            toks.append(('keyword' if w in KEYWORDS else 'name', w, line))
            i = m.end()
            continue
        if c.isdigit() or (c == '.' and i + 1 < n and src[i + 1].isdigit()):
            m = _HEX.match(src, i) or _DEC.match(src, i)
            if not m:
                raise LexError('line %d: bad number' % line)
            j = m.end()
            if j < n and (src[j].isalnum() or src[j] == '_'):
                raise LexError('line %d: malformed number' % line)
            toks.append(('number', m.group(0), line))
            i = j
            continue
        if c in '"\'':
            s, i, line2 = _short_string(src, i, line)
            toks.append(('string', s, line))
            line = line2
            continue
        if c == '[':
            lb = _long_bracket(src, i, line)
            if lb is not None:
                body, i, nl = lb
                toks.append(('string', body, line))
                line += nl
                continue
        for op in _OPS:
            if src.startswith(op, i):
                toks.append(('op', op, line))
                i += len(op)
                break
        else:
            raise LexError('line %d: unexpected character %r' % (line, c))
    return toks

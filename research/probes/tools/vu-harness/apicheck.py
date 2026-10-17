"""Static API and observe-only checker for the mharness mod (DESIGN.md 12.2).

    python -I apicheck.py [--mod <dir>] [--vudocs <VU-Docs dir>] [--quiet]

Tokens come from py/mhlib/lualex.py, so strings and comments never trigger a rule. Rules (numbers as in DESIGN 12.2):
 1  Events:Subscribe / Hooks:Install names are string literals valid for the file's realm; NetEvents names are 'mh:*'.
 2  Lib:Method( on a VU library exists in the realm.
 3  Enum.Value exists.
 4  members: a '.field' after an engine member must be an engine member; ':Method(' with an upper-case name must be an
    inventory method; reads of personal fields (guid, accountGuid, onlineId, ip, a player's name) are errors.
 5  forbidden symbols, forbidden strings, Lua functions VU removes.
 6  assignments: forbidden_assign fields nowhere; engine fields only in mh_bots.lua and only bot_assign fields.
 7  fixture calls only in mh_bots.lua inside a function that also calls fx(.
 8  print( only in mh_row.lua.
 9  literal row('<e>', {...}): keys in the schema of <e>; prompt fields, mark fields and fx options likewise.
10  require paths are literals that resolve to files of the mod.
11  RCON:SendCommand only with a whitelisted literal and no arguments; admin literals only in mh_cmd.lua/mh_runner.lua
    inside a function that calls fx(.
12  ctx:wait*/coroutine.yield inside a function literal passed as a call argument, except literals passed to
    ctx:config(, coroutine.create/wrap(, pcall(, xpcall( (their bodies run inside the scenario coroutine).
13  :Is('<x>') and EntityManager:GetIterator('<x>') literals are inventory dtype/etype names.
14  --vudocs: every inventory entry re-confirmed against VU-Docs (read-only), confirmed n/N.
15  undefined globals (coarse, file scope): a free name must be a Lua/VU global, an enum type or MH.
Exit 0 with 0 errors.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'py'))

from mhlib import lualex  # noqa: E402

LIBS = ('Events', 'Hooks', 'NetEvents', 'ChatManager', 'PlayerManager', 'EntityManager', 'SharedUtils', 'RCON',
        'TicketManager', 'RaycastManager', 'ResourceManager', 'ClientUtils', 'Console')
PII_FIELDS = ('guid', 'accountGuid', 'onlineId', 'ip')
PLAYERISH = ('pl', 'player', 'inflictor', 'enemy', 'gp', 'owner', 'giver', 'human')
PROMPT_KEYS = {'n', 'of', 'wid', 'zone', 'd', 'tid', 'dir', 'wait'}
YIELDERS = ('wait', 'wait_ticks', 'wait_until')
EXEMPT_CALLEES = {('ctx', ':', 'config'), ('self', ':', 'config'), ('coroutine', '.', 'create'),
                  ('coroutine', '.', 'wrap'), (None, None, 'pcall'), (None, None, 'xpcall')}


def load_json(rel):
    with open(os.path.join(HERE, *rel.split('/')), encoding='utf-8') as f:
        return json.load(f)


class Inventory:
    def __init__(self, inv, schema):
        self.inv = inv
        self.schema = schema
        self.methods = {}      # (lib, method) -> set(realms)
        self.events = {}       # (kind, name) -> set(realms)
        self.enums = {}        # type -> set(values)
        self.members = set()   # every prop name
        self.mnames = set()    # every method name of a type
        self.types = set()
        self.dtypes = set()
        self.etypes = set()
        self.ctors = set()
        for e in inv['entries']:
            k, s, r = e['kind'], e['sym'], e['realm']
            if k == 'method':
                lib, m = s.split(':', 1)
                self.methods.setdefault((lib, m), set()).add(r)
                self.mnames.add(m)
                self.types.add(lib)
            elif k in ('event', 'hook'):
                self.events.setdefault((k, s), set()).add(r)
            elif k == 'enum':
                t, v = s.split('.', 1)
                self.enums.setdefault(t, set()).add(v)
            elif k == 'prop':
                t, f = s.split('.', 1)
                self.members.add(f)
                self.types.add(t)
            elif k == 'dtype':
                self.dtypes.add(s)
            elif k == 'etype':
                self.etypes.add(s)
            elif k == 'ctor':
                self.ctors.add(s)
        self.globals = (set(inv['lua_globals']) | set(inv['vu_globals']) | set(self.enums) | self.ctors | {'MH'})
        self.forbidden_symbols = set(inv['forbidden_symbols'])
        self.forbidden_strings = list(inv['forbidden_strings'])
        self.lua_forbidden = list(inv['lua_forbidden'])
        self.forbidden_assign = set(inv['forbidden_assign']['fields'])
        self.bot_assign = set(inv['bot_assign']['fields'])
        self.fixture_calls = set(inv['fixture_calls'])
        self.rcon_read = set(inv['rcon_read_whitelist']['read'])
        self.rcon_admin = set(inv['rcon_read_whitelist']['admin'])
        self.rows = schema['rows']


def realm_of(rel):
    if rel.startswith('ext/server/'):
        return 'server'
    if rel.startswith('ext/client/'):
        return 'client'
    return 'shared'


def realm_ok(realms, realm):
    if realm == 'shared':
        return 'shared' in realms
    return realm in realms or 'shared' in realms


class FileCheck:
    def __init__(self, rel, src, inv, files):
        self.rel = rel
        self.inv = inv
        self.files = files
        self.realm = realm_of(rel)
        self.base = os.path.basename(rel)
        self.errors = []
        try:
            self.t = lualex.tokenize(src)
        except lualex.LexError as exc:
            self.t = []
            self.err(0, 0, 'lex: %s' % exc)
        self.n = len(self.t)
        self._blocks()

    # helpers ---------------------------------------------------------------------------------------------------
    def err(self, line, rule, msg):
        self.errors.append((self.rel, line, rule, msg))

    def tok(self, i):
        if 0 <= i < self.n:
            return self.t[i]
        return ('eof', '', self.t[-1][2] if self.t else 0)

    def is_op(self, i, v):
        k, val, _ = self.tok(i)
        return k == 'op' and val == v

    def is_kw(self, i, v):
        k, val, _ = self.tok(i)
        return k == 'keyword' and val == v

    def _blocks(self):
        """Function ranges: list of (start, end) token indices; matching of block keywords."""
        self.funcs = []
        stack = []
        for i, (k, v, _ln) in enumerate(self.t):
            if k != 'keyword':
                continue
            if v == 'function':
                stack.append(('func', i))
            elif v in ('while', 'for'):
                stack.append(('hdr', i))
            elif v == 'do':
                if stack and stack[-1][0] == 'hdr':
                    stack[-1] = ('loop', stack[-1][1])
                else:
                    stack.append(('do', i))
            elif v == 'if':
                stack.append(('if', i))
            elif v == 'repeat':
                stack.append(('repeat', i))
            elif v == 'until':
                if stack and stack[-1][0] == 'repeat':
                    stack.pop()
            elif v == 'end':
                if stack:
                    kind, s = stack.pop()
                    if kind == 'func':
                        self.funcs.append((s, i))
        self.funcs.sort()

    def enclosing_func(self, i):
        best = None
        for s, e in self.funcs:
            if s <= i <= e and (best is None or s > best[0]):
                best = (s, e)
        return best

    def func_calls_fx(self, rng):
        if rng is None:
            return False
        s, e = rng
        for j in range(s, e):
            k, v, _ = self.t[j]
            if k == 'name' and v == 'fx' and self.is_op(j + 1, '('):
                return True
        return False

    def call_args(self, i):
        """i is the index of '('; return list of (start, end) token ranges of the top-level arguments and the index of
        the matching ')'."""
        depth = 0
        args = []
        start = i + 1
        j = i
        while j < self.n:
            k, v, _ = self.t[j]
            if k == 'op' and v in '([{':
                depth += 1
            elif k == 'op' and v in ')]}':
                depth -= 1
                if depth == 0:
                    if j > start:
                        args.append((start, j))
                    return args, j
            elif k == 'keyword' and v == 'function':
                # skip the whole function literal
                for s, e in self.funcs:
                    if s == j:
                        j = e
                        break
            elif k == 'op' and v == ',' and depth == 1:
                args.append((start, j))
                start = j + 1
            j += 1
        return args, self.n - 1

    def ctor_keys(self, s, e):
        """Top-level keys of a table constructor whose '{' is at s."""
        keys = []
        depth = 0
        j = s
        while j < e:
            k, v, ln = self.t[j]
            if k == 'op' and v in '([{':
                depth += 1
            elif k == 'op' and v in ')]}':
                depth -= 1
            elif k == 'keyword' and v == 'function':
                for fs, fe in self.funcs:
                    if fs == j:
                        j = fe
                        break
            elif depth == 1 and k == 'name' and self.is_op(j + 1, '=') and not self.is_op(j + 2, '='):
                if self.tok(j - 1)[1] in ('{', ','):
                    keys.append((v, ln))
            j += 1
        return keys

    # rules -----------------------------------------------------------------------------------------------------
    def run(self):
        if not self.t:
            return self.errors
        self._declared_names()
        for i in range(self.n):
            k, v, ln = self.t[i]
            if k == 'name':
                self.r_name(i, v, ln)
            elif k == 'string':
                for fs in self.inv.forbidden_strings:
                    if fs in v:
                        self.err(ln, 5, 'forbidden string %r' % fs)
            elif k == 'op' and v == '=' and not self.is_op(i + 1, '=') and self.tok(i - 1)[1] not in ('=', '~', '<',
                                                                                                    '>'):
                self.r_assign(i)
            elif k == 'keyword' and v == 'function':
                self.r_callback(i)
        return self.errors

    def _declared_names(self):
        d = set()
        t = self.t
        for i, (k, v, _ln) in enumerate(t):
            if k != 'keyword':
                continue
            if v == 'local':
                j = i + 1
                if self.is_kw(j, 'function'):
                    j += 1
                    if self.tok(j)[0] == 'name':
                        d.add(self.tok(j)[1])
                    continue
                while self.tok(j)[0] == 'name':
                    d.add(self.tok(j)[1])
                    j += 1
                    if self.is_op(j, '<'):
                        j += 3
                    if self.is_op(j, ','):
                        j += 1
                    else:
                        break
            elif v == 'for':
                j = i + 1
                while self.tok(j)[0] == 'name':
                    d.add(self.tok(j)[1])
                    j += 1
                    if self.is_op(j, ','):
                        j += 1
                    else:
                        break
            elif v == 'function':
                j = i + 1
                while not self.is_op(j, '(') and j < self.n:
                    j += 1
                j += 1
                while j < self.n and not self.is_op(j, ')'):
                    if self.tok(j)[0] == 'name':
                        d.add(self.tok(j)[1])
                    j += 1
        self.declared = d

    def r_name(self, i, v, ln):
        inv = self.inv
        prev = self.tok(i - 1)
        nxt = self.tok(i + 1)
        after_dot = prev[0] == 'op' and prev[1] in ('.', ':')
        # rule 5: forbidden symbols (any name position), removed Lua functions
        if v in inv.forbidden_symbols:
            self.err(ln, 5, 'forbidden symbol %s' % v)
        if not after_dot:
            for lf in inv.lua_forbidden:
                if '.' in lf:
                    a, b = lf.split('.', 1)
                    if v == a and self.is_op(i + 1, '.') and self.tok(i + 2)[1] == b:
                        self.err(ln, 5, 'Lua function removed in VU: %s' % lf)
                elif v == lf and v not in self.declared:
                    self.err(ln, 5, 'Lua function removed in VU: %s' % lf)
        # rule 8
        if v == 'print' and not after_dot and self.rel != 'ext/shared/mh_row.lua':
            self.err(ln, 8, 'print outside mh_row.lua')
        # rule 15: free names
        if not after_dot and v not in self.declared and v not in inv.globals and v != 'self':
            is_key = self.is_op(i + 1, '=') and not self.is_op(i + 2, '=') and prev[1] in ('{', ',') and \
                self._in_braces(i)
            if not is_key and prev[1] != 'goto' and not (prev[1] == '::'):
                self.err(ln, 15, 'undefined global %s' % v)
        # library calls (rules 1, 2, 10, 11, 13)
        if v in LIBS and not after_dot and self.is_op(i + 1, ':') and self.tok(i + 2)[0] == 'name':
            m = self.tok(i + 2)[1]
            realms = inv.methods.get((v, m))
            if realms is None or not realm_ok(realms, self.realm):
                self.err(ln, 2, '%s:%s not in the inventory for realm %s' % (v, m, self.realm))
            if self.is_op(i + 3, '('):
                self.r_libcall(i, v, m, i + 3, ln)
        # rule 3: enums
        if v in inv.enums and not after_dot and self.is_op(i + 1, '.') and self.tok(i + 2)[0] == 'name':
            val = self.tok(i + 2)[1]
            if val not in inv.enums[v]:
                self.err(ln, 3, 'unknown enum value %s.%s' % (v, val))
        # rule 4: member chains, upper-case methods, personal fields
        if after_dot and prev[1] == '.':
            base = self.tok(i - 2)
            if v in PII_FIELDS:
                self.err(ln, 4, 'personal field .%s' % v)
            if v == 'name' and base[0] == 'name' and base[1] in PLAYERISH:
                self.err(ln, 4, 'personal field %s.name' % base[1])
            if base[0] == 'name' and self.tok(i - 3)[1] == '.' and base[1] in inv.members and \
                    v not in inv.members and base[1] not in ('x', 'y', 'z'):
                self.err(ln, 4, 'member .%s after engine member .%s is not in the inventory' % (v, base[1]))
        if after_dot and prev[1] == ':' and self.is_op(i + 1, '(') and v[:1].isupper():
            base = self.tok(i - 2)
            if not (base[0] == 'name' and base[1] in LIBS) and v not in inv.mnames:
                self.err(ln, 4, 'method :%s not in the inventory' % v)
            # rule 7: fixtures
            if v in inv.fixture_calls:
                if self.rel != 'ext/server/mh_bots.lua':
                    self.err(ln, 7, 'fixture call :%s outside mh_bots.lua' % v)
                elif not self.func_calls_fx(self.enclosing_func(i)):
                    self.err(ln, 7, 'fixture call :%s in a function without fx(' % v)
            if v == 'Is' and self.is_op(i + 1, '(') and self.tok(i + 2)[0] == 'string':
                s = self.tok(i + 2)[1]
                if s not in inv.dtypes and s not in inv.etypes:
                    self.err(ln, 13, ':Is(%r) is not an inventory dtype/etype' % s)
        # rule 9: literal rows and fields
        if v == 'row' and self.is_op(i + 1, '(') and self.tok(i + 2)[0] == 'string':
            e = self.tok(i + 2)[1]
            spec = inv.rows.get(e)
            if spec is None:
                self.err(ln, 9, 'row type %r not in the schema' % e)
            else:
                args, _end = self.call_args(i + 1)
                if len(args) >= 2 and self.is_op(args[1][0], '{'):
                    allowed = set(spec['req']) | set(spec['opt'])
                    for key, kl in self.ctor_keys(args[1][0], args[1][1]):
                        if key not in allowed:
                            self.err(kl, 9, 'row %s: key %s not in the schema' % (e, key))
        if v in ('prompt', 'mark', 'fx') and after_dot and self.is_op(i + 1, '('):
            args, _end = self.call_args(i + 1)
            for a in args:
                if self.is_op(a[0], '{') and self._ctor_ends(a):
                    if v == 'prompt':
                        allowed = PROMPT_KEYS
                    elif v == 'mark':
                        allowed = set(inv.rows['mark']['req']) | set(inv.rows['mark']['opt'])
                    else:
                        allowed = set(inv.rows['fx']['opt'])
                    for key, kl in self.ctor_keys(a[0], a[1]):
                        if key not in allowed:
                            self.err(kl, 9, '%s: field %s not in the schema' % (v, key))
        # rule 10: require
        if v == 'require' and not after_dot:
            if not self.is_op(i + 1, '(') or self.tok(i + 2)[0] != 'string' or not self.is_op(i + 3, ')'):
                if not (prev[1] == ',' or prev[1] == '('):  # pcall(require, '...')
                    self.err(ln, 10, 'require needs one string literal')
                elif self.tok(i + 2)[0] == 'string':
                    self._require_path(self.tok(i + 2)[1], ln, optional=True)
            else:
                self._require_path(self.tok(i + 2)[1], ln)
        # rule 12 helpers handled in r_callback

    def _ctor_ends(self, a):
        s, e = a
        depth = 0
        for j in range(s, e):
            k, v, _ = self.t[j]
            if k == 'op' and v in '([{':
                depth += 1
            elif k == 'op' and v in ')]}':
                depth -= 1
                if depth == 0:
                    return j == e - 1
        return False

    def _in_braces(self, i):
        depth = 0
        for j in range(i - 1, -1, -1):
            k, v, _ = self.t[j]
            if k == 'op' and v in ')]}':
                depth += 1
            elif k == 'op' and v in '([{':
                if depth == 0:
                    return v == '{'
                depth -= 1
        return False

    def _require_path(self, path, ln, optional=False):
        p = path[:-4] if path.endswith('.lua') else path
        if '.' in p:
            self.err(ln, 10, 'require of a non-.lua path %r' % path)
            return
        if p.startswith('__shared/'):
            key = 'shared/' + p[9:]
        else:
            key = ('shared' if self.realm == 'shared' else self.realm) + '/' + p
        if key not in self.files and not (optional and p == '__shared/mh_cert'):
            self.err(ln, 10, 'require(%r) does not resolve to a file of the mod' % path)

    def r_libcall(self, i, lib, m, lp, ln):
        inv = self.inv
        args, _end = self.call_args(lp)
        first = self.tok(args[0][0]) if args else ('none', '', ln)
        lit = first[0] == 'string' and args and args[0][1] - args[0][0] == 1
        if (lib, m) in (('Events', 'Subscribe'), ('Hooks', 'Install')):
            kind = 'event' if lib == 'Events' else 'hook'
            if not lit:
                self.err(ln, 1, '%s:%s needs a literal name' % (lib, m))
            else:
                realms = inv.events.get((kind, first[1]))
                if realms is None or not realm_ok(realms, self.realm):
                    self.err(ln, 1, '%s %r not in the inventory for realm %s' % (kind, first[1], self.realm))
        if lib == 'NetEvents' and m in ('Subscribe', 'SendTo', 'Send', 'Broadcast'):
            if not lit or not first[1].startswith('mh:'):
                self.err(ln, 1, 'NetEvents name must be a literal mh:*')
        if (lib, m) == ('RCON', 'SendCommand'):
            if not lit or len(args) != 1:
                self.err(ln, 11, 'RCON:SendCommand needs exactly one literal command')
            elif first[1] in inv.rcon_admin:
                if self.base not in ('mh_cmd.lua', 'mh_runner.lua') or not self.func_calls_fx(self.enclosing_func(i)):
                    self.err(ln, 11, 'admin command %r outside mh_cmd/mh_runner or without fx(' % first[1])
            elif first[1] not in inv.rcon_read:
                self.err(ln, 11, 'RCON command %r not whitelisted' % first[1])
        if (lib, m) == ('EntityManager', 'GetIterator') and lit and first[1] not in inv.etypes:
            self.err(ln, 13, 'GetIterator(%r) is not an inventory etype' % first[1])

    def r_assign(self, i):
        """Check the lvalue list before the '=' at i (statement assignments; table constructor keys are skipped)."""
        inv = self.inv
        j = i - 1
        first = True
        while j >= 0:
            fields = []
            end = j
            # walk one lvalue backwards: (name | ] ... [) ('.' name | '[' .. ']')*
            while j >= 0:
                k, v, _ = self.t[j]
                if k == 'name' and self.is_op(j - 1, '.'):
                    fields.append((v, self.t[j][2]))
                    j -= 2
                    continue
                if k == 'op' and v == ']':
                    depth = 0
                    while j >= 0:
                        kk, vv, _ = self.t[j]
                        if kk == 'op' and vv == ']':
                            depth += 1
                        elif kk == 'op' and vv == '[':
                            depth -= 1
                            if depth == 0:
                                break
                        j -= 1
                    j -= 1
                    fields.append(('[]', 0))
                    continue
                if k == 'op' and v == ')':
                    return  # f(x).y = ... : a call result; not an lvalue we track
                break
            if j < 0:
                return
            k, v, _ = self.t[j]
            if k != 'name' or end == j:
                return
            if first and not fields:
                return  # plain name (local/global assignment or a constructor key)
            if fields and fields[0][0] != '[]':
                fname, fl = fields[0]
                self._check_assign_field(fname, fl)
            first = False
            j -= 1
            if not self.is_op(j, ','):
                return
            j -= 1

    def _check_assign_field(self, fname, ln):
        inv = self.inv
        if fname in inv.forbidden_assign:
            self.err(ln, 6, 'assignment of forbidden field .%s' % fname)
        elif fname in inv.members:
            if self.rel != 'ext/server/mh_bots.lua':
                self.err(ln, 6, 'assignment of engine field .%s outside mh_bots.lua' % fname)
            elif fname not in inv.bot_assign:
                self.err(ln, 6, 'mh_bots.lua assigns engine field .%s (not in bot_assign)' % fname)

    def r_callback(self, i):
        """Rule 12: a function literal passed as a call argument must not yield (unless the callee is exempt)."""
        prev = self.tok(i - 1)
        if not (prev[0] == 'op' and prev[1] in ('(', ',')):
            return
        # find the call paren
        depth = 0
        j = i - 1
        while j >= 0:
            k, v, _ = self.t[j]
            if k == 'op' and v in ')]}':
                depth += 1
            elif k == 'op' and v in '([{':
                if depth == 0:
                    break
                depth -= 1
            j -= 1
        if j < 0 or not self.is_op(j, '('):
            return
        callee = self.tok(j - 1)
        sep = self.tok(j - 2)
        base = self.tok(j - 3)
        key = (base[1], sep[1], callee[1]) if sep[0] == 'op' and sep[1] in ('.', ':') else (None, None, callee[1])
        if key in EXEMPT_CALLEES:
            return
        if callee[0] != 'name':
            return
        rng = None
        for s, e in self.funcs:
            if s == i:
                rng = (s, e)
                break
        if rng is None:
            return
        for q in range(rng[0], rng[1]):
            k, v, ln = self.t[q]
            if k == 'name' and v in YIELDERS and self.is_op(q - 1, ':') and self.is_op(q + 1, '('):
                self.err(ln, 12, 'ctx:%s inside a callback passed to %s(' % (v, callee[1]))
            if k == 'name' and v == 'yield' and self.is_op(q - 1, '.') and self.tok(q - 2)[1] == 'coroutine':
                self.err(ln, 12, 'coroutine.yield inside a callback passed to %s(' % callee[1])


def collect(mod_dir):
    ext = os.path.join(mod_dir, 'ext')
    out = {}
    for dirpath, _d, files in os.walk(ext):
        for fn in sorted(files):
            if fn.endswith('.lua'):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, mod_dir).replace(os.sep, '/')
                with open(full, encoding='utf-8') as f:
                    out[rel] = f.read()
    return out


def check_sources(sources, inv=None):
    """sources: {'ext/server/x.lua': src}. Returns a list of (file, line, rule, message)."""
    if inv is None:
        inv = Inventory(load_json('schema/api_inventory.json'), load_json('schema/rows_v1.json'))
    keys = set()
    for rel in sources:
        if rel.startswith('ext/') and rel.endswith('.lua'):
            keys.add(rel[4:-4])
    errors = []
    for rel in sorted(sources):
        errors.extend(FileCheck(rel, sources[rel], inv, keys).run())
    return errors


def check_vudocs(root):
    from mhlib import vudocs
    inv = load_json('schema/api_inventory.json')
    if not os.path.isdir(os.path.join(root, 'content', 'vext', 'ref')):
        return None, 'VU-Docs not readable: inventory facts unconfirmed this run'
    n_ok, n, failures, ro = vudocs.confirm_all(inv['entries'], root)
    bad = list(failures)
    for e in inv['entries']:
        if e['kind'] == 'prop' and e.get('ro') is not None:
            r = vudocs.confirm(e, root)
            if r['found'] and r['ro'] is not None and r['ro'] != e['ro']:
                bad.append((e['sym'], e['doc'], 'read-only flag differs'))
    return (n_ok, n, bad), None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--mod', default=os.path.join(HERE, 'mod', 'mharness'))
    ap.add_argument('--vudocs', default=None)
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)
    sources = collect(a.mod)
    errors = check_sources(sources)
    for rel, ln, rule, msg in errors:
        print('ERROR %s:%d rule %d: %s' % (rel, ln, rule, msg))
    fail = bool(errors)
    if a.vudocs:
        res, why = check_vudocs(a.vudocs)
        if res is None:
            print(why)
        else:
            n_ok, n, bad = res
            print('vudocs: confirmed %d/%d' % (n_ok, n))
            for sym, doc, reason in bad:
                print('  VUDOCS %s (%s): %s' % (sym, doc, reason))
            fail = fail or bool(bad)
    print('apicheck: %d files, %d errors%s' % (len(sources), len(errors), '' if not fail else ' FAIL'))
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())

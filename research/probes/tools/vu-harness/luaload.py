"""Load every mharness Lua file in a real Lua 5.4 runtime (lupa.lua54) inside a VU-like sandbox (DESIGN.md 12.1).

    python -I luaload.py [--mod <dir>] [--quiet]

VeniceEXT runs Lua 5.4 (VU-Docs content/vext/_index.md, "Lua engine"). Per realm (server, client) this builds an
environment with only the libraries VU documents as available (table string math os coroutine package utf8 plus the
base functions, minus dofile/load/loadfile/string.dump and the os/package members VU removes), VU's `require`
(realm-relative paths, the `__shared/` prefix, optional `.lua`, each file executed once) and strict globals (reading an
undefined global or assigning any global except `MH` is an error with file:line). VU globals are permissive recorder
stubs here (enum values from schema/api_inventory.json), so the check covers syntax and top-level code only; the
behaviour is exercised by the mock VU runtime (py/mockvu, DESIGN.md section 13).

Exit 0 when every file compiles and both entry orders (shared first, realm first) load without error in both realms,
and every Events/Hooks/NetEvents name used at load time is valid for its realm.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Lua host code: runs outside the sandbox and keeps the real libraries; the mod only ever sees `env`.
_HOST = r"""
local sources, realm, inv_enums, vu_globals, order = ...
local loaded = {}
local errors = {}
local records = { events = {}, hooks = {}, netevents = {} }

local function permissive(name)
  local p
  local mt = {}
  mt.__index = function(_, k) return permissive(name .. '.' .. tostring(k)) end
  mt.__newindex = function() end
  mt.__call = function(_, ...)
    local a1, a2 = ...
    if name == 'Events:Subscribe' or name == 'Events.Subscribe' then records.events[#records.events + 1] = a2 end
    return permissive(name .. '()')
  end
  mt.__tostring = function() return 'stub:' .. name end
  mt.__len = function() return 0 end
  mt.__concat = function(a, b) return tostring(a) .. tostring(b) end
  for _, op in ipairs({ '__add', '__sub', '__mul', '__div', '__unm' }) do
    mt[op] = function() return permissive(name .. op) end
  end
  for _, op in ipairs({ '__eq', '__lt', '__le' }) do
    mt[op] = function() return false end
  end
  p = setmetatable({}, mt)
  return p
end

-- Library objects: method-call stubs that record names (obj:Method(name, ...)).
local function library(libname)
  return setmetatable({}, {
    __index = function(_, method)
      return function(self, a1, ...)
        if libname == 'Events' and method == 'Subscribe' then records.events[#records.events + 1] = a1 end
        if libname == 'Hooks' and method == 'Install' then records.hooks[#records.hooks + 1] = a1 end
        if libname == 'NetEvents' and method == 'Subscribe' then records.netevents[#records.netevents + 1] = a1 end
        return permissive(libname .. ':' .. method .. '()')
      end
    end,
    __newindex = function() error('assignment into VU library ' .. libname, 2) end,
  })
end

local LIBS = { Events = 1, Hooks = 1, NetEvents = 1, ChatManager = 1, PlayerManager = 1, EntityManager = 1,
  SharedUtils = 1, RCON = 1, TicketManager = 1, RaycastManager = 1, ResourceManager = 1, ClientUtils = 1, Console = 1 }

local function copy(t, drop)
  local r = {}
  for k, v in pairs(t) do if not (drop and drop[k]) then r[k] = v end end
  return r
end

local env = {}
local base = { 'assert', 'error', 'ipairs', 'next', 'pairs', 'pcall', 'print', 'rawequal', 'rawget', 'rawlen',
  'rawset', 'select', 'setmetatable', 'getmetatable', 'tonumber', 'tostring', 'type', 'xpcall' }
for _, n in ipairs(base) do env[n] = _G[n] end
env.print = function() end
env._VERSION = _VERSION
env.table = copy(table)
env.string = copy(string, { dump = true })
env.string.starts = function(a, b) return a:sub(1, #b) == b end
env.string.ends = function(a, b) return b == '' or a:sub(-#b) == b end
env.math = copy(math)
env.os = copy(os, { execute = true, exit = true, getenv = true, remove = true, rename = true, setlocale = true,
  tmpname = true })
env.coroutine = copy(coroutine)
env.utf8 = copy(utf8)
env.package = { loaded = {} }
env.json = { encode = function() return '{}' end, decode = function() return {} end }
for name, members in pairs(inv_enums) do
  env[name] = setmetatable(copy(members), { __index = function(_, k) error('unknown enum value ' .. name .. '.' .. k, 2) end })
end
for _, g in ipairs(vu_globals) do
  if env[g] == nil then env[g] = LIBS[g] and library(g) or permissive(g) end
end
env._G = env

local function resolve(path)
  if path:sub(-4) == '.lua' then path = path:sub(1, -5) end
  if path:find('%.') then return nil, 'require of a non-.lua path: ' .. path end
  if path:sub(1, 9) == '__shared/' then return 'shared/' .. path:sub(10) end
  return realm .. '/' .. path
end

env.require = function(path)
  if type(path) ~= 'string' then error('require needs a string', 2) end
  local key, why = resolve(path)
  if not key then error(why, 2) end
  if loaded[key] ~= nil then return loaded[key] end
  local src = sources[key]
  if not src then error('require: no file ext/' .. key .. '.lua', 2) end
  local chunk, err = load(src, '@ext/' .. key .. '.lua', 't', env)
  if not chunk then error(err, 0) end
  loaded[key] = true
  local r = chunk()
  if r ~= nil then loaded[key] = r end
  return loaded[key]
end

setmetatable(env, {
  __index = function(_, k)
    if k == 'MH' then return nil end
    error("undefined global '" .. tostring(k) .. "'", 2)
  end,
  __newindex = function(t, k, v)
    if k == 'MH' then rawset(t, k, v) else error("assignment to global '" .. tostring(k) .. "'", 2) end
  end,
})

-- 1. compile every file of the realm and of shared
for key, src in pairs(sources) do
  local chunk, err = load(src, '@ext/' .. key .. '.lua', 't', env)
  if not chunk then errors[#errors + 1] = err end
end
-- 2. run the two entry points in the given order
if #errors == 0 then
  for _, entry in ipairs(order) do
    local ok, err = pcall(env.require, entry)
    if not ok then errors[#errors + 1] = 'entry ' .. entry .. ': ' .. tostring(err) end
  end
end
-- 3. require every remaining file on its own (lazily loaded scenario modules)
do
  for key in pairs(sources) do
    if loaded[key] == nil and (key:sub(1, #realm + 1) == realm .. '/' or key:sub(1, 7) == 'shared/') then
      local path = key:sub(1, 7) == 'shared/' and ('__shared/' .. key:sub(8)) or key:sub(#realm + 2)
      local ok, err = pcall(env.require, path)
      if not ok then errors[#errors + 1] = 'file ' .. key .. ': ' .. tostring(err) end
    end
  end
end
return errors, records
"""


def _collect(mod_dir):
    """Return {'shared/x': src, 'server/y': src, ...} for every .lua under mod/ext."""
    ext = os.path.join(mod_dir, 'ext')
    out = {}
    for realm in ('shared', 'server', 'client'):
        base = os.path.join(ext, realm)
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, ext).replace(os.sep, '/')
                if not fn.endswith('.lua'):
                    raise SystemExit('not a .lua file in ext: ' + rel)
                with open(full, 'rb') as f:
                    data = f.read()
                if len(data) > 1024 * 1024:
                    raise SystemExit('file too large: ' + rel)
                out[rel[:-4]] = data.decode('utf-8')
    return out


def _inventory():
    with open(os.path.join(HERE, 'schema', 'api_inventory.json'), encoding='utf-8') as f:
        return json.load(f)


def check(mod_dir, quiet=False):
    import lupa.lua54 as lua54
    inv = _inventory()
    sources = _collect(mod_dir)
    enums = {}
    for e in inv['entries']:
        if e['kind'] == 'enum':
            t, v = e['sym'].split('.', 1)
            enums.setdefault(t, {})[v] = e['value']
    names_by_realm = {}
    for e in inv['entries']:
        if e['kind'] in ('event', 'hook'):
            names_by_realm.setdefault((e['kind'], e['realm']), set()).add(e['sym'])
    problems = []
    for realm in ('server', 'client'):
        for order in (['__shared/__init__', '__init__'], ['__init__', '__shared/__init__']):
            rt = lua54.LuaRuntime(register_eval=False, register_builtins=False, unpack_returned_tuples=True)
            if rt.eval('_VERSION') != 'Lua 5.4':
                raise SystemExit('lupa.lua54 is not Lua 5.4')
            realm_sources = {k: v for k, v in sources.items() if k.startswith(('shared/', realm + '/'))}
            host = rt.execute(_HOST.replace('local sources, realm, inv_enums, vu_globals, order = ...',
                                            'return function(sources, realm, inv_enums, vu_globals, order)') + '\nend')
            lua_sources = rt.table_from(realm_sources)
            lua_enums = rt.table_from({k: rt.table_from(v) for k, v in enums.items()})
            errs, rec = host(lua_sources, realm, lua_enums, rt.table_from(inv['vu_globals']), rt.table_from(order))
            for i in range(1, len(errs) + 1):
                problems.append('%s %s: %s' % (realm, '>'.join(order), errs[i]))
            for kind, key in (('event', 'events'), ('hook', 'hooks')):
                allowed = names_by_realm.get((kind, realm), set()) | names_by_realm.get((kind, 'shared'), set())
                for i in range(1, len(rec[key]) + 1):
                    name = rec[key][i]
                    if name not in allowed:
                        problems.append('%s: %s %r not in the inventory for this realm' % (realm, kind, name))
            for i in range(1, len(rec['netevents']) + 1):
                if not str(rec['netevents'][i]).startswith('mh:'):
                    problems.append('%s: NetEvent %r is not ours (mh:*)' % (realm, rec['netevents'][i]))
    if not quiet:
        print('luaload: %d files, Lua 5.4 (lupa %s), realms server+client, both entry orders'
              % (len(sources), __import__('lupa').__version__))
        for p in problems:
            print('  ERROR ' + p)
        print('luaload: %s' % ('PASS' if not problems else 'FAIL (%d)' % len(problems)))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--mod', default=os.path.join(HERE, 'mod', 'mharness'))
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)
    return 1 if check(a.mod, a.quiet) else 0


if __name__ == '__main__':
    sys.exit(main())

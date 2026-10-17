"""luaload.py on the mod (0 errors) and on planted errors in a scratch copy (each reported). DESIGN.md 12.1."""

import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import luaload  # noqa: E402

MOD = os.path.join(HERE, 'mod', 'mharness')


def _plant(root, rel, line):
    p = os.path.join(root, 'ext', *rel.split('/'))
    with open(p, encoding='utf-8') as f:
        s = f.read()
    assert '\nreturn M' in s, rel
    s = s.replace('\nreturn M', '\n' + line + '\nreturn M', 1)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(s)


class LuaLoad(unittest.TestCase):
    def test_mod_loads(self):
        self.assertEqual(luaload.check(MOD, quiet=True), [])

    def _bad(self, rel, line, expect):
        with tempfile.TemporaryDirectory() as tmp:
            dst = os.path.join(tmp, 'mharness')
            shutil.copytree(MOD, dst)
            _plant(dst, rel, line)
            problems = luaload.check(dst, quiet=True)
        self.assertTrue(any(expect in p for p in problems), (expect, problems))

    def test_io_is_undefined(self):
        self._bad('server/mh_ids.lua', 'local x = io.open', "undefined global 'io'")

    def test_load_is_undefined(self):
        self._bad('shared/mh_vec.lua', 'local f = load', "undefined global 'load'")

    def test_global_assignment(self):
        self._bad('client/mh_fov.lua', 'foo = 1', "assignment to global 'foo'")

    def test_unknown_enum_value(self):
        self._bad('shared/mh_vec.lua', 'local y = UpdatePass.UpdatePass_Bogus', 'unknown enum value')

    def test_event_wrong_realm(self):
        self._bad('client/mh_fov.lua', "Events:Subscribe('Player:Chat', function() end)", "event 'Player:Chat'")

    def test_missing_require(self):
        self._bad('server/scenarios/wc.lua', "local z = require('nosuch')", 'no file ext/server/nosuch.lua')

    def test_syntax_error(self):
        self._bad('server/mh_cmd.lua', 'local function f( end', 'mh_cmd.lua:')


if __name__ == '__main__':
    unittest.main()

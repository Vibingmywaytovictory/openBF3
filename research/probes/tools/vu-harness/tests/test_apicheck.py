"""apicheck.py rules (DESIGN.md 12.2): the whole mod passes; each rule has a negative snippet that it reports and a
positive twin that it accepts. Snippets are tiny synthetic files, checked with apicheck.check_sources."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'py'))

import apicheck  # noqa: E402

SRV = 'ext/server/t_snippet.lua'

# rule -> (negative source, positive source); each runs as its own server file
CASES = {
    1: ("Events:Subscribe('Bogus:NoSuchEvent', function() end)\n",
        "Events:Subscribe('Level:Loaded', function() end)\n"),
    2: ("local n = PlayerManager:NoSuchMethodXyz()\n",
        "local n = PlayerManager:GetPlayerCount()\n"),
    3: ("local a = EntryInputActionEnum.EIANoSuchAction\n",
        "local a = EntryInputActionEnum.EIAFire\n"),
    4: ("local function f(player) return player.guid end\nreturn f\n",
        "local function f(player) return player.teamId end\nreturn f\n"),
    5: ("local function f(i) i:MakeWritable() end\nreturn f\n",
        "local function f(i) return i end\nreturn f\n"),
    6: ("local function f(player) player.soldier.health = 1 end\nreturn f\n",
        "local t = {}\nt.count = 1\n"),
    9: ("local R = require('__shared/mh_row')\nR.row('dmg', { bogus_key_xyz = 1 })\n",
        "local R = require('__shared/mh_row')\nR.row('dmg', { victim = 1 })\n"),
    12: ("local function g(ctx) pcall(print, 1) table.sort({}, function() ctx:wait(1) end) end\nreturn g\n",
         "local function g(ctx) ctx:config(1, 1, 'x', nil, function() ctx:wait(1) end) end\nreturn g\n"),
    7: ("local p = PlayerManager:CreatePlayer('b', 1, 1)\n",
        "local p = PlayerManager:GetPlayerById(1)\n"),
    8: ("print('x')\n",
        "local s = tostring(1)\n"),
    10: ("local m = require('no_such_module_xyz')\n",
         "local m = require('__shared/mh_row')\n"),
    11: ("local r = RCON:SendCommand('admin.killPlayer')\n",
         "local r = 1\n"),
    13: ("local it = EntityManager:GetIterator('NoSuchEntityTypeXyz')\n",
         "local it = nil\n"),
    15: ("local y = zzz_undefined_global_xyz + 1\n",
         "local zzz = 1\nlocal y = zzz + 1\n"),
}


def rules_of(src, rel=SRV):
    srcs = {rel: src, 'ext/shared/mh_row.lua': 'return {}\n'}  # a require target for rule 10
    return sorted(set(e[2] for e in apicheck.check_sources(srcs) if e[0] == rel))


class TestApicheck(unittest.TestCase):
    def test_whole_mod_clean(self):
        errs = apicheck.check_sources(apicheck.collect(os.path.join(HERE, 'mod', 'mharness')))
        self.assertEqual(errs, [], errs[:5])

    def test_negative_and_positive(self):
        for rule, (bad, good) in sorted(CASES.items()):
            with self.subTest(rule=rule):
                self.assertIn(rule, rules_of(bad), 'rule %d not reported for %r' % (rule, bad))
                self.assertNotIn(rule, rules_of(good), 'rule %d reported for the positive %r' % (rule, good))


if __name__ == '__main__':
    unittest.main()

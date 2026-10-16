-- mharness server entry (SKELETON; DESIGN.md section 3). Wires events, hooks, NetEvents and commands.
-- Events:Subscribe (shared/library/events.md), Hooks:Install (shared/library/hooks.md).
-- Observe-only: hooks never call Pass or Return (content/vext/guides/hooks.md); fixtures only in mh_bots.
-- ext/shared/__init__.lua also runs on its own in each realm; the shared modules are required here explicitly
-- (require caches by path, so loading twice is harmless) and the load order between the two entries does not matter.
MH = MH or {}
MH.row = require('__shared/mh_row')
MH.clock = require('__shared/mh_clock')
MH.vec = require('__shared/mh_vec')
MH.gs = require('__shared/mh_gs')
MH.ids = require('mh_ids')
MH.chat = require('mh_chat')
MH.cmd = require('mh_cmd')
MH.meta = require('mh_meta')
MH.bots = require('mh_bots')
MH.sites = require('mh_sites')
MH.samplers = require('mh_samplers')
MH.hooks = require('mh_hooks')
MH.events = require('mh_events')
MH.runner = require('mh_runner')
MH.scenarios = require('scenarios/registry')

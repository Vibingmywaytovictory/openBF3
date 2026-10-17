-- mharness shared entry (SKELETON). Loads the shared modules for both realms; does nothing else.
-- require paths: content/vext/guides/code-splitting.md ('__shared/' prefix).
MH = MH or {}
-- Marker for census.vm (DESIGN.md 3.1, live:L27): visible to the realm entry only if both share one Lua state.
MH._shared_entry = true
MH.row = require('__shared/mh_row')
MH.clock = require('__shared/mh_clock')
MH.vec = require('__shared/mh_vec')
MH.gs = require('__shared/mh_gs')

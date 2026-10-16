-- mharness shared entry (SKELETON). Loads the shared modules for both realms; does nothing else.
-- require paths: content/vext/guides/code-splitting.md ('__shared/' prefix).
MH = MH or {}
MH.row = require('__shared/mh_row')
MH.clock = require('__shared/mh_clock')
MH.vec = require('__shared/mh_vec')
MH.gs = require('__shared/mh_gs')

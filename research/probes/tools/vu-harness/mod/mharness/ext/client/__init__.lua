-- mharness client entry (SKELETON; DESIGN.md section 3.11). Rows only while the server's 'mh:ctl' says record.
-- NetEvents (client/library/netevents.md), Console:Register (client/library/console.md).
-- ext/shared/__init__.lua also runs on its own in each realm; the shared modules are required here explicitly
-- (require caches by path, so loading twice is harmless) and the load order between the two entries does not matter.
MH = MH or {}
MH.row = require('__shared/mh_row')
MH.clock = require('__shared/mh_clock')
MH.vec = require('__shared/mh_vec')
MH.gs = require('__shared/mh_gs')
MH.cgs = require('mh_cgs')
MH.fov = require('mh_fov')
MH.cam = require('mh_cam')

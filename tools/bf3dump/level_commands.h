#pragma once
// bf3dump level commands over bf3_core/level: the canonical placement listing of one level x mode
// (byte-identical to research/probes/tools/level-oracle, which drives IW4L's level_bf3) and the
// modes a level offers.

#include "bf3/core/install.h"

#include <memory>
#include <string>

namespace bf3dump {

// `level <LEVEL> <MODE> [--legacy-mvdb]`: load, traverse, census and collision placements; the
// listing goes to stdout, timings and counts to stderr.
int cmd_level(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level, const std::string &p_mode, bool p_legacy_mvdb);
// `levelmodes <LEVEL>`: the modes LevelDescriptionAsset lists, one per line.
int cmd_level_modes(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level);

} // namespace bf3dump

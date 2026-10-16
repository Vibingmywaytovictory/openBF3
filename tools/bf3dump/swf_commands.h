#pragma once
// bf3dump swf commands (UI ui1; docs/design/ui-swf-fonts.md §12.4): the SwfMovie census, one movie's symbol table,
// the atlas label map, a layout tree, the HUD placement table and the UI fonts. The output is BF3 data: keep it out
// of the repo. No command writes a converted font anywhere: `swf fonts` uses a derived-data Cache root only.
//
//   swf census [--tsv <out.tsv>] [--threads=N]
//   swf movie <name>
//   swf labels <name>
//   swf layout <name> [--depth=N]
//   swf hud
//   swf fonts [--text-db=<partition>] [--cache=<dir> | --no-cache]
// <name> is a SwfMovie res name ("ui/assets/iconsingame") or a bare movie name ("iconsingame").

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

const char *swf_usage();
// Runs `swf <sub> ...`; returns 2 on a usage error.
int cmd_swf(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump

#pragma once
// UI ui2 (docs/design/ui-minimap-icons.md §15): bf3dump's minimap, HUD icon and HUD source commands, read-only, text on
// stdout (counts and timings on stderr). Nothing is written anywhere.
//   minimap <LEVEL> [MODE]       UIMinimapData, the mode's combat areas and objectives (default TeamDeathMatch0) with
//                                each objective's UV, mask texel and inside flag through all 8 mapping variants
//   minimap --census             gate G2 / G3: every MP level's UIMinimapData and satellite, every combat-area asset and
//                                distance field over @all-mp
//   hud-icons                    the hud3dicons atlas: entries, states, frame rects
//   item-icons [LEVEL]           every weapon catalog variant of the level (default MP_Subway) -> its _small icon
//   hud-sources [--unbound]      the DataSources table (TSV) and the binding check; --unbound lists the in-game
//                                components without HudModel rows and their sources

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

const char *ui2_usage();
// Returns -1 when p_cmd is none of the ui2 commands.
int dispatch_ui2(const EbxContext &p_ctx, const std::string &p_cmd, const std::vector<std::string> &p_rest);

} // namespace bf3dump

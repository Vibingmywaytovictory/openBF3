#pragma once
// bf3dump looks (L1 look; README "L1 look"): the census of a level x mode's drawn materials, M1 mapping against the
// L1 mapping through the level shader database (bf3_render look_materials.h).

#include "bf3/core/install.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

// `looks <LEVEL> <MODE> [--materials] [--shader NAME]...`: per shader graph, the plan materials, the drawn instance slots,
// the texture parameters the slots name, the graph's constants, and how many slots M1 and L1 texture (base colour,
// normal map where BF3 gives one); then the totals and the gate line `l1.albedo <percent> ...`. --materials lists
// every L1 material (shader, colour and normal texture with tier / UV / scale). Exit 0 when the L1 base colour
// coverage of drawn instance slots is >= 95 %, else 1.
int cmd_looks(const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump

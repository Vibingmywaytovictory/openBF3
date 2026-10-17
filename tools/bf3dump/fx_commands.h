#pragma once
// M2a VFX1 (docs/design/m2a-vfx1.md §7.7): bf3dump fx census | compile | simulate | check | bench | atlas | impacts,
// the bf3_fx compiler, simulator, atlas and impact table printed, and the gate commands of §2.

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

const char *fx_usage();
// p_args: everything after "fx". Returns the process exit code (2 on a usage error).
int cmd_fx(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

} // namespace bf3dump

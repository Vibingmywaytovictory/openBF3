#pragma once
// bf3dump M2a vm commands (docs/design/m2a-vm.md; m2a.md §11.1, E5, E11): `vm clips`, the 1p viewmodel clip roles of
// the kit items as bf3_render's load_viewmodel_set resolves them (read-only; clips and EBX only, no meshes). Output is
// BF3 data and stays out of the repo. Ends with the install.unchanged row; exit 1 when the install changed or an
// E11 coverage row fails.

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

// vm clips [<unlock>...] [--team us|ru|all] [--level <sb>]: per item the role table (source, "__name", codec,
//   controller, seconds, channels), the zoom-clip endpoint check, the aiming controller's zoom data; the E5 rows
//   (each kit weapon's authored reload clip lengths next to its ReloadTime) and, without unlocks, the E11 coverage
//   report over the 8 default loadouts ("E11 VIEWMODEL COVERAGE: PASS (32/32)").
int cmd_vm(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

const char *vm_usage();

} // namespace bf3dump

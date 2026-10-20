#pragma once

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

int cmd_weapon_sim(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

int cmd_range(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

const char *weapon_sim_usage();

}

#pragma once

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

int cmd_lagcomp(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

const char *lagcomp_usage();

}

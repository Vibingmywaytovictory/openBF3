#pragma once

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

const char *fx_usage();

int cmd_fx(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

}

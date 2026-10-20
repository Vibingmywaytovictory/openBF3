#pragma once

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

int dispatch_gameplay(const EbxContext &p_ctx, const std::string &p_cmd, const std::vector<std::string> &p_args);

const char *gameplay_usage();

}

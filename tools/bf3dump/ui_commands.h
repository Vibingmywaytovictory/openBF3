#pragma once

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

const char *ui2_usage();

int dispatch_ui2(const EbxContext &p_ctx, const std::string &p_cmd, const std::vector<std::string> &p_rest);

}

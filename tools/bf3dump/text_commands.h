#pragma once

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

int cmd_text_lookup(const EbxContext &p_ctx, const std::vector<std::string> &p_keys);

int cmd_text_census(const EbxContext &p_ctx, const std::string &p_spec);

int cmd_text_tsv(const EbxContext &p_ctx, const std::string &p_out);

}

#pragma once
// bf3dump gameplay table inspectors (bf3_core gameplay readers): soldier, mode, kits, vehicle,
// scoring. Text output, one "key<TAB>value" per line.

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

// Runs a gameplay command; returns -1 when p_cmd/p_args are not one of them.
int dispatch_gameplay(const EbxContext &p_ctx, const std::string &p_cmd, const std::vector<std::string> &p_args);
// The usage lines of the gameplay commands.
const char *gameplay_usage();

} // namespace bf3dump

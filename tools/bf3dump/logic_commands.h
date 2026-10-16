#pragma once
// bf3dump logic commands (S4 level logic; docs/design/s4.md §9.4): the instantiated logic graph of a level x mode
// (bf3_core gameplay/logic_graph.h) and scripted interpreter runs (bf3_sim sim/logic/logic_harness.h). Read-only on
// the install; output to stdout, or to a file the write guard allows (census --out).

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

const char *logic_usage();
// `logic <sub> ...`: instantiate | stats | ports | fields | run | trace | census (see logic_usage()).
int cmd_logic(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump

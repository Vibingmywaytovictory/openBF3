#pragma once

#include <string>
#include <vector>

namespace bf3dump {

const char *master_usage();
int cmd_master(const std::vector<std::string> &p_args);
int cmd_servers(const std::vector<std::string> &p_args);

}

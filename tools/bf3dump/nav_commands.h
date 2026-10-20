#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

const char *nav_usage();
int cmd_nav(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);
int cmd_bots(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

}

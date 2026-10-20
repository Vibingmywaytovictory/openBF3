#pragma once

#include "bf3/core/install.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

int cmd_vehicledrive(const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

int cmd_vehicle_replay(const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

const char *vehicle_usage();

}

#pragma once

#include "bf3/core/install.h"

#include <memory>
#include <string>

namespace bf3dump {

int cmd_level(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level, const std::string &p_mode, bool p_legacy_mvdb);

int cmd_level_modes(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level);

}

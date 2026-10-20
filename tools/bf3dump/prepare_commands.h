#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

std::filesystem::path default_cache_root();

int cmd_prepare(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

}

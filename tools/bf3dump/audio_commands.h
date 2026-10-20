#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

int cmd_audio(const bf3::core::InstallReader &p_reader, std::shared_ptr<bf3::core::Install> p_install, const std::vector<std::string> &p_args);
const char *audio_usage();

}

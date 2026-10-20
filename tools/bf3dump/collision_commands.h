#pragma once

#include "bf3/core/install.h"
#include "bf3/core/level/collision_soup.h"

#include <memory>
#include <string>

namespace bf3dump {

bf3::core::Result<std::string> collision_listing(const bf3::core::level::LevelCollision &p_collision, unsigned p_threads = 0);

int cmd_collision(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level, const std::string &p_mode);

}

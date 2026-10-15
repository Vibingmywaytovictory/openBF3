#pragma once
// bf3dump prepare: converts one level x mode's placed meshes and its weapon variants into the derived-data
// cache (bf3_render level_prepare.h; README "Preparing the cache").

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

// `prepare <LEVEL> <MODE> [--cache=<dir>] [--threads=N]`. The default cache root is the folder Godot's
// user://bf3cache resolves to for the project (%APPDATA%\Godot\app_userdata\bf3-opensource\bf3cache).
int cmd_prepare(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump

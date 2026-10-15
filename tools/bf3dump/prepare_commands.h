#pragma once
// bf3dump prepare: converts one level x mode's placed meshes and its weapon variants into the derived-data
// cache (bf3_render level_prepare.h; README "Preparing the cache").

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

// The folder Godot's user://bf3cache resolves to for the project
// (%APPDATA%\Godot\app_userdata\bf3-opensource\bf3cache); empty when APPDATA is not set.
std::filesystem::path default_cache_root();

// `prepare <LEVEL> <MODE> [--cache=<dir>] [--threads=N] [--no-weapons] [--no-meshes] [--shapes]`. The default
// cache root is default_cache_root(). --shapes (M1 T1, docs/design/m1.md §12.4) adds the static world: after
// bf3_render's prepare_level returns, the level is opened again for a second, sequential pass through
// load_static_world_cached ("jolt-tile" and "jolt-world" entries), so a prepared root is warm for the game; it
// also prints the install.unchanged row.
int cmd_prepare(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump

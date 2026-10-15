#pragma once
// The game's derived-data cache (README "Derived-data cache", "Preparing the cache"): bf3_core's Cache under
// user://bf3cache, shared by everything in the process that converts install data (the viewmodel demo's meshes
// now, the level loader later).
//
// * default_cache_root() globalises user://bf3cache (ProjectSettings: main thread). It is the folder
//   `bf3dump prepare` writes by default (%APPDATA%\Godot\app_userdata\bf3-opensource\bf3cache), so a prepared
//   cache is what the game reads.
// * acquire_cache() opens the root on the calling thread, which should be a loader thread: open replays the
//   journal and reconciles the tree, which must not stall the main thread (plan §2.3). Holders of the same root
//   share one Cache (a root can be open once per process; another process holding it gives Busy, and the
//   callers then convert without a cache). The Cache closes when the last holder lets go.
// * Writes are guarded by InstallReader::write_guard(): the cache refuses a root inside the install.

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/path_guard.h"

#include <filesystem>
#include <memory>
#include <string>

namespace bf3::godot_side {

// user://bf3cache as an absolute path. Main thread only.
std::filesystem::path default_cache_root();

struct AcquiredCache {
	std::shared_ptr<bf3::core::Cache> cache; // null unless status is Ok
	bf3::core::CacheStatus status = bf3::core::CacheStatus::Closed;
	std::string detail;
	double open_seconds = 0.0; // 0 when an open Cache was shared
	bool shared = false; // another holder had it open already
};

// Any thread (meant for loader threads).
AcquiredCache acquire_cache(const std::filesystem::path &p_root, const bf3::core::PathGuard &p_guard);

} // namespace bf3::godot_side

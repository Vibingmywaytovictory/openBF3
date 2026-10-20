#pragma once

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/path_guard.h"

#include <filesystem>
#include <memory>
#include <string>

namespace bf3::godot_side {

std::filesystem::path default_cache_root();

struct AcquiredCache {
	std::shared_ptr<bf3::core::Cache> cache;
	bf3::core::CacheStatus status = bf3::core::CacheStatus::Closed;
	std::string detail;
	double open_seconds = 0.0;
	bool shared = false;
};

AcquiredCache acquire_cache(const std::filesystem::path &p_root, const bf3::core::PathGuard &p_guard);

}

#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render {

struct PrepareOptions {
	unsigned threads = 0;
	bool meshes = true;
	bool weapons = true;
	size_t max_failures_listed = 20;
};

struct PrepareTotals {
	uint64_t items = 0, hits = 0, converted = 0, reused = 0, uncached = 0, failed = 0, dropped_corrupt = 0;
	uint64_t bytes_hit = 0, bytes_written = 0;
	uint64_t vertices = 0, triangles = 0;
	double seconds = 0.0;
};

struct PrepareReport {
	std::string level;
	std::string mode;
	size_t partitions = 0;
	size_t placements = 0;
	size_t scene_meshes = 0;
	size_t census_failures = 0;
	PrepareTotals meshes;
	PrepareTotals weapons;
	std::vector<std::pair<std::string, std::string>> failures;
	double seconds_load = 0.0;
	double seconds_census = 0.0;
	double seconds_total = 0.0;
};

core::Result<PrepareReport> prepare_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode, core::Cache *p_cache,
		const PrepareOptions &p_options);

}

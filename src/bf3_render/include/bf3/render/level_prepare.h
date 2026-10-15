#pragma once
// `bf3dump prepare` (plan §3.12 "Prepare levels", §6 M0): converts what one level x mode draws into the
// derived-data cache, item by item. Godot-free.
//
//   1. load_level + World::traverse + mesh_census (bf3_core/level): the scene meshes the mode places.
//   2. Meshes: one item per distinct MeshSet res of a kept scene mesh. LODs: the census's (at most 4) when any
//      kept variant is model kind, LOD 0 when all are world kind (world-kind meshes keep only LOD 0); drawn
//      subsets; unskinned. Each item: read the res, compute its key, get the blob or load the LODs' chunks,
//      prepare_mesh, encode, put; the item's arrays are freed before the lane takes the next one, so peak
//      memory is the level database plus one item per lane.
//   3. Weapons: every SoldierWeaponUnlockAsset under weapons/ in the level database (the level bundle carries
//      the full weapon EBX), resolved with Win32/UI for the name SID, through resolve_weapon_cached.
// Items run on p_options.threads lanes (bf3::core::parallel_for). Textures are not cached (plan §2.4: decoded
// from the install at every load).

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
	unsigned threads = 0; // lanes for meshes and weapons; 0 = all cores
	bool meshes = true;
	bool weapons = true;
	size_t max_failures_listed = 20;
};

struct PrepareTotals {
	uint64_t items = 0, hits = 0, converted = 0, reused = 0, uncached = 0, failed = 0, dropped_corrupt = 0;
	uint64_t bytes_hit = 0, bytes_written = 0;
	uint64_t vertices = 0, triangles = 0; // meshes: over every prepared LOD
	double seconds = 0.0;
};

struct PrepareReport {
	std::string level;
	std::string mode;
	size_t partitions = 0;
	size_t placements = 0;
	size_t scene_meshes = 0; // kept census variants
	size_t census_failures = 0;
	PrepareTotals meshes;
	PrepareTotals weapons;
	std::vector<std::pair<std::string, std::string>> failures; // (item, error), the first few
	double seconds_load = 0.0; // load_level + traverse
	double seconds_census = 0.0;
	double seconds_total = 0.0;
};

core::Result<PrepareReport> prepare_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode, core::Cache *p_cache,
		const PrepareOptions &p_options);

} // namespace bf3::render

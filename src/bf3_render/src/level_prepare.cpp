#include "bf3/render/level_prepare.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/thread_pool.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/mesh_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace bf3::render {

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

void copy_totals(const core::ConvertTotals &p_from, PrepareTotals &r_to) {
	r_to.items = p_from.items;
	r_to.hits = p_from.hits;
	r_to.converted = p_from.converted;
	r_to.reused = p_from.reused;
	r_to.uncached = p_from.uncached;
	r_to.failed = p_from.failed;
	r_to.dropped_corrupt = p_from.dropped_corrupt;
	r_to.bytes_hit = p_from.bytes_hit;
	r_to.bytes_written = p_from.bytes_written;
}

struct Failures {
	std::mutex mutex;
	std::vector<std::pair<std::string, std::string>> list;
	size_t limit = 20;
	void add(std::string p_item, std::string p_error) {
		std::lock_guard<std::mutex> lock(mutex);
		if (list.size() < limit) {
			list.emplace_back(std::move(p_item), std::move(p_error));
		}
	}
};

}

core::Result<PrepareReport> prepare_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode, core::Cache *p_cache,
		const PrepareOptions &p_options) {
	const Clock::time_point start = Clock::now();
	PrepareReport report;
	report.level = std::string(p_level);
	report.mode = std::string(p_mode);
	Failures failures;
	failures.limit = p_options.max_failures_listed;

	core::level::LoadOptions load_options;
	load_options.threads = p_options.threads;
	core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded = core::level::load_level(p_install, p_level, p_mode, load_options);
	if (!loaded) {
		return loaded.error();
	}
	const core::level::LoadedLevel &level = **loaded;
	report.partitions = level.stats.partitions;
	core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(level);
	if (!world) {
		return world.error();
	}
	report.seconds_load = since(start);

	if (p_options.meshes) {
		Clock::time_point t0 = Clock::now();
		core::level::CensusOptions census_options;
		census_options.threads = p_options.threads;
		core::Result<core::level::MeshCensus> census = core::level::mesh_census(level, **world, census_options);
		if (!census) {
			return census.error();
		}
		report.seconds_census = since(t0);
		report.placements = census->placements;
		report.census_failures = census->failures.size();

		for (const core::level::SceneMesh &m : census->meshes) {
			if (m.kept) {
				++report.scene_meshes;
			}
		}
		const std::vector<SceneMeshRequest> items = scene_mesh_requests(*census);

		t0 = Clock::now();
		core::ConvertTotals totals;
		std::atomic<uint64_t> vertices{ 0 }, triangles{ 0 };
		const core::LevelMount &mount = level.mount();
		core::parallel_for(
				items.size(),
				[&](size_t i) {
					const SceneMeshRequest &item = items[i];
					core::Result<MeshHead> head = read_mesh_head(mount, item.res);
					if (!head) {
						totals.add_failure();
						failures.add(item.res, head.error().message);
						return;
					}
					core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, mount, *head, item.request);
					if (!mesh) {
						totals.add_failure();
						failures.add(item.res, mesh.error().message);
						return;
					}
					totals.add(mesh->item);
					if (!mesh->item.cache_error.empty()) {
						failures.add(item.res, "cache: " + mesh->item.cache_error);
					}
					vertices += mesh->mesh.vertices();
					triangles += mesh->mesh.triangles();
				},
				p_options.threads);
		copy_totals(totals, report.meshes);
		report.meshes.vertices = vertices;
		report.meshes.triangles = triangles;
		report.meshes.seconds = since(t0);
	}

	if (p_options.weapons) {
		const Clock::time_point t0 = Clock::now();
		const std::vector<std::string> variants = core::gameplay::weapon_variants_of(level.db());
		core::gameplay::WeaponCacheSources sources;
		sources.level = &level.db();
		sources.level_digest = core::gameplay::mount_partition_digest(level.mount());
		core::Result<std::unique_ptr<core::EbxLibrary>> ui = core::EbxLibrary::open(p_install, { "Win32/UI" }, p_options.threads);
		if (ui) {
			sources.ui = ui->get();
			sources.ui_digest = core::gameplay::library_partition_digest(**ui);
		} else {
			failures.add("Win32/UI", ui.error().message + " (weapons resolved without name SIDs)");
		}
		core::ConvertTotals totals;
		core::parallel_for(
				variants.size(),
				[&](size_t i) {
					core::Result<core::gameplay::CachedWeapon> w = core::gameplay::resolve_weapon_cached(p_cache, sources, variants[i]);
					if (!w) {
						totals.add_failure();
						failures.add(variants[i], w.error().message);
						return;
					}
					totals.add(w->item);
					if (!w->item.cache_error.empty()) {
						failures.add(variants[i], "cache: " + w->item.cache_error);
					}
				},
				p_options.threads);
		copy_totals(totals, report.weapons);
		report.weapons.seconds = since(t0);
	}
	report.failures = std::move(failures.list);
	report.seconds_total = since(start);
	return report;
}

}

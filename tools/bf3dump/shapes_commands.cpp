// bf3dump shapes (M1 T1; docs/design/m1.md §5.5): the static world of one level x mode through the cache.
#include "shapes_commands.h"

#include "prepare_commands.h"

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/static_world.h"

#include <windows.h>
// psapi after windows.h
#include <psapi.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>

using namespace bf3::core;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

double mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

std::filesystem::path utf8_path(std::string_view p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string path_utf8(const std::filesystem::path &p_path) {
	const std::u8string s = p_path.u8string();
	return std::string(s.begin(), s.end());
}

} // namespace

Result<ShapesPass> run_shapes_pass(const std::shared_ptr<Install> &p_install, std::string_view p_level, std::string_view p_mode, Cache *p_cache,
		unsigned p_threads, bool p_raycast) {
	ShapesPass out;
	auto clock = std::chrono::steady_clock::now();
	level::LoadOptions load;
	load.threads = p_threads;
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_install, p_level, p_mode, load);
	if (!loaded) {
		return loaded.error();
	}
	Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
	if (!world) {
		return world.error();
	}
	out.seconds_load = seconds_since(clock);
	clock = std::chrono::steady_clock::now();
	Result<level::SourceDigest> digest = level::level_source_digest(**loaded);
	if (!digest) {
		return digest.error();
	}
	out.source_digest = digest->digest ? digest->digest->hex() : std::string();
	out.digest_bundles = digest->bundles;
	out.digest_entries = digest->entries;
	out.seconds_digest = seconds_since(clock);
	bf3::sim::StaticWorldLoadOptions options;
	options.build.threads = p_threads;
	options.build.include_raycast = p_raycast;
	options.soup_threads = p_threads;
	Result<std::shared_ptr<const bf3::sim::StaticWorldShapes>> shapes = bf3::sim::load_static_world_cached(p_cache, **loaded, **world, options);
	if (!shapes) {
		return shapes.error();
	}
	out.shapes = std::move(*shapes);
	return out; // the World goes before the level it points into (reverse declaration order)
}

void print_shapes_pass(const ShapesPass &p_pass) {
	const bf3::sim::StaticWorldStats &s = p_pass.shapes->stats();
	std::printf("shapes   source digest %s (%zu bundles, %zu manifest entries, %.3f s)\n",
			p_pass.source_digest.empty() ? "unavailable" : p_pass.source_digest.c_str(), p_pass.digest_bundles, p_pass.digest_entries, p_pass.seconds_digest);
	std::printf("shapes   %zu tiles, %zu MeshShapes, %zu tags; triangles collision %zu, raycast %zu, character %zu (%zu in all)\n", s.tiles, s.shapes, s.tags,
			s.triangles[0], s.triangles[1], s.triangles[2], s.triangles[0] + s.triangles[1] + s.triangles[2]);
	std::printf("shapes   %s; %zu of %zu tiles hit, %zu cooked; resident %.1f MiB (Jolt GetStats), cache %.1f MiB read or written\n",
			s.world_hit ? "world hit (no soup)" : "world miss", s.tiles_hit, s.tiles, s.tiles_cooked, mib(s.shape_bytes), mib(s.cache_bytes));
	std::printf("shapes   load + traverse %.3f s; soup %.3f s, cook %.3f s, restore %.3f s, shapes total %.3f s\n", p_pass.seconds_load, s.seconds_soup, s.seconds_cook,
			s.seconds_restore, s.seconds_total);
}

int cmd_shapes(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	std::vector<std::string> positional;
	std::filesystem::path root;
	bool use_cache = true;
	bool raycast = true;
	bool tiles = false;
	unsigned threads = 0;
	for (const std::string &a : p_args) {
		if (a.rfind("--cache=", 0) == 0) {
			root = utf8_path(std::string_view(a).substr(8));
		} else if (a == "--no-cache") {
			use_cache = false;
		} else if (a == "--no-raycast") {
			raycast = false;
		} else if (a == "--tiles") {
			tiles = true;
		} else if (a.rfind("--threads=", 0) == 0) {
			const std::string_view v = std::string_view(a).substr(10);
			const auto res = std::from_chars(v.data(), v.data() + v.size(), threads);
			if (res.ec != std::errc() || res.ptr != v.data() + v.size() || threads == 0 || threads > 256) {
				std::fprintf(stderr, "shapes: --threads wants 1..256: %s\n", a.c_str());
				return 2;
			}
		} else if (a.rfind("--", 0) == 0) {
			std::fprintf(stderr, "shapes: unknown option %s\n", a.c_str());
			return 2;
		} else {
			positional.push_back(a);
		}
	}
	if (positional.size() != 2) {
		std::fprintf(stderr, "usage: bf3dump shapes <LEVEL> <MODE> [--cache=<dir> | --no-cache] [--threads=N] [--no-raycast] [--tiles]\n");
		return 2;
	}
	const InstallSnapshot before = p_reader.snapshot();
	Cache cache;
	if (use_cache) {
		if (root.empty()) {
			root = default_cache_root();
			if (root.empty()) {
				std::fprintf(stderr, "shapes: APPDATA is not set; pass --cache=<dir> or --no-cache\n");
				return 2;
			}
		}
		std::error_code ec;
		root = std::filesystem::absolute(root, ec);
		if (ec) {
			std::fprintf(stderr, "shapes: bad cache root\n");
			return 2;
		}
		CacheOptions cache_options;
		cache_options.root = root;
		cache_options.guard = p_reader.write_guard(); // refuses the install (and anything below it)
		std::string detail;
		const CacheStatus opened = cache.open(std::move(cache_options), &detail);
		if (opened != CacheStatus::Ok) {
			std::fprintf(stderr, "shapes: cache %s: %s %s\n", path_utf8(root).c_str(), cache_status_name(opened), detail.c_str());
			return 1;
		}
		std::printf("shapes %s %s -> %s (engine %s, jolt %s)\n", positional[0].c_str(), positional[1].c_str(), path_utf8(cache.root()).c_str(),
				std::string(cache_engine_id()).c_str(), bf3::sim::jolt_build_id().c_str());
	} else {
		std::printf("shapes %s %s (no cache, jolt %s)\n", positional[0].c_str(), positional[1].c_str(), bf3::sim::jolt_build_id().c_str());
	}
	const auto start = std::chrono::steady_clock::now();
	Result<ShapesPass> pass = run_shapes_pass(p_install, positional[0], positional[1], use_cache ? &cache : nullptr, threads, raycast);
	cache.close();
	if (!pass) {
		std::fprintf(stderr, "shapes %s %s: %s\n", positional[0].c_str(), positional[1].c_str(), pass.error().message.c_str());
		return 1;
	}
	print_shapes_pass(*pass);
	if (tiles) {
		const bf3::sim::StaticWorldShapes &w = *pass->shapes;
		for (size_t i = 0; i < w.tile_count(); ++i) {
			const bf3::sim::StaticTileInfo &t = w.tile(i);
			std::printf("K %d %d %u %u %u %u %s\n", t.x, t.z, t.triangles[0], t.triangles[1], t.triangles[2], t.tags, t.content.hex().c_str());
		}
	}
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
	const SnapshotDiff diff = diff_snapshots(before, p_reader.snapshot());
	std::printf("total    %.3f s; peak working set %.1f MiB, peak private bytes %.1f MiB\n", seconds_since(start), mib(pmc.PeakWorkingSetSize),
			mib(pmc.PeakPagefileUsage));
	std::printf("install.unchanged %s\n", before.valid && diff.clean() ? ("yes (" + std::to_string(before.files.size()) + " files: sizes and mtimes equal)").c_str()
																	  : ("NO: " + diff.summary()).c_str());
	return before.valid && diff.clean() ? 0 : 1;
}

} // namespace bf3dump

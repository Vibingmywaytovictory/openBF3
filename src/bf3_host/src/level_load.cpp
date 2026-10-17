// The Godot-free load steps of a session (docs/design/m1.md §12; level_load.h): the level database, then the
// SharedLevel (spawns, soldier, static world through the cache). Runs on the loader thread; the headless tests
// call it directly.
#include "bf3/host/level_load.h"

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/install.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi after windows.h
#include <psapi.h>

namespace bf3::host {

namespace {

uint64_t now_us() {
	return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

void sample_working_set(LoadProgress *p_progress) {
	if (p_progress == nullptr) {
		return;
	}
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		const uint64_t peak = uint64_t(pmc.PeakWorkingSetSize);
		uint64_t seen = p_progress->peak_working_set.load(std::memory_order_relaxed);
		while (peak > seen && !p_progress->peak_working_set.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) {
		}
	}
}

// Each stage's share of the M1 load budget (docs §12.1: Phase B dominates; meshes and textures run on the lanes,
// the upload behind the loading screen). Sums to 1000.
constexpr std::array<uint32_t, kLoadStageCount> kStageWeight = {
	20, // Mount
	80, // Database
	120, // Shapes
	250, // Meshes
	30, // Materials
	300, // Textures
	150, // Upload
	30, // Connect
	20, // Spawn
	0, // Done
	0, // Failed
};

} // namespace

const char *load_stage_name(LoadStage p_stage) {
	switch (p_stage) {
		case LoadStage::Mount:
			return "mount";
		case LoadStage::Database:
			return "db";
		case LoadStage::Shapes:
			return "shapes";
		case LoadStage::Meshes:
			return "meshes";
		case LoadStage::Materials:
			return "materials";
		case LoadStage::Textures:
			return "textures";
		case LoadStage::Upload:
			return "upload";
		case LoadStage::Connect:
			return "connect";
		case LoadStage::Spawn:
			return "spawn";
		case LoadStage::Done:
			return "done";
		case LoadStage::Failed:
			return "failed";
	}
	return "unknown";
}

void LoadProgress::begin(LoadStage p_stage, uint32_t p_total) {
	const size_t i = size_t(p_stage);
	if (i >= kLoadStageCount) {
		return;
	}
	total[i].store(p_total, std::memory_order_relaxed);
	uint64_t zero = 0;
	start_us[i].compare_exchange_strong(zero, now_us(), std::memory_order_relaxed);
	// The furthest stage started (Failed sticks: it is the highest value).
	uint8_t seen = stage.load(std::memory_order_relaxed);
	while (uint8_t(p_stage) > seen && !stage.compare_exchange_weak(seen, uint8_t(p_stage), std::memory_order_release)) {
	}
}

void LoadProgress::advance(LoadStage p_stage, uint32_t p_count) {
	const size_t i = size_t(p_stage);
	if (i < kLoadStageCount) {
		done[i].fetch_add(p_count, std::memory_order_relaxed);
	}
}

void LoadProgress::end(LoadStage p_stage) {
	const size_t i = size_t(p_stage);
	if (i >= kLoadStageCount) {
		return;
	}
	uint64_t zero = 0;
	start_us[i].compare_exchange_strong(zero, now_us(), std::memory_order_relaxed);
	end_us[i].store(now_us(), std::memory_order_release);
}

uint32_t LoadProgress::permille() const {
	if (stage.load(std::memory_order_acquire) == uint8_t(LoadStage::Done)) {
		return 1000;
	}
	uint64_t sum = 0;
	for (size_t i = 0; i < kLoadStageCount; ++i) {
		const uint32_t w = kStageWeight[i];
		if (w == 0) {
			continue;
		}
		if (end_us[i].load(std::memory_order_acquire) != 0) {
			sum += w;
			continue;
		}
		if (start_us[i].load(std::memory_order_relaxed) == 0) {
			continue;
		}
		const uint32_t t = total[i].load(std::memory_order_relaxed);
		const uint32_t d = done[i].load(std::memory_order_relaxed);
		if (t > 0) {
			sum += uint64_t(w) * std::min(d, t) / t;
		}
	}
	return uint32_t(std::min<uint64_t>(sum, 999));
}

LevelDatabase::LevelDatabase() = default;
LevelDatabase::~LevelDatabase() {
	world.reset(); // the World points into the level
	level.reset();
}
LevelDatabase::LevelDatabase(LevelDatabase &&) noexcept = default;
LevelDatabase &LevelDatabase::operator=(LevelDatabase &&p_other) noexcept {
	if (this != &p_other) {
		world.reset();
		level.reset();
		install = std::move(p_other.install);
		level = std::move(p_other.level);
		world = std::move(p_other.world);
		seconds_mount = p_other.seconds_mount;
		seconds_database = p_other.seconds_database;
	}
	return *this;
}

core::Result<LevelDatabase> open_level_database(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode,
		unsigned p_threads, LoadProgress *p_progress) {
	if (p_install == nullptr) {
		return core::make_error("open_level_database: no install");
	}
	LevelDatabase db;
	db.install = p_install;
	// bf3_core's load_level mounts the superbundle and parses the mode's partitions in one call: the Mount stage
	// covers both, the Database stage the traversal (docs §12.1 lists their times together, 0.17 s on Metro).
	if (p_progress != nullptr) {
		p_progress->begin(LoadStage::Mount, 1);
	}
	auto clock = std::chrono::steady_clock::now();
	core::level::LoadOptions load;
	load.threads = p_threads;
	core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded = core::level::load_level(p_install, p_level, p_mode, load);
	if (!loaded) {
		if (p_progress != nullptr) {
			p_progress->begin(LoadStage::Failed);
		}
		return core::error_at("open_level_database " + std::string(p_level) + " " + std::string(p_mode), loaded.error());
	}
	db.level = std::move(*loaded);
	const double load_seconds = seconds_since(clock);
	db.seconds_mount = std::max(0.0, load_seconds - db.level->stats.parse_seconds);
	if (p_progress != nullptr) {
		p_progress->advance(LoadStage::Mount);
		p_progress->end(LoadStage::Mount);
		p_progress->begin(LoadStage::Database, 1);
	}
	clock = std::chrono::steady_clock::now();
	core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(*db.level);
	if (!world) {
		if (p_progress != nullptr) {
			p_progress->begin(LoadStage::Failed);
		}
		return core::error_at("open_level_database " + std::string(p_level) + " " + std::string(p_mode) + ": traverse", world.error());
	}
	db.world = std::move(*world);
	db.seconds_database = db.level->stats.parse_seconds + seconds_since(clock);
	if (p_progress != nullptr) {
		p_progress->advance(LoadStage::Database);
		p_progress->end(LoadStage::Database);
	}
	sample_working_set(p_progress);
	return db;
}

core::Result<std::shared_ptr<const SharedLevel>> load_shared_level(const LevelDatabase &p_db, const SharedLoadOptions &p_options, LoadProgress *p_progress) {
	if (p_db.level == nullptr || p_db.world == nullptr) {
		return core::make_error("load_shared_level: no level database");
	}
	const core::level::LoadedLevel &level = *p_db.level;
	const core::level::World &world = *p_db.world;
	auto shared = std::make_shared<SharedLevel>();
	shared->level = level.info.id.empty() ? level.root_name : level.info.id;
	shared->mode = level.mode;

	auto clock = std::chrono::steady_clock::now();
	// Tables: the source digest (from the manifests the mount already read), the spawns, the soldier.
	core::Result<core::level::SourceDigest> digest = core::level::level_source_digest(level);
	if (!digest) {
		return core::error_at("load_shared_level: source digest", digest.error());
	}
	shared->source_digest = digest->digest;
	core::Result<core::level::LevelSpawns> spawns = core::level::read_level_spawns(world);
	if (!spawns) {
		return core::error_at("load_shared_level: spawns", spawns.error());
	}
	shared->spawns = sim::to_spawn_points(*spawns);
	if (shared->spawns.empty()) {
		return core::make_error("load_shared_level: the mode has no spawn points");
	}
	for (const core::level::CharacterSpawnerDef &s : spawns->spawners) {
		if (s.enabled && std::isfinite(s.protection_radius) && s.protection_radius > 0.0f) {
			shared->spawn_protection_radius = s.protection_radius; // TDM: both spawners say 10
			break;
		}
	}
	core::Result<core::gameplay::SoldierDef> soldier = core::gameplay::read_soldier_def(level.db());
	if (!soldier) {
		return core::error_at("load_shared_level: soldier", soldier.error());
	}
	core::Result<sim::MotorParams> params = sim::make_motor_params(*soldier, p_options.tuning);
	if (!params) {
		return core::error_at("load_shared_level: motor params", params.error());
	}
	shared->motor = *params;
	shared->seconds_tables = seconds_since(clock);

	// The static world through the cache (warm: restore; cold: soup + cook on the shape threads).
	if (p_progress != nullptr) {
		p_progress->begin(LoadStage::Shapes, 1);
	}
	clock = std::chrono::steady_clock::now();
	sim::StaticWorldLoadOptions options;
	options.build.threads = p_options.threads;
	options.soup_threads = p_options.threads;
	// S1 (docs/design/s1.md §5.2, §8): the terrain cover (sorted, unique), recorded for load_bot_level's check.
	options.terrain_cover = p_options.terrain_cover;
	std::sort(options.terrain_cover.begin(), options.terrain_cover.end());
	options.terrain_cover.erase(std::unique(options.terrain_cover.begin(), options.terrain_cover.end()), options.terrain_cover.end());
	shared->terrain_cover = options.terrain_cover;
	options.cover_water = p_options.cover_water;
	shared->cover_water = p_options.cover_water;
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::load_static_world_cached(p_options.cache, level, world, options);
	if (!shapes) {
		if (p_progress != nullptr) {
			p_progress->begin(LoadStage::Failed);
		}
		return core::error_at("load_shared_level: static world", shapes.error());
	}
	shared->static_world = std::move(*shapes);
	shared->seconds_shapes = seconds_since(clock);
	if (p_progress != nullptr) {
		const sim::StaticWorldStats &s = shared->static_world->stats();
		CacheWarmth warmth = CacheWarmth::Cold;
		if (s.world_hit) {
			warmth = CacheWarmth::Warm;
		} else if (s.tiles_hit > 0) {
			warmth = CacheWarmth::Partial;
		}
		p_progress->warmth.store(uint8_t(warmth), std::memory_order_relaxed);
		p_progress->advance(LoadStage::Shapes);
		p_progress->end(LoadStage::Shapes);
	}
	if (p_options.gunplay) { // M2a sv (named insertion: the gunplay tables, SharedLoadOptions::gunplay)
		const core::Status gunplay = load_shared_gunplay(p_db, *shared); // M2a sv
		if (!gunplay.ok()) { // M2a sv
			return core::error_at("load_shared_level: gunplay", gunplay.error()); // M2a sv
		} // M2a sv
	} // M2a sv
	sample_working_set(p_progress);
	return std::shared_ptr<const SharedLevel>(std::move(shared));
}

std::shared_ptr<const SharedLevel> synthetic_shared_level(std::shared_ptr<const sim::StaticWorldShapes> p_world, std::vector<sim::SpawnPoint> p_spawns,
		const sim::MotorParams &p_motor) {
	auto shared = std::make_shared<SharedLevel>();
	shared->level = "synthetic";
	shared->mode = "synthetic";
	shared->static_world = std::move(p_world);
	shared->spawns = std::move(p_spawns);
	shared->motor = p_motor;
	return shared;
}

} // namespace bf3::host

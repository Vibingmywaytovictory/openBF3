#pragma once

#include "bf3/core/cache/digest.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/result.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
class Install;
}
namespace bf3::core::level {
class LoadedLevel;
class World;
}
namespace bf3::core::gameplay {
class LogicGraph;
struct ScoringTable;
struct EorTimeline;
}
namespace bf3::sim::logic {
struct RoundNativeParams;
}

namespace bf3::host {

enum class LoadStage : uint8_t {
	Mount = 0,
	Database = 1,
	Shapes = 2,
	Meshes = 3,
	Materials = 4,
	Textures = 5,
	Upload = 6,
	Connect = 7,
	Spawn = 8,
	Done = 9,
	Failed = 10,
};
inline constexpr size_t kLoadStageCount = 11;
const char *load_stage_name(LoadStage p_stage);

enum class CacheWarmth : uint8_t { Unknown = 0, Cold = 1, Partial = 2, Warm = 3 };

struct LoadProgress {
	std::atomic<uint8_t> stage{ uint8_t(LoadStage::Mount) };
	std::array<std::atomic<uint32_t>, kLoadStageCount> done{};
	std::array<std::atomic<uint32_t>, kLoadStageCount> total{};
	std::array<std::atomic<uint64_t>, kLoadStageCount> start_us{};
	std::array<std::atomic<uint64_t>, kLoadStageCount> end_us{};
	std::atomic<uint8_t> warmth{ uint8_t(CacheWarmth::Unknown) };
	std::atomic<uint64_t> peak_working_set{ 0 };

	void begin(LoadStage p_stage, uint32_t p_total = 0);
	void advance(LoadStage p_stage, uint32_t p_count = 1);
	void end(LoadStage p_stage);

	uint32_t permille() const;
};

struct LevelDatabase {
	std::shared_ptr<const core::Install> install;
	std::unique_ptr<core::level::LoadedLevel> level;
	std::unique_ptr<core::level::World> world;
	double seconds_mount = 0.0;
	double seconds_database = 0.0;

	LevelDatabase();
	~LevelDatabase();
	LevelDatabase(LevelDatabase &&) noexcept;
	LevelDatabase &operator=(LevelDatabase &&) noexcept;
};

core::Result<LevelDatabase> open_level_database(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode,
		unsigned p_threads, LoadProgress *p_progress);

struct GunplayLevel;
struct TdmTuning;

struct SharedLevel {
	std::string level;
	std::string mode;
	std::string level_name_sid;
	std::optional<core::Sha256Digest> source_digest;
	std::shared_ptr<const sim::StaticWorldShapes> static_world;
	std::vector<sim::SpawnPoint> spawns;
	std::vector<core::level::CharacterSpawnerDef> spawners;
	float spawn_protection_radius = 10.0f;
	sim::MotorParams motor;
	double seconds_shapes = 0.0;
	double seconds_tables = 0.0;

	std::shared_ptr<const GunplayLevel> gunplay;
	double seconds_gunplay = 0.0;

	std::shared_ptr<const core::gameplay::LogicGraph> logic;
	uint64_t logic_digest = 0;
	double seconds_logic = 0.0;

	std::shared_ptr<const sim::logic::RoundNativeParams> logic_natives;
	std::shared_ptr<const TdmTuning> tdm_tuning;
	std::shared_ptr<const core::gameplay::ScoringTable> scoring;
	std::shared_ptr<const core::gameplay::EorTimeline> eor;
	std::vector<std::string> tdm_notes;
};

struct SharedLoadOptions {
	core::Cache *cache = nullptr;

	unsigned threads = 0;
	sim::MotorTuning tuning;

	bool gunplay = false;

	bool tdm = false;
	std::string tdm_tuning_cfg;
};

core::Result<std::shared_ptr<const SharedLevel>> load_shared_level(const LevelDatabase &p_db, const SharedLoadOptions &p_options,
		LoadProgress *p_progress);

std::shared_ptr<const SharedLevel> synthetic_shared_level(std::shared_ptr<const sim::StaticWorldShapes> p_world, std::vector<sim::SpawnPoint> p_spawns,
		const sim::MotorParams &p_motor);

core::Status load_shared_gunplay(const LevelDatabase &p_db, SharedLevel &r_level);

core::Status load_shared_gunplay(const LevelDatabase &p_db, SharedLevel &r_level, const SharedLoadOptions &p_options);

}

#pragma once
// Loading one level x mode for a session (plan §2.4, §3.12, D17; gap load §6.6-§6.7; docs/design/m1.md §12).
// Godot-free: the Godot node runs these on its loader thread and shows LoadProgress on the loading screen; the
// headless tests run them directly.
//
// Two results with different lifetimes:
//   LevelDatabase  the mounted level, its parsed partitions and the traversal (bf3_core LoadedLevel + World).
//                  World is not thread-safe (World::Scopes): only the loader thread runs the steps that read
//                  EBX (spawns, census, soup placements, scene plan). Its LevelMount (inside LoadedLevel) is
//                  read-only and thread-safe and the Phase B lanes read it (load_scene_request,
//                  prepare_scene_texture, prepare_scene_terrain), so the database lives until Phase B ends.
//   SharedLevel    what the simulation needs, immutable after the load and shared (shared_ptr<const>) by the
//                  ServerHost thread and the ClientSession thread: the static world shapes, the spawn points,
//                  the motor params, the ids and digests.
// Order on the loader thread (the World-touching steps are serial, the heavy steps parallel; docs §12.1):
//   open_level_database  mount -> partitions -> traverse                                  [Mount, Database]
//   bf3_render level_scene.h: census -> plan_level_scene -> SceneLanes::start (the mesh blobs, textures and
//                        terrain run on the scene lanes' own threads from here on)        [Meshes, Materials, Textures]
//   load_shared_level    spawns, soldier def; static world through the cache (cold: the soup and the tile cook
//                        run on SharedLoadOptions::threads threads of their own, beside the scene lanes) [Shapes]
// The scene plan goes first because load_static_world_cached blocks the loader until its cook ends. The two
// thread pools are sized together by the caller (docs §12.2: 16 cores = 8 scene lanes + 8 shape threads), so the
// load does not run 16 + 16 workers at once.

#include "bf3/core/cache/digest.h"
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
} // namespace bf3::core
namespace bf3::core::level {
class LoadedLevel;
class World;
} // namespace bf3::core::level
namespace bf3::core::gameplay {
class LogicGraph; // M2b W1.0 (ru): bf3/core/gameplay/logic_graph.h
} // namespace bf3::core::gameplay

namespace bf3::host {

// The loading screen's stages (the Loading message's stage byte, plan §2.7 "stage: mount|db|textures|meshes|
// shapes|nav|upload" plus the session's own connect and spawn).
enum class LoadStage : uint8_t {
	Mount = 0,
	Database = 1, // partitions + traversal
	Shapes = 2, // static world (cache restore, or soup + cook)
	Meshes = 3, // census + mesh blobs
	Materials = 4,
	Textures = 5,
	Upload = 6, // main thread: RenderingServer resources and instances
	Connect = 7, // server started, client connected, MatchInfo
	Spawn = 8, // first snapshot with the own soldier
	Done = 9,
	Failed = 10,
};
inline constexpr size_t kLoadStageCount = 11;
const char *load_stage_name(LoadStage p_stage); // "mount", "db", "shapes", ...

enum class CacheWarmth : uint8_t { Unknown = 0, Cold = 1, Partial = 2, Warm = 3 };

// Written by the loader thread(s), read by the main thread every frame. Lock-free.
struct LoadProgress {
	std::atomic<uint8_t> stage{ uint8_t(LoadStage::Mount) }; // the furthest stage started
	std::array<std::atomic<uint32_t>, kLoadStageCount> done{}; // items done per stage
	std::array<std::atomic<uint32_t>, kLoadStageCount> total{}; // items expected per stage (0 = unknown)
	std::array<std::atomic<uint64_t>, kLoadStageCount> start_us{}; // monotonic microseconds, 0 = not started
	std::array<std::atomic<uint64_t>, kLoadStageCount> end_us{};
	std::atomic<uint8_t> warmth{ uint8_t(CacheWarmth::Unknown) };
	std::atomic<uint64_t> peak_working_set{ 0 }; // bytes, sampled by the loader

	void begin(LoadStage p_stage, uint32_t p_total = 0);
	void advance(LoadStage p_stage, uint32_t p_count = 1);
	void end(LoadStage p_stage);
	// Overall progress 0..1000, stages weighted by their share of the M1 budget (docs §12).
	uint32_t permille() const;
};

struct LevelDatabase {
	std::shared_ptr<const core::Install> install;
	std::unique_ptr<core::level::LoadedLevel> level;
	std::unique_ptr<core::level::World> world; // declared after `level`: destroyed first
	double seconds_mount = 0.0;
	double seconds_database = 0.0;

	LevelDatabase();
	~LevelDatabase();
	LevelDatabase(LevelDatabase &&) noexcept;
	LevelDatabase &operator=(LevelDatabase &&) noexcept;
};

core::Result<LevelDatabase> open_level_database(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode,
		unsigned p_threads, LoadProgress *p_progress);

struct GunplayLevel; // M2a sv: gunplay.h

struct SharedLevel {
	std::string level; // MP_Subway
	std::string mode; // TeamDeathMatch0
	std::optional<core::Sha256Digest> source_digest; // level/source_digest.h
	std::shared_ptr<const sim::StaticWorldShapes> static_world;
	std::vector<sim::SpawnPoint> spawns; // every point of the mode (TDM: 102)
	float spawn_protection_radius = 10.0f; // the mode's CharacterSpawnReferenceObjectData (TDM: 10)
	sim::MotorParams motor; // from characters/soldiers/mpsoldier + MotorTuning
	double seconds_shapes = 0.0;
	double seconds_tables = 0.0;
	// (TC removed S1's terrain_cover / cover_water record: the static world has terrain everywhere, so the server's
	// motor, the client prediction world and the nav agree with no option; docs/design/terrain-collision.md §8.2.)
	// M2a sv (docs/design/m2a.md §4.2, §5.6, §6.5): the weapon defs, materials, soldier, hit capsules, capsule pose
	// table and kits (gunplay.h GunplayLevel), loaded when SharedLoadOptions::gunplay is set. Null: no gunplay, the
	// GameServer runs M1's game unchanged.
	std::shared_ptr<const GunplayLevel> gunplay;
	double seconds_gunplay = 0.0;
	// M2b W1.0 (docs/design/m2b.md §2.6 #3, §3.2 "Load"; owner ru): with SharedLoadOptions::tdm, the mode's logic graph
	// (build_logic_graph as bf3dump builds it; 0.16 s on Metro, not cached) and its digest (MatchInfo::logic_digest).
	// Null / 0 without it: no rules can run.
	std::shared_ptr<const core::gameplay::LogicGraph> logic;
	uint64_t logic_digest = 0;
	double seconds_logic = 0.0;
};

struct SharedLoadOptions {
	core::Cache *cache = nullptr; // null: convert without a cache
	// Soup + tile-cook threads (StaticWorldBuildOptions::threads and soup_threads); 0 = all cores. The session
	// passes the cores the scene lanes do not use (docs §12.2).
	unsigned threads = 0;
	sim::MotorTuning tuning;
	// (TC removed S1's terrain_cover and cover_water: every terrain cell has collision; the water rule is the bots'
	// nav bake's, BotLoadOptions / NavBakeOptions::water.)
	// M2a sv (docs/design/m2a.md §4.2): load the gunplay tables (load_gunplay_level: the level's weapons, the mode's
	// kits, the capsule pose table from the retail clips; ~1.5 s on Metro [measured, noisy], nothing cached). Off by
	// default so M1's load and its E4 / E5 numbers stay as they were until the integration turns it on.
	bool gunplay = false;
	// M2b W1.0 (docs/design/m2b.md §2.6 #3; owner ru): one switch for the three TDM load parts: the logic graph
	// (SharedLevel::logic), the assault gadget of #9 (GunplayLoadOptions::tdm_kits; needs gunplay) and the motor's
	// immunity abort of #19 (MotorParams::protect_abort_*). Off: the M2a load, tables and hashes, unchanged. A client
	// makes the same load as its server (MatchInfo::rules_flags).
	bool tdm = false;
};

// On the loader thread that owns p_db.
core::Result<std::shared_ptr<const SharedLevel>> load_shared_level(const LevelDatabase &p_db, const SharedLoadOptions &p_options,
		LoadProgress *p_progress);

// A SharedLevel over a synthetic static world (tests): the given spawn points, motor params from p_motor.
std::shared_ptr<const SharedLevel> synthetic_shared_level(std::shared_ptr<const sim::StaticWorldShapes> p_world, std::vector<sim::SpawnPoint> p_spawns,
		const sim::MotorParams &p_motor);

// M2a sv (docs/design/m2a.md §4.2, §5.6, §6.5): fills r_level.gunplay from the database's install, level and mode
// (gunplay.h load_gunplay_level). load_shared_level calls it when SharedLoadOptions::gunplay is set; a tool that holds
// a SharedLevel it built itself may call it too.
core::Status load_shared_gunplay(const LevelDatabase &p_db, SharedLevel &r_level);
// M2b W1.0 (#3; owner ru): the same with the load options (SharedLoadOptions::tdm: the TDM kits); the 2-argument form
// passes default options.
core::Status load_shared_gunplay(const LevelDatabase &p_db, SharedLevel &r_level, const SharedLoadOptions &p_options);

} // namespace bf3::host

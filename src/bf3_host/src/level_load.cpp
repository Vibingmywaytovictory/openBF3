#include "bf3/host/level_load.h"

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/install.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/static_world.h"

#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/logic/round_natives.h"

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

#include <psapi.h>

namespace bf3::host {

namespace {

uint64_t now_us() {
	return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

using bf3::core::seconds_since;

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

constexpr std::array<uint32_t, kLoadStageCount> kStageWeight = {
	20,
	80,
	120,
	250,
	30,
	300,
	150,
	30,
	20,
	0,
	0,
};

}

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
	world.reset();
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

core::Status load_shared_tdm(const LevelDatabase &p_db, const SharedLoadOptions &p_options, SharedLevel &r_level);

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
			shared->spawn_protection_radius = s.protection_radius;
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

	if (p_progress != nullptr) {
		p_progress->begin(LoadStage::Shapes, 1);
	}
	clock = std::chrono::steady_clock::now();
	sim::StaticWorldLoadOptions options;
	options.build.threads = p_options.threads;
	options.soup_threads = p_options.threads;
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
	if (p_options.gunplay) {
		const core::Status gunplay = load_shared_gunplay(p_db, *shared, p_options);
		if (!gunplay.ok()) {
			return core::error_at("load_shared_level: gunplay", gunplay.error());
		}
	}
	if (p_options.tdm) {
		const core::Status tdm = load_shared_tdm(p_db, p_options, *shared);
		if (!tdm.ok()) {
			return core::error_at("load_shared_level: tdm", tdm.error());
		}
	}
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

namespace {

namespace gp = core::gameplay;

std::optional<double> ebx_number(const core::EbxInstance &p_entity, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_entity.get(p_field);
	if (!v || v->kind() == core::EbxValue::Kind::Bool) {
		return std::nullopt;
	}
	if (const std::optional<double> d = v->as_f64()) {
		return std::isfinite(*d) ? d : std::nullopt;
	}
	if (const std::optional<int64_t> i = v->as_i64()) {
		return double(*i);
	}
	return std::nullopt;
}

std::optional<bool> ebx_bool(const core::EbxInstance &p_entity, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_entity.get(p_field);
	if (!v || v->kind() != core::EbxValue::Kind::Bool) {
		return std::nullopt;
	}
	return v->as_bool();
}

uint8_t ebx_team(const core::EbxInstance &p_entity, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_entity.get(p_field);
	if (!v) {
		return 0;
	}
	if (const std::optional<std::string_view> name = v->enum_name()) {
		return *name == "Team1" ? 1 : (*name == "Team2" ? 2 : 0);
	}
	const std::optional<int64_t> i = v->as_i64();
	return i && (*i == 1 || *i == 2) ? uint8_t(*i) : 0;
}

void read_native_params(const gp::LogicWorld &p_world, const gp::LogicGraph &p_graph, sim::logic::RoundNativeParams &r_params) {
	using T = gp::LogicTypeId;
	bool pre = false, human = false, auto_team = false, restriction = false;
	for (const gp::LogicObject &ob : p_world.objects()) {
		const T type = gp::logic_type_of(ob.entity.type_name());
		if (type != T::KillCounter && type != T::PreRound && type != T::HumanPlayer && type != T::AutoTeam && type != T::InputRestriction) {
			continue;
		}
		const core::EbxInstance &e = ob.entity;
		if (type == T::KillCounter) {
			for (const uint32_t node : p_graph.nodes_with_guid(e.guid())) {
				if (p_graph.nodes()[node].type != T::KillCounter) {
					continue;
				}
				bool seen = false;
				for (const sim::logic::RoundNativeParams::KillCounter &k : r_params.kill_counters) {
					seen |= k.node == node;
				}
				if (seen || r_params.kill_counters.size() >= 64) {
					continue;
				}
				sim::logic::RoundNativeParams::KillCounter k;
				k.node = node;
				k.team = ebx_team(e, "TeamId");
				k.max_kill_count = int32_t(std::clamp(ebx_number(e, "MaxKillCount").value_or(100.0), 0.0, 100000.0));
				k.enemy_weight = float(std::clamp(ebx_number(e, "EnemyWeight").value_or(1.0), -100.0, 100.0));
				k.ignore_ai = ebx_bool(e, "IgnoreAI").value_or(false);
				r_params.kill_counters.push_back(k);
			}
		} else if (type == T::PreRound && !pre) {
			pre = true;
			r_params.round_min_players = int32_t(std::clamp(ebx_number(e, "RoundMinPlayerCount").value_or(4.0), 0.0, 256.0));
			r_params.round_max_players = int32_t(std::clamp(ebx_number(e, "RoundMaxPlayerCount").value_or(8.0), 0.0, 256.0));
			r_params.round_restart_countdown_s = float(std::clamp(ebx_number(e, "RoundRestartCountdown").value_or(10.0), 0.0, 3600.0));
		} else if (type == T::HumanPlayer && !human) {
			human = true;
			r_params.player_killed_delay_s = float(std::clamp(ebx_number(e, "PlayerKilledDelay").value_or(5.0), 0.0, 3600.0));
		} else if (type == T::AutoTeam && !auto_team) {
			auto_team = true;
			r_params.auto_balance = ebx_bool(e, "AutoBalance").value_or(true);
			r_params.rotate_team_on_new_round = ebx_bool(e, "RotateTeamOnNewRound").value_or(true);
			r_params.balance_player_count = int32_t(std::clamp(ebx_number(e, "PlayerCountNeededToAutoBalance").value_or(8.0), 0.0, 256.0));
			r_params.balance_team_difference = int32_t(std::clamp(ebx_number(e, "TeamDifferenceToAutoBalance").value_or(3.0), 0.0, 256.0));
		} else if (type == T::InputRestriction && !restriction) {
			restriction = true;
			namespace rb = net::restrict_bits;
			static const std::pair<const char *, uint32_t> kFields[] = { { "Throttle", rb::kThrottle }, { "Strafe", rb::kStrafe }, { "Yaw", rb::kYaw },
				{ "Pitch", rb::kPitch }, { "Jump", rb::kJump }, { "ChangePose", rb::kCrouch }, { "Prone", rb::kProne }, { "Fire", rb::kFire },
				{ "Zoom", rb::kZoom }, { "Reload", rb::kReload }, { "Sprint", rb::kSprint }, { "MeleeAttack", rb::kMelee },
				{ "ThrowGrenade", rb::kThrowGrenade } };
			uint32_t allowed = 0;
			for (const auto &[field, bit] : kFields) {
				const std::optional<bool> b = ebx_bool(e, field);
				if (!b) {
					r_params.notes.push_back(std::string("InputRestriction.") + field + " not read (allowed)");
				}
				if (b.value_or(true)) {
					allowed |= bit;
				}
			}
			r_params.restrict_allowed = allowed;
		}
	}
	const bool counters = !r_params.kill_counters.empty();
	r_params.from_data = pre && human && auto_team && restriction && counters;
	if (!r_params.from_data) {
		r_params.notes.push_back(std::string("natives: missing") + (pre ? "" : " PreRound") + (human ? "" : " HumanPlayer") + (auto_team ? "" : " AutoTeam") +
				(restriction ? "" : " InputRestriction") + (counters ? "" : " KillCounter") + " (the named defaults)");
	}
}

void set_immunity_abort(const core::level::LoadedLevel &p_level, const TdmTuning &p_tuning, SharedLevel &r_level) {
	core::Result<gp::SoldierDef> soldier = gp::read_soldier_def(p_level.db());
	if (!soldier) {
		r_level.tdm_notes.push_back("immunity abort: no soldier def (" + soldier.error().message + "): none");
		return;
	}
	std::vector<std::optional<std::string>> names;
	if (const core::EbxPartition *partition = p_level.db().partition_by_name("characters/soldiers/mpsoldier")) {
		if (const std::optional<core::EbxInstance> entity = p_level.db().follow(partition->primary().data(), "Object")) {
			if (const std::optional<core::EbxInstance> health = p_level.db().follow(entity->data(), "HealthModule")) {
				if (const std::optional<core::EbxValue> list = health->get("AbortSpawnImmortalityInputs")) {
					if (const core::OptionalEbxArray array = list->as_array()) {
						for (const core::EbxValue v : *array) {
							if (names.size() >= kMaxImmunityAbortEntries) {
								break;
							}
							const std::optional<std::string_view> n = v.enum_name();
							names.push_back(n ? std::optional<std::string>(std::string(*n)) : std::nullopt);
						}
					}
				}
			}
		}
	}
	bool any_name = false;
	for (const std::optional<std::string> &n : names) {
		any_name |= n.has_value();
	}
	uint32_t buttons = 0;
	bool move = false;
	std::string mapped, unmapped;
	const std::vector<int32_t> &values = soldier->abort_spawn_immortality_inputs;
	for (size_t i = 0; i < values.size() && i < kMaxImmunityAbortEntries; ++i) {
		const int32_t value = values[i];
		const std::optional<std::string> name = i < names.size() ? names[i] : std::nullopt;
		std::string label = std::to_string(value);
		bool done = false;
		if (any_name && name) {

			std::string_view n = *name;
			if (n.rfind("EIA", 0) == 0) {
				n.remove_prefix(3);
			}
			label += "=" + *name;
			static const std::pair<const char *, uint32_t> kNames[] = { { "Fire", sim::buttons::kFire }, { "Jump", sim::buttons::kJump },
				{ "Sprint", sim::buttons::kSprint }, { "Zoom", sim::buttons::kZoom }, { "Reload", sim::buttons::kReload } };
			if (n == "Throttle" || n == "Strafe") {
				move = true;
				done = true;
			}
			for (const auto &[word, bit] : kNames) {
				if (n == word) {
					buttons |= bit;
					done = true;
				}
			}
		} else if (!any_name) {
			for (const ImmunityAbortEntry &m : p_tuning.immunity_abort_map) {
				if (m.value == value) {
					buttons |= m.buttons;
					move |= m.move;
					done = true;
				}
			}
		}
		std::string &list = done ? mapped : unmapped;
		list += (list.empty() ? "" : " ") + label;
	}
	r_level.motor.protect_abort_buttons = buttons;
	r_level.motor.protect_abort_move = move;
	r_level.tdm_notes.push_back(std::string("immunity abort: ") +
			(any_name ? "by the enum names" : "by tdm_tuning.cfg's immunity_abort_map (the type info has no enum names)") + "; mapped {" + mapped +
			"}, unmapped {" + unmapped + "} [inferred, measured = false]");
}

}

core::Status load_shared_tdm(const LevelDatabase &p_db, const SharedLoadOptions &p_options, SharedLevel &r_level) {
	if (p_db.level == nullptr) {
		return core::make_error("tdm: no level database");
	}
	const auto clock = std::chrono::steady_clock::now();
	const core::level::LoadedLevel &level = *p_db.level;
	gp::LogicGraphOptions go;
	go.mode = level.mode;
	go.side = gp::LogicSide::Server;
	gp::LogicWorldOptions wo;
	wo.mode = go.mode;
	wo.expand_root_objects = true;
	wo.max_depth = go.max_depth;
	wo.max_scopes = go.max_scopes;
	wo.max_objects = go.max_objects;
	wo.max_walk_steps = go.max_walk_steps;
	wo.max_query_steps = go.max_query_steps;
	wo.max_work = go.max_work;
	core::Result<gp::LogicWorld> world = gp::LogicWorld::build(level.db(), level.root(), wo);
	if (!world) {
		return core::error_at("tdm: logic graph walk", world.error());
	}
	core::Result<gp::LogicGraph> graph = gp::build_logic_graph(*world, go);
	if (!graph) {
		return core::error_at("tdm: logic graph", graph.error());
	}
	auto shared_graph = std::make_shared<const gp::LogicGraph>(std::move(*graph));
	r_level.logic_digest = shared_graph->digest();
	auto params = std::make_shared<sim::logic::RoundNativeParams>();
	read_native_params(*world, *shared_graph, *params);
	r_level.logic = std::move(shared_graph);
	for (const std::string &n : params->notes) {
		r_level.tdm_notes.push_back(n);
	}
	r_level.logic_natives = std::move(params);

	core::Result<TdmTuning> tuning = load_tdm_tuning(p_options.tdm_tuning_cfg);
	if (!tuning) {
		return tuning.error();
	}
	core::Result<gp::ScoringTable> scoring = gp::read_scoring(level.db());
	if (scoring) {
		const auto row = [&](std::string_view p_sid, int32_t &r_value) {
			if (const std::optional<double> v = scoring->score(p_sid); v && std::isfinite(*v) && std::fabs(*v) <= 100000.0) {
				r_value = int32_t(std::lround(*v));
			} else {
				r_level.tdm_notes.push_back("scoring: no " + std::string(p_sid) + " row (default " + std::to_string(r_value) + ")");
			}
		};
		row("ID_SCORE_KILL", tuning->score_kill);
		row("ID_SCORE_HEADSHOT", tuning->score_headshot);
		row("ID_SCORE_REVIVE", tuning->score_revive);
		r_level.scoring = std::make_shared<const gp::ScoringTable>(std::move(*scoring));
	} else {
		r_level.tdm_notes.push_back("scoring: " + scoring.error().message + " (the defaults)");
	}
	core::Result<gp::EorTimeline> eor = gp::read_eor_timeline(level.db());
	if (!eor) {
		return core::error_at("tdm", eor.error());
	}
	r_level.eor = std::make_shared<const gp::EorTimeline>(*eor);
	set_immunity_abort(level, *tuning, r_level);
	r_level.tdm_tuning = std::make_shared<const TdmTuning>(std::move(*tuning));
	r_level.seconds_logic = seconds_since(clock);
	return core::Status::success();
}

core::Status load_shared_gunplay(const LevelDatabase &p_db, SharedLevel &r_level, const SharedLoadOptions &p_options) {
	if (p_db.install == nullptr || p_db.level == nullptr) {
		return core::make_error("load_shared_gunplay: no level database");
	}
	const auto clock = std::chrono::steady_clock::now();
	const std::string id = p_db.level->info.id.empty() ? p_db.level->root_name : p_db.level->info.id;
	GunplayLoadOptions o;
	o.tdm_kits = p_options.tdm;
	o.loadouts = p_options.tdm;
	core::Result<std::shared_ptr<const GunplayLevel>> g = load_gunplay_level(p_db.install, id, p_db.level->mode, o);
	if (!g) {
		return g.error();
	}
	r_level.gunplay = std::move(*g);
	r_level.seconds_gunplay = seconds_since(clock);
	return core::Status::success();
}

}

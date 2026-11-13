#include "bf3/host/gunplay.h"

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/server_pose.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install.h"
#include "bf3/core/level/level.h"
#include "bf3/core/math.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/fnv.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/core/util/text.h"
#include "bf3/host/spawn_effects.h"
#include "bf3/host/squads.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_install.h"
#include "bf3/sim/weapons/weapon_rules.h"
#include "server_gadgets.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

namespace bf3::host {

namespace sw = sim::weapons;
namespace gp = core::gameplay;

namespace {

using bf3::core::ms_since;
using bf3::core::seconds_since;
using bf3::core::splitmix64;

bool alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead && p_state.life != 0;
}

constexpr std::array<std::array<uint8_t, 2>, gp::kCapsuleTransitions> kTransitionPoses{ { { 0, 1 }, { 0, 2 }, { 1, 0 }, { 1, 2 }, { 2, 0 }, { 2, 1 } } };

constexpr size_t kAckRing = 64;
constexpr size_t kShotBoostRing = 64;

struct ShotBoost {
	uint32_t shot_index = 0;
	float boost = 1.0f;
	bool set = false;
};
}

uint64_t gunplay_level_hash(std::string_view p_level, std::string_view p_mode) {
	uint64_t h = 0xCBF29CE484222325ull;
	auto add = [&](std::string_view p_text) {
		for (const char c : p_text) {
			h ^= uint8_t(c);
			h *= 0x100000001B3ull;
		}
	};
	add(p_level);
	add("/");
	add(p_mode);
	return h;
}

uint64_t derive_match_seed(uint64_t p_server_seed, uint64_t p_level_hash) {
	return splitmix64(p_server_seed ^ p_level_hash);
}

core::Result<gp::CapsulePoseTable> standin_capsule_table(const gp::SoldierDef &p_soldier, std::span<const gp::HitCapsule> p_capsules,
		const sw::SurfaceMaterialTable &p_materials) {
	core::Result<sw::CapsuleTableData> data = sw::capsule_table_data(p_soldier, p_capsules, p_materials);
	if (!data) {
		return core::error_at("standin_capsule_table", data.error());
	}
	const size_t n = data->standin[0].size();
	if (n == 0 || n > gp::kMaxTableCapsules || data->standin[1].size() != n || data->standin[2].size() != n) {
		return core::make_error("standin_capsule_table: the stand-in layouts have " + std::to_string(n) + " / " + std::to_string(data->standin[1].size()) +
				" / " + std::to_string(data->standin[2].size()) + " capsules");
	}
	gp::CapsulePoseTable t;
	t.version = gp::kCapsuleTableVersion;
	t.capsule_count = uint8_t(n);
	t.pitch_deg = anim::capsule_pitch_buckets(anim::CrowdConfig{});
	t.steady.resize(gp::kCapsuleStances * gp::kCapsulePitchBuckets * n);
	t.transition.resize(gp::kCapsuleTransitions * gp::kCapsuleTransitionFractions * gp::kCapsulePitchBuckets * n);
	for (size_t s = 0; s < gp::kCapsuleStances; ++s) {
		for (size_t pb = 0; pb < gp::kCapsulePitchBuckets; ++pb) {
			std::copy(data->standin[s].begin(), data->standin[s].end(), t.steady.begin() + ptrdiff_t(t.steady_index(s, pb)));
		}
		t.row_source[s] = gp::capsule_row_source::kStandIn;
	}
	for (size_t r = 0; r < gp::kCapsuleTransitions; ++r) {
		const size_t from = kTransitionPoses[r][0], to = kTransitionPoses[r][1];
		for (size_t f = 0; f < gp::kCapsuleTransitionFractions; ++f) {
			const float w = anim::capsule_transition_fraction(f);
			for (size_t pb = 0; pb < gp::kCapsulePitchBuckets; ++pb) {
				const gp::CapsuleLocal *a = &t.steady[t.steady_index(from, pb)];
				const gp::CapsuleLocal *b = &t.steady[t.steady_index(to, pb)];
				gp::CapsuleLocal *o = &t.transition[t.transition_index(r, f, pb)];
				for (size_t c = 0; c < n; ++c) {
					o[c] = a[c];
					for (size_t k = 0; k < 3; ++k) {
						o[c].a[k] = a[c].a[k] + (b[c].a[k] - a[c].a[k]) * w;
						o[c].b[k] = a[c].b[k] + (b[c].b[k] - a[c].b[k]) * w;
					}
				}
			}
		}
		t.row_source[gp::kCapsuleStances + r] = gp::capsule_row_source::kStandIn;
	}
	double bound = 0;
	for (const std::vector<gp::CapsuleLocal> *v : { &t.steady, &t.transition }) {
		for (const gp::CapsuleLocal &e : *v) {
			for (const float *p : { e.a, e.b }) {
				bound = std::max(bound, std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]) + double(e.radius));
			}
		}
	}
	t.bound_radius = float(bound) + 1e-3f;
	t.hash = gp::capsule_pose_table_hash(t);
	if (!gp::capsule_pose_table_valid(t)) {
		return core::make_error("standin_capsule_table: the table fails its structural checks");
	}
	return t;
}

namespace {

gp::CapsulePoseTable build_capsules(std::shared_ptr<const core::Install> p_install, GunplayLevel &r_level, const sw::CapsuleTableData &p_data,
		bool &r_ok) {
	r_ok = false;
	auto fallback = [&](const std::string &p_why) {
		r_level.notes.push_back("capsules: whole-table stand-in (" + p_why + ")");
		r_level.capsule_standin = true;
		core::Result<gp::CapsulePoseTable> s = standin_capsule_table(r_level.soldier, r_level.capsules, *r_level.materials);
		if (!s) {
			r_level.notes.push_back("capsules: stand-in failed: " + s.error().message);
			return gp::CapsulePoseTable();
		}
		r_ok = true;
		return std::move(*s);
	};
	core::Result<std::unique_ptr<core::ant::AntInstallSource>> source = core::ant::AntInstallSource::open(p_install);
	if (!source) {
		return fallback("the Ant source did not open: " + source.error().message);
	}
	core::Result<std::unique_ptr<core::ant::AntLibrary>> lib = anim::load_ant_library(**source);
	if (!lib) {
		return fallback("the Ant library did not load: " + lib.error().message);
	}
	core::Result<anim::ServerPoseClips> clips = anim::load_server_pose_clips(**source, **lib);
	if (!clips) {
		return fallback("the server pose clips did not load: " + clips.error().message);
	}
	for (const std::string &n : clips->notes) {
		r_level.notes.push_back("capsules: clips: " + n);
	}
	anim::CapsuleTableInputs inputs;
	inputs.capsules = r_level.capsules;
	inputs.property = p_data.property;
	inputs.zone = p_data.zone;
	inputs.standin = p_data.standin;
	anim::CapsuleTableReport report;
	core::Result<gp::CapsulePoseTable> table = anim::build_capsule_pose_table(clips->rig, *clips, inputs, &report);
	if (!table) {
		return fallback("the builder failed: " + table.error().message);
	}
	for (const std::string &n : report.notes) {
		r_level.notes.push_back("capsules: build: " + n);
	}
	size_t standins = 0;
	for (const uint8_t s : table->row_source) {
		standins += s == gp::capsule_row_source::kStandIn ? 1 : 0;
	}
	r_level.notes.push_back("capsules: " + std::to_string(gp::kCapsuleRows - standins) + " clip-derived rows, " + std::to_string(standins) + " stand-in rows");
	r_ok = true;
	return std::move(*table);
}

core::Status sim_def_status(const core::Result<gp::WeaponDef> &p_def, const sw::SurfaceMaterialTable &p_materials) {
	if (!p_def) {
		return p_def.error();
	}
	const core::Result<sw::WeaponSimDef> d = sw::make_weapon_sim_def(*p_def, p_materials);
	return d ? core::Status::success() : core::Status(d.error());
}

}

core::Result<std::shared_ptr<const GunplayLevel>> load_gunplay_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level_id,
		std::string_view p_mode) {
	return load_gunplay_level(std::move(p_install), p_level_id, p_mode, GunplayLoadOptions{});
}

core::Result<std::shared_ptr<const GunplayLevel>> load_gunplay_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level_id,
		std::string_view p_mode, const GunplayLoadOptions &p_options) {
	const std::string where = "load_gunplay_level " + std::string(p_level_id) + " " + std::string(p_mode);
	if (p_install == nullptr) {
		return core::make_error(where + ": no install");
	}
	const std::optional<core::Level> info = p_install->level(p_level_id);
	if (!info) {
		return core::make_error(where + ": no such level");
	}
	auto level = std::make_shared<GunplayLevel>();
	level->level_hash = gunplay_level_hash(p_level_id, p_mode);
	auto clock = std::chrono::steady_clock::now();
	core::Result<sw::InstallWeapons> w = sw::open_install_weapons(p_install, info->superbundle);
	if (!w) {
		return core::error_at(where + ": weapons", w.error());
	}
	level->materials = w->materials;
	level->soldier = w->soldier;
	level->capsules = w->capsules;
	level->sprint_recover_seconds = w->sprint_recover_seconds;
	level->max_health = w->max_health;
	level->seconds_weapons = seconds_since(clock);

	clock = std::chrono::steady_clock::now();
	const core::EbxResolver &db = *w->catalog->sources().level;
	gp::WorkBudget budget;
	core::Result<gp::ModeDef> mode = gp::read_mode_def(db, p_level_id, p_mode, &budget);
	if (!mode) {
		return core::error_at(where + ": mode", mode.error());
	}
	core::Result<gp::UnlockProgression> progression = gp::read_unlock_progression(db, &budget);
	if (!progression) {
		return core::error_at(where + ": progression", progression.error());
	}
	core::Result<gp::WeaponSwitching> switching = gp::read_weapon_switching(db, &budget);
	if (!switching) {
		return core::error_at(where + ": weapon switching", switching.error());
	}
	level->switching = std::move(*switching);
	std::vector<gp::KitDef> kits;
	std::set<std::string> variants;
	for (const gp::TeamDef &t : mode->teams) {
		if (t.team < 1 || t.team > int(kKitTeams)) {
			continue;
		}
		for (const std::string &asset : t.kits) {
			core::Result<gp::KitDef> kit = gp::read_kit_def(db, asset, &*progression, &budget);
			if (!kit) {
				return core::error_at(where + ": kit " + asset, kit.error());
			}
			for (const uint8_t s : kKitSlots) {
				if (const gp::KitUnlock *u = kit->default_unlock(gp::WeaponSlot(s))) {
					std::string v = core::to_lower_ascii(u->unlock);
					variants.insert(v);
				}
			}
			kits.push_back(std::move(*kit));
		}
	}
	if (p_options.tdm_kits) {
		variants.insert(std::string(kTdmAssaultGadget));
	}
	std::vector<std::string> ordered(variants.begin(), variants.end());
	if (p_options.loadouts) {
		for (std::string &v : loadout_variants(kits)) {
			if (!variants.contains(v)) {
				ordered.push_back(std::move(v));
			}
		}
	}
	for (const std::string &v : ordered) {
		core::Result<gp::WeaponDef> def = w->catalog->resolve(v);
		if (!def) {
			return core::error_at(where + ": weapon " + v, def.error());
		}
		level->defs.push_back(std::move(*def));
	}
	if (p_options.loadouts) {
		for (const KitGadgetUnlock &u : loadout_gadget_unlocks(kits)) {
			if (u.type != "SoldierWeaponUnlockAsset" || u.weapon.empty()) {
				level->notes.push_back("gadget " + u.unlock + ": a " + u.type + " with no weapon");
				continue;
			}
			core::Result<sw::GadgetDef> g = sw::read_gadget_def(db, u.weapon);
			if (!g) {
				return core::error_at(where, g.error());
			}
			if (variants.contains(u.unlock)) {
				continue;
			}
			if (g->fire_only_when_locked) {
				level->notes.push_back("gadget " + u.unlock + ": FireOnlyWhenLockedOn, and no lock-on is simulated");
				continue;
			}
			const bool deployable = g->kind != sw::GadgetKind::None;
			core::Result<gp::WeaponDef> def = w->catalog->resolve(u.unlock);
			const core::Status simulated = sim_def_status(def, *level->materials);
			if (!simulated) {
				if (deployable) {
					return core::error_at(where + ": gadget " + u.unlock, simulated.error());
				}
				level->notes.push_back("gadget " + u.unlock + ": " + simulated.error().message);
				continue;
			}
			if (deployable) {
				g->def = uint16_t(level->defs.size());
				level->gadgets.push_back(std::move(*g));
			}
			level->defs.push_back(std::move(*def));
			variants.insert(u.unlock);
		}
		for (const gp::WeaponDef &d : level->defs) {
			core::Result<sw::AmmoPickupDef> pick = sw::read_ammo_pickup(db, d.blueprint);
			if (!pick) {
				level->notes.push_back("ammo pickup: " + pick.error().message);
			}
			level->ammo_pickups.push_back(pick ? *pick : sw::AmmoPickupDef());
		}
	}
	KitTableOptions kit_options;
	if (p_options.tdm_kits) {
		kit_options.assault_gadget = std::string(kTdmAssaultGadget);
	}
	core::Result<KitTable> table = make_kit_table(mode->teams, kits, level->defs, kit_options);
	if (!table) {
		return core::error_at(where, table.error());
	}
	level->kits = std::move(*table);
	if (p_options.loadouts) {
		core::Result<LoadoutCatalog> catalog =
				make_loadout_catalog(db, *progression, kits, level->defs, level->kits, &budget);
		if (!catalog) {
			return core::error_at(where, catalog.error());
		}
		LoadoutVariantsReport report;
		core::Result<std::vector<std::vector<sw::WeaponSimDef>>> variants = make_loadout_variants(
				*catalog, level->defs,
				[&](const std::string &p_unlock, const std::vector<std::string> &p_accessories) {
					return w->catalog->resolve(p_unlock, p_accessories);
				},
				*level->materials, &report);
		if (!variants) {
			return core::error_at(where, variants.error());
		}
		level->variants = std::move(*variants);
		level->variant_resolves = report.resolves;
		catalog->hash = loadout_catalog_hash(*catalog);
		level->loadouts = std::move(*catalog);
		level->kits.hash = core::fnv1a_fold(level->kits.hash, level->loadouts.hash);
		core::Result<gp::SpecializationTable> specializations = gp::make_specialization_table(level->soldier, kits);
		if (!specializations) {
			return core::error_at(where, specializations.error());
		}
		level->specializations = std::move(*specializations);
		level->kits.hash = core::fnv1a_fold(level->kits.hash, level->specializations.hash);
		level->kits.hash = core::fnv1a_fold(level->kits.hash, gadget_defs_hash(level->gadgets, level->ammo_pickups));
	}
	for (const std::string &s : level->kits.skipped) {
		level->notes.push_back("kits: skipped " + s);
	}
	level->seconds_kits = seconds_since(clock);

	clock = std::chrono::steady_clock::now();
	core::Result<sw::CapsuleTableData> data = sw::capsule_table_data(level->soldier, level->capsules, *level->materials);
	if (!data) {
		return core::error_at(where + ": capsule inputs", data.error());
	}
	bool ok = false;
	level->capsule_table = build_capsules(p_install, *level, *data, ok);
	if (!ok) {
		return core::make_error(where + ": no capsule table (" + (level->notes.empty() ? std::string("?") : level->notes.back()) + ")");
	}
	level->seconds_capsules = seconds_since(clock);
	return std::shared_ptr<const GunplayLevel>(std::move(level));
}

core::Result<sw::Mechanics> load_mechanics_cfg(const std::string &p_path) {
	if (p_path.empty()) {
		return sw::Mechanics{};
	}
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(p_path, ec);
	if (ec) {
		return core::make_error("weapon mechanics: " + p_path + ": " + ec.message());
	}
	if (size > sw::kMaxMechanicsTuningBytes) {
		return core::make_error("weapon mechanics: " + p_path + ": " + std::to_string(size) + " bytes (limit " + std::to_string(sw::kMaxMechanicsTuningBytes) + ")");
	}
	std::ifstream in(p_path, std::ios::binary);
	if (!in) {
		return core::make_error("weapon mechanics: cannot open " + p_path);
	}
	std::string text(size_t(size), '\0');
	in.read(text.data(), std::streamsize(size));
	text.resize(size_t(in.gcount()));
	core::Result<sw::Mechanics> m = sw::parse_mechanics_tuning(text);
	if (!m) {
		return core::error_at(p_path, m.error());
	}
	return *m;
}

core::Result<std::shared_ptr<const sw::WeaponTable>> make_match_table(const GunplayLevel &p_level, const sw::Mechanics &p_mechanics, bool p_exact_aim,
		bool p_infinite_magazine) {
	sw::WeaponTableOptions o;
	o.mechanics = p_mechanics;
	o.exact_aim = p_exact_aim;
	o.infinite_magazine = p_infinite_magazine;
	o.sprint_recover_seconds = p_level.sprint_recover_seconds;
	o.variants = p_level.variants;
	if (p_level.materials == nullptr) {
		return core::make_error("make_match_table: no materials");
	}
	return sw::make_weapon_table(p_level.defs, p_level.materials, o);
}

uint64_t match_weapon_hash(const sw::WeaponTable &p_table, const gp::CapsulePoseTable &p_capsules) {
	return sw::weapon_table_hash(p_table) ^ p_capsules.hash;
}

uint8_t match_weapon_flags(bool p_exact_aim, bool p_infinite_magazine) {
	return uint8_t((p_exact_aim ? net::weapon_flags::kExactAim : 0u) | (p_infinite_magazine ? net::weapon_flags::kInfiniteMagazine : 0u));
}

core::Status load_shared_gunplay(const LevelDatabase &p_db, SharedLevel &r_level) {
	if (p_db.install == nullptr || p_db.level == nullptr) {
		return core::make_error("load_shared_gunplay: no level database");
	}
	const auto clock = std::chrono::steady_clock::now();
	const std::string id = p_db.level->info.id.empty() ? p_db.level->root_name : p_db.level->info.id;
	core::Result<std::shared_ptr<const GunplayLevel>> g = load_gunplay_level(p_db.install, id, p_db.level->mode);
	if (!g) {
		return g.error();
	}
	r_level.gunplay = std::move(*g);
	r_level.seconds_gunplay = seconds_since(clock);
	return core::Status::success();
}

struct Gunplay::Impl {
	struct AckEntry {
		bool valid = false;
		sim::Tick tick = 0;
		AckState ack;
	};
	struct Slot {
		bool armed = false;
		uint8_t team = 0;
		uint8_t kit = 0;
		sim::Tick spawn_tick = 0;
		bool fired = false;
		sw::SoldierWeaponsState weapons;
		sw::WeaponTickOutput out;
		sim::Tick out_tick = 0;
		bool shot_last_tick = false;
		bool firing = false;
		uint32_t reload_count = 0;
		uint32_t burst_count = 0;
		std::array<AckEntry, kAckRing> acks{};
		std::array<std::array<std::optional<LoadoutChoice>, kKitCount>, kKitTeams> choices{};
		Loadout loadout;
		uint32_t specializations = 0;
	};
	struct TickDeath {
		net::DeathEvent event;
	};

	const SharedLevel *shared = nullptr;
	const GunplayLevel *level = nullptr;
	GunplayConfig config;
	std::shared_ptr<const sw::WeaponTable> table;
	uint64_t match_seed = 0;
	uint64_t weapon_hash = 0;
	const SquadTable *squads = nullptr;
	uint8_t flags = 0;
	std::unique_ptr<sw::ProjectileWorld> projectiles;
	std::unique_ptr<ServerGadgets> gadgets;
	std::vector<GadgetSoldier> gadget_soldiers;
	std::span<const sw::PackVehicle> pack_vehicles;
	std::unique_ptr<LagHistory> history;
	std::unique_ptr<RewindProvider> rewind;
	const sim::CollisionWorld *world = nullptr;
	std::array<Slot, net::kMaxSlots> slots{};
	std::array<uint8_t, net::kMaxSlots> teams{};
	sw::SuppressionDef suppression_def;
	std::array<sw::SuppressionState, net::kMaxSlots> suppression{};
	std::array<std::array<ShotBoost, kShotBoostRing>, net::kMaxSlots> shot_boost{};
	std::unordered_map<uint64_t, sim::Vec3> update_start;
	std::vector<sw::Projectile> flying;
	std::unordered_map<uint64_t, size_t> landed;
	sw::ProjectileStepOutput step_out;
	std::span<const sim::vehicles::VehicleTarget> vehicle_targets;
	sim::Tick step_tick = 0;
	std::vector<sw::DamageEvent> tick_damage;
	std::vector<net::DeathEvent> tick_deaths;
	std::vector<sw::DetonationEvent> tick_detonations;
	std::vector<uint8_t> pending_fall_deaths;
	std::vector<sw::ShotEvent> interval_shots;
	GunplayStats stats;
	double weapons_acc_ms = 0;
	sw::SoldierWeaponsState empty_state;
	std::vector<RewindTraceRow> rewind_rows;

	float boost_at_spawn(uint32_t p_slot, uint32_t p_shot, float p_now) const {
		if (p_slot >= shot_boost.size()) {
			return p_now;
		}
		const ShotBoost &b = shot_boost[p_slot][p_shot % kShotBoostRing];
		return b.set && b.shot_index == p_shot ? b.boost : p_now;
	}

	void clear(uint8_t p_slot) {
		Slot &s = slots[p_slot];
		const sw::SoldierWeaponsState old = s.weapons;
		s.armed = false;
		s.weapons = sw::SoldierWeaponsState();
		s.weapons.player_slot = p_slot;
		s.weapons.next_shot_index = old.next_shot_index;
		s.weapons.tick = old.tick;
		s.shot_last_tick = false;
	}
};

Gunplay::Gunplay() :
		impl_(std::make_unique<Impl>()) {}

Gunplay::~Gunplay() = default;

core::Result<std::unique_ptr<Gunplay>> Gunplay::create(const SharedLevel &p_level, const GunplayConfig &p_config) {
	if (p_level.gunplay == nullptr) {
		return core::make_error("Gunplay: the level has no gunplay tables (SharedLoadOptions::gunplay)");
	}
	const GunplayLevel &level = *p_level.gunplay;
	if (!gp::capsule_pose_table_valid(level.capsule_table)) {
		return core::make_error("Gunplay: the capsule pose table fails its structural checks");
	}
	core::Result<sw::Mechanics> mechanics = load_mechanics_cfg(p_config.mechanics_cfg);
	if (!mechanics) {
		return core::error_at("Gunplay", mechanics.error());
	}
	core::Result<std::shared_ptr<const sw::WeaponTable>> table = make_match_table(level, *mechanics, p_config.exact_aim, p_config.infinite_magazine);
	if (!table) {
		return core::error_at("Gunplay: weapon table", table.error());
	}
	std::unique_ptr<Gunplay> g(new Gunplay());
	Impl &m = *g->impl_;
	m.shared = &p_level;
	m.level = &level;
	m.config = p_config;
	m.table = *table;
	m.match_seed = p_config.match_seed;
	m.weapon_hash = match_weapon_hash(*m.table, level.capsule_table);
	m.flags = match_weapon_flags(p_config.exact_aim, p_config.infinite_magazine);
	m.projectiles = std::make_unique<sw::ProjectileWorld>(m.table);
	m.gadgets = std::make_unique<ServerGadgets>(level, m.table);
	core::Result<sw::SuppressionDef> suppression = sw::make_suppression_def(level.soldier);
	if (!suppression) {
		return core::error_at("Gunplay", suppression.error());
	}
	m.suppression_def = *suppression;
	m.history = std::make_unique<LagHistory>(p_config.lag, uint32_t(net::kMaxSlots));
	m.rewind = std::make_unique<RewindProvider>(*m.history, level.capsule_table, p_config.lag);
	if (p_config.observer != nullptr && p_config.observer->wants_rewind_rows()) {
		m.rewind->set_trace(&m.rewind_rows);
	}
	for (size_t i = 0; i < m.slots.size(); ++i) {
		m.slots[i].weapons.player_slot = uint32_t(i);
	}
	return g;
}

void Gunplay::set_world(const sim::CollisionWorld &p_world) {
	impl_->world = &p_world;
}

void Gunplay::set_match_seed(uint64_t p_seed) {
	impl_->match_seed = p_seed;
}

void Gunplay::set_squads(const SquadTable *p_squads) {
	impl_->squads = p_squads;
}

gp::SpecializationEffects Gunplay::effects(uint8_t p_slot) const {
	return gp::specialization_effects_of(impl_->level->specializations, specialization_mask(p_slot));
}

uint32_t Gunplay::specialization_mask(uint8_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size()) {
		return 0;
	}
	std::vector<uint32_t> squad;
	if (m.squads != nullptr && m.squads->squad_of(p_slot) != 0) {
		for (const uint8_t mate : m.squads->members(m.squads->team_of(p_slot), m.squads->squad_of(p_slot))) {
			if (mate < m.slots.size() && (mate == p_slot || m.slots[mate].armed)) {
				squad.push_back(m.slots[mate].loadout.choice.specialization);
			}
		}
	}
	return gp::specialization_mask(m.level->specializations, m.slots[p_slot].loadout.choice.specialization, squad);
}

uint32_t Gunplay::spawn_specializations(uint8_t p_slot) const {
	const Impl &m = *impl_;
	return p_slot < m.slots.size() && m.slots[p_slot].armed ? m.slots[p_slot].specializations : 0u;
}

sim::Command clamp_command_pitch(const sim::Command &p_command, const gp::AimingPose &p_pose) {
	sim::Command out = p_command;
	const int16_t low = sim::quantize_pitch(p_pose.min_pitch_deg * core::kDegToRadF);
	const int16_t high = sim::quantize_pitch(p_pose.max_pitch_deg * core::kDegToRadF);
	if (low <= high) {
		out.pitch = std::clamp(out.pitch, low, high);
	}
	return out;
}

sim::Command Gunplay::clamp_pitch(uint8_t p_slot, const sim::Command &p_command, sim::Pose p_pose) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].armed) {
		return p_command;
	}
	const sw::SoldierWeaponsState &w = m.slots[p_slot].weapons;
	const uint16_t def = w.active < w.slots.size() ? w.slots[w.active].def : sw::kNoWeapon;
	if (def >= m.level->defs.size() || size_t(p_pose) >= gp::kSoldierPoseCount) {
		return p_command;
	}
	const gp::WeaponAiming &aiming = m.level->defs[def].aiming;
	return aiming.present ? clamp_command_pitch(p_command, aiming.poses[size_t(p_pose)]) : p_command;
}

const sw::SuppressionDef &Gunplay::suppression_def() const {
	return impl_->suppression_def;
}

sw::SuppressionState Gunplay::suppression(uint8_t p_slot) const {
	return p_slot < impl_->suppression.size() ? impl_->suppression[p_slot] : sw::SuppressionState();
}

uint16_t Gunplay::suppression_counter(uint8_t p_slot, uint32_t p_tick) const {
	return sw::suppression_counter(impl_->suppression_def, suppression(p_slot), p_tick);
}

bool Gunplay::suppressed(uint8_t p_slot, uint32_t p_tick) const {
	return sw::suppressed(impl_->suppression_def, suppression(p_slot), p_tick);
}

float Gunplay::suppression_scale(uint8_t p_slot, uint32_t p_tick) const {
	return sw::suppression_scale(impl_->suppression_def, suppression(p_slot), p_tick);
}

void Gunplay::suppress(uint32_t p_tick, std::span<const sim::SoldierState> p_states) {
	Impl &m = *impl_;
	if (!m.suppression_def.present) {
		return;
	}
	const auto key = [](uint32_t p_slot, uint32_t p_shot, uint8_t p_pellet) {
		return uint64_t(p_slot) << 40 | uint64_t(p_shot) << 8 | uint64_t(p_pellet);
	};
	const auto point = [](const sw::DVec3 &p_v) { return sim::Vec3{ float(p_v[0]), float(p_v[1]), float(p_v[2]) }; };
	const std::span<const sw::Projectile> live = m.projectiles->live();
	m.landed.clear();
	for (size_t i = 0; i < live.size(); ++i) {
		m.landed.emplace(key(live[i].player_slot, live[i].shot_index, live[i].pellet), i);
	}
	const bool update_end = p_tick % sw::kSuppressionUpdateTicks == 0;
	const size_t n = std::min(p_states.size(), m.slots.size());
	for (const sw::Projectile &b : m.flying) {
		if ((b.flags & sw::projectile_flags::kResting) != 0 || b.def >= m.table->defs.size()) {
			continue;
		}
		const sw::ProjectileSimDef &pd = m.table->def(b.def, b.variant).projectile;
		if (!(pd.suppression_min > 0.0f || pd.suppression_max > 0.0f)) {
			continue;
		}
		const uint64_t k = key(b.player_slot, b.shot_index, b.pellet);
		const auto start = m.update_start.try_emplace(k, point(b.position)).first;
		sim::Vec3 to{};
		bool ended = false;
		if (const auto it = m.landed.find(k); it != m.landed.end()) {
			to = point(live[it->second].position);
		} else {
			ended = true;
			bool found = false;
			for (const sw::ImpactEvent &i : m.step_out.impacts) {
				if (!found && i.player_slot == b.player_slot && i.shot_index == b.shot_index && i.pellet == b.pellet) {
					to = i.position;
					found = true;
				}
			}
			for (const sw::DetonationEvent &d : m.step_out.detonations) {
				if (!found && d.player_slot == b.player_slot && d.shot_index == b.shot_index) {
					to = d.position;
					found = true;
				}
			}
			if (!found) {
				m.update_start.erase(start);
				continue;
			}
		}
		if (!ended && !update_end) {
			continue;
		}
		const sim::Vec3 from = start->second;
		if (ended) {
			m.update_start.erase(start);
		} else {
			start->second = to;
		}
		const sim::Vec3 origin = point(b.origin);
		const uint8_t shooter_team = b.player_slot < m.teams.size() ? m.teams[b.player_slot] : uint8_t(0);
		const float boost = m.boost_at_spawn(b.player_slot, b.shot_index,
				effects(uint8_t(std::min<uint32_t>(b.player_slot, 255u)))[gp::SpecializationTarget::SuppressionBoost]);
		for (size_t v = 0; v < n; ++v) {
			if (v == b.player_slot || !m.slots[v].armed || !alive(p_states[v]) ||
					(shooter_team != 0 && m.teams[v] == shooter_team)) {
				continue;
			}
			const sim::SoldierState &st = p_states[v];
			const sim::Vec3 centre{ st.position[0],
				st.position[1] + 0.5f * m.shared->motor.poses[size_t(st.pose)].height, st.position[2] };
			const std::optional<sw::NearMiss> miss =
					sw::near_miss(from, to, origin, centre, m.suppression_def.sphere_radius);
			if (!miss) {
				continue;
			}
			const bool hit =
					std::any_of(m.step_out.impacts.begin(), m.step_out.impacts.end(), [&](const sw::ImpactEvent &i) {
						return i.kind == sw::ImpactKind::Target && i.target == v && i.player_slot == b.player_slot &&
								i.shot_index == b.shot_index && i.pellet == b.pellet;
					});
			if (hit) {
				continue;
			}
			const uint32_t add = sw::near_miss_add(sw::near_miss_amount(pd, m.suppression_def, *miss),
					effects(uint8_t(v))[gp::SpecializationTarget::SuppressionResist], boost);
			if (add == 0) {
				continue;
			}
			m.suppression[v] = sw::add_suppression(m.suppression_def, m.suppression[v], p_tick, add);
			++m.stats.near_misses;
		}
	}
	if (update_end) {
		std::erase_if(m.update_start, [&](const auto &p_entry) { return !m.landed.contains(p_entry.first); });
	}
}

float Gunplay::received_damage(uint8_t p_victim, const sw::DamageEvent &p_event) const {
	if (p_event.kind != sw::DamageKind::Blast && p_event.kind != sw::DamageKind::DudBlast) {
		return p_event.amount;
	}
	return p_event.amount * effects(p_victim)[gp::SpecializationTarget::ExplosionDamage];
}

void Gunplay::on_spawn(uint8_t p_slot, uint8_t p_kit, uint8_t p_team, uint32_t p_tick) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size()) {
		return;
	}
	m.clear(p_slot);
	Impl::Slot &s = m.slots[p_slot];
	m.suppression[p_slot] = sw::SuppressionState();
	m.teams[p_slot] = p_team;
	s.team = p_team;
	s.kit = p_kit < kKitCount ? p_kit : 0;
	if (p_team < 1 || p_team > kKitTeams) {
		return;
	}
	const KitLoadout &l = m.level->kits.loadout(p_team, s.kit);
	s.loadout = default_loadout(m.level->loadouts, m.level->kits, p_team, s.kit);
	if (const std::optional<LoadoutChoice> &choice = s.choices[p_team - 1][s.kit]) {
		if (core::Result<Loadout> chosen = resolve_loadout(m.level->loadouts, m.level->kits, p_team, s.kit, *choice)) {
			s.loadout = std::move(*chosen);
		} else {
			++m.stats.loadout_dropped;
		}
	}
	if (!sw::spawn_soldier_weapons(s.weapons, *m.table, s.loadout.defs, l.active, p_tick, s.loadout.variants).ok()) {
		return;
	}
	s.specializations = specialization_mask(p_slot);
	apply_spawn_effects(
			s.weapons, *m.table, gp::specialization_effects_of(m.level->specializations, s.specializations));
	m.gadgets->armed(p_slot, s.weapons);
	s.armed = true;
	s.spawn_tick = p_tick;
	s.fired = false;
	s.firing = false;
	s.out = sw::WeaponTickOutput();
}

void Gunplay::step_soldier(uint8_t p_slot, const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size()) {
		return;
	}
	Impl::Slot &s = m.slots[p_slot];
	if (alive(p_before) && !alive(p_after)) {
		m.pending_fall_deaths.push_back(p_slot);
	}
	if (!s.armed) {
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	if (p_command.tick != s.weapons.tick + 1u) {
		++m.stats.weapon_precondition_failures;
	}
	const uint32_t slot_field = (p_command.buttons & sw::weapon_buttons::kSlotMask) >> sw::weapon_buttons::kSlotShift;
	if (slot_field > sw::kMaxWeaponSlots) {
		++m.stats.slot_field_clamped;
	}
	const sw::WeaponContext ctx = sw::make_weapon_context(
			m.shared->motor, p_before, p_after, p_command, suppression_scale(p_slot, p_command.tick));
	sw::step_weapons_with_rules(*m.table, s.weapons, p_command, ctx, m.match_seed, s.spawn_tick, s.out);
	s.out_tick = p_command.tick;
	m.gadgets->detonator(p_slot, s.weapons.current().def, (p_command.buttons & sim::buttons::kZoom) != 0);
	if ((s.out.events & sw::weapon_events::kSwitchRefused) != 0) {
		++m.stats.slot_select_empty;
	}
	if ((s.out.events & sw::weapon_events::kFireModeChanged) != 0) {
		++m.stats.fire_mode_changes;
	}
	if ((s.out.events & sw::weapon_events::kReloadStarted) != 0) {
		++s.reload_count;
	}
	if (s.out.shot_count > 0) {
		if (!s.shot_last_tick) {
			++s.burst_count;
		}
		s.firing = true;
		s.fired = true;

		bool have_view = false;
		bool refused = false;
		ShotView view;
		if (p_command.view_quality != uint8_t(sim::ViewQuality::None)) {
			const Impl::AckEntry &a = s.acks[p_command.tick % kAckRing];
			if (a.valid && a.tick == p_command.tick) {
				have_view = m.history->shot_view(p_slot, a.ack, p_command.view_seq8, int32_t(p_command.view_back_q8), p_command.view_quality, view);
				refused = !have_view;
				m.stats.view_refused += refused ? s.out.shot_count : 0u;
			} else {
				refused = true;
				m.stats.no_ack_state += s.out.shot_count;
			}
		} else {
			view.quality = uint8_t(sim::ViewQuality::None);
			have_view = true;
		}
		for (uint8_t i = 0; i < s.out.shot_count; ++i) {
			const sw::ShotEvent &e = s.out.shots[i];
			if (m.gadgets->is_gadget(e.def)) {
				m.gadgets->deploy(e, s.team, p_command.tick, m.world);
			} else if (!m.projectiles->spawn(e).ok()) {
				++m.stats.projectiles_refused;
			} else {
				m.shot_boost[p_slot][e.shot_index % kShotBoostRing] =
						ShotBoost{ e.shot_index, effects(p_slot)[gp::SpecializationTarget::SuppressionBoost], true };
			}
			if (refused) {
				m.rewind->note_refused(p_slot, e.shot_index);
			} else if (have_view) {
				m.rewind->note_shot(p_slot, e.shot_index, view);
			}
			m.interval_shots.push_back(e);
			++m.stats.shots;
		}
	}
	s.shot_last_tick = s.out.shot_count > 0;
	if (m.config.observer) {
		m.config.observer->weapon_tick(p_slot, p_command, s.weapons, s.out);
	}
	m.weapons_acc_ms += ms_since(start);
}

void Gunplay::step_projectiles(uint32_t p_tick, std::span<const sim::SoldierState> p_states) {
	Impl &m = *impl_;
	m.stats.weapons_ms = m.weapons_acc_ms;
	m.stats.tick_weapons_ms_max = std::max(m.stats.tick_weapons_ms_max, m.weapons_acc_ms);
	m.weapons_acc_ms = 0;
	const auto start = std::chrono::steady_clock::now();
	m.step_out.clear();
	m.step_tick = p_tick;
	if (m.world == nullptr) {
		return;
	}
	m.rewind->begin_tick(p_tick, p_states, std::span<const uint8_t>(m.teams.data(), std::min(p_states.size(), m.teams.size())));
	m.flying.assign(m.projectiles->live().begin(), m.projectiles->live().end());
	const sim::vehicles::JoinedTargets targets(*m.rewind, m.vehicle_targets);
	m.projectiles->step(p_tick, *m.world, targets, m.step_out);
	m.gadgets->detonate_packs(p_tick, p_states, m.teams, m.pack_vehicles, *m.world, targets, m.step_out);
	m.rewind->end_tick();
	suppress(p_tick, p_states);
	m.stats.budget_exhausted += m.step_out.budget_exhausted ? 1u : 0u;
	m.stats.live_projectiles = uint32_t(m.projectiles->live().size());
	m.stats.rewind_ms = m.rewind->last_tick_ms();
	m.stats.projectiles_ms = ms_since(start);
	m.stats.lag = m.rewind->stats();
	if (m.config.observer) {
		m.config.observer->projectiles(p_tick, m.step_out, m.projectiles->live());
		if (!m.rewind_rows.empty()) {
			m.config.observer->rewind_rows(p_tick, m.rewind_rows);
			m.rewind_rows.clear();
		}
	}
}

void Gunplay::add_damage(std::span<const sw::DamageEvent> p_events) {
	impl_->step_out.damage.insert(impl_->step_out.damage.end(), p_events.begin(), p_events.end());
}

void Gunplay::set_pack_vehicles(std::span<const sw::PackVehicle> p_vehicles) {
	impl_->pack_vehicles = p_vehicles;
}

void Gunplay::set_vehicle_targets(std::span<const sim::vehicles::VehicleTarget> p_targets) {
	impl_->vehicle_targets = p_targets;
}

void Gunplay::take_damage_if(
		const std::function<bool(const sw::DamageEvent &)> &p_take, std::vector<sw::DamageEvent> &r_taken) {
	std::vector<sw::DamageEvent> &damage = impl_->step_out.damage;
	const auto kept =
			std::stable_partition(damage.begin(), damage.end(), [&](const sw::DamageEvent &e) { return !p_take(e); });
	r_taken.insert(r_taken.end(), kept, damage.end());
	damage.erase(kept, damage.end());
}

const sw::ProjectileStepOutput &Gunplay::step_output() const {
	return impl_->step_out;
}

void Gunplay::apply_rules(uint32_t p_tick, std::span<sim::SoldierState> p_states, CombatEventSink &r_sink) {
	Impl &m = *impl_;
	m.tick_damage = m.step_out.damage;
	m.tick_deaths.clear();
	m.tick_detonations = m.step_out.detonations;
	if (m.step_tick != p_tick) {
		m.tick_damage.clear();
		m.tick_detonations.clear();
	}
	std::sort(m.tick_damage.begin(), m.tick_damage.end(), [](const sw::DamageEvent &a, const sw::DamageEvent &b) {
		if (a.time != b.time) {
			return a.time < b.time;
		}
		if (a.attacker_slot != b.attacker_slot) {
			return a.attacker_slot < b.attacker_slot;
		}
		if (a.shot_index != b.shot_index) {
			return a.shot_index < b.shot_index;
		}
		if (a.pellet != b.pellet) {
			return a.pellet < b.pellet;
		}
		return a.target < b.target;
	});
	for (const sw::DamageEvent &e : m.tick_damage) {
		const uint32_t victim = e.target;
		bool applied = false;
		float health_after = 0;
		if (victim >= p_states.size() || victim >= m.slots.size()) {
			++m.stats.damage_dropped;
		} else if (!alive(p_states[victim])) {
			++m.stats.dead_victim_dropped;
			++m.stats.damage_dropped;
		} else if (!m.config.friendly_fire && e.attacker_slot != victim && e.attacker_slot < m.teams.size() && m.teams[e.attacker_slot] != 0 &&
				m.teams[e.attacker_slot] == m.teams[victim]) {
			++m.stats.team_damage_dropped;
			++m.stats.damage_dropped;
		} else {
			sim::SoldierState &st = p_states[victim];
			sw::Mechanics mech = m.table->mechanics;
			if (m.slots[victim].fired) {
				mech.spawn_protection_blocks = false;
			}
			const float amount = received_damage(uint8_t(victim), e);
			applied = sw::apply_damage(st, amount, mech);
			health_after = st.health;
			if (!applied) {
				++m.stats.protected_refused;
				++m.stats.damage_dropped;
			} else {
				++m.stats.hits;
				net::HitEvent h;
				h.victim = uint8_t(victim);
				h.shooter = uint8_t(std::min<uint32_t>(e.attacker_slot, 255u));
				h.shot_index = e.shot_index;
				h.pellet = e.pellet;
				h.zone = e.zone;
				h.damage = amount;
				h.health_after = st.health;
				h.time = e.time;
				h.position = st.position;
				h.normal = { 0.0f, 1.0f, 0.0f };
				for (const sw::ImpactEvent &i : m.step_out.impacts) {
					if (i.kind == sw::ImpactKind::Target && i.target == victim && i.player_slot == e.attacker_slot && i.shot_index == e.shot_index &&
							i.pellet == e.pellet) {
						h.position = i.position;
						h.normal = i.normal;
						break;
					}
				}
				if (e.kind == sw::DamageKind::Blast || e.kind == sw::DamageKind::DudBlast) {
					for (const sw::DetonationEvent &d : m.step_out.detonations) {
						if (d.player_slot == e.attacker_slot && d.shot_index == e.shot_index) {
							h.position = d.position;
							h.normal = d.normal;
							break;
						}
					}
				}
				r_sink.hit(h);
				if (!alive(st)) {
					net::DeathEvent d;
					d.victim = uint8_t(victim);
					d.killer = m.gadgets->ownerless(e.shot_index) ? uint8_t(victim)
																  : uint8_t(std::min<uint32_t>(e.attacker_slot, 255u));
					d.def = e.def;
					d.zone = e.zone;
					d.shot_index = e.shot_index;
					d.headshot = e.zone == sw::HitZone::Head && e.kind == sw::DamageKind::Bullet;
					d.time = e.time;
					++m.stats.deaths;
					m.clear(uint8_t(victim));
					m.gadgets->owner_died(uint8_t(victim));
					m.tick_deaths.push_back(d);
					r_sink.death(d);
				}
			}
		}
		if (m.config.observer) {
			m.config.observer->damage(p_tick, e, applied, health_after);
		}
	}
	for (const uint8_t slot : m.pending_fall_deaths) {
		net::DeathEvent d;
		d.victim = slot;
		d.killer = slot;
		d.def = sw::kNoWeapon;
		d.time = sw::tick_end(p_tick);
		++m.stats.deaths;
		m.clear(slot);
		m.gadgets->owner_died(slot);
		m.tick_deaths.push_back(d);
		r_sink.death(d);
	}
	m.pending_fall_deaths.clear();
	for (const sw::DetonationEvent &d : m.tick_detonations) {
		net::DetonationWire w;
		w.shooter = uint8_t(std::min<uint32_t>(d.player_slot, 255u));
		w.shot_index = d.shot_index;
		w.def = d.def;
		w.time = d.time;
		w.position = d.position;
		w.normal = d.normal;
		w.dud = d.dud;
		r_sink.detonation(w);
	}
	m.gadget_soldiers.assign(std::min(p_states.size(), m.slots.size()), GadgetSoldier());
	for (size_t i = 0; i < m.gadget_soldiers.size(); ++i) {
		if (m.slots[i].armed && alive(p_states[i])) {
			m.gadget_soldiers[i] = GadgetSoldier{ &p_states[i], &m.slots[i].weapons, m.teams[i] };
		}
	}
	m.gadgets->blast(m.tick_detonations, m.world, m.teams, m.config.friendly_fire);
	m.gadgets->step(p_tick, m.world, m.gadget_soldiers, m.level->max_health);
}

const sw::SoldierWeaponsState &Gunplay::weapons(uint8_t p_slot) const {
	const Impl &m = *impl_;
	return p_slot < m.slots.size() ? m.slots[p_slot].weapons : m.empty_state;
}

uint64_t Gunplay::weapon_hash() const {
	return impl_->weapon_hash;
}

uint64_t Gunplay::match_seed() const {
	return impl_->match_seed;
}

uint8_t Gunplay::weapon_flags() const {
	return impl_->flags;
}

const GunplayStats &Gunplay::stats() const {
	return impl_->stats;
}

const GunplayConfig &Gunplay::config() const {
	return impl_->config;
}

const GunplayLevel &Gunplay::level() const {
	return *impl_->level;
}

const sw::WeaponTable &Gunplay::table() const {
	return *impl_->table;
}

uint64_t Gunplay::kit_hash() const {
	return impl_->level->kits.hash;
}

LagHistory &Gunplay::history() {
	return *impl_->history;
}

const RewindProvider &Gunplay::rewind() const {
	return *impl_->rewind;
}

void Gunplay::clear(uint8_t p_slot) {
	if (p_slot < impl_->slots.size()) {
		impl_->clear(p_slot);
		impl_->teams[p_slot] = 0;
		impl_->gadgets->forget(p_slot);
	}
}

void Gunplay::left(uint8_t p_slot) {
	if (p_slot < impl_->slots.size()) {
		impl_->clear(p_slot);
		impl_->teams[p_slot] = 0;
		impl_->gadgets->owner_left(p_slot);
	}
}

void Gunplay::clear_deployables() {
	impl_->gadgets->clear();
}

std::span<const sw::Deployable> Gunplay::deployables() const {
	return impl_->gadgets->live();
}

uint64_t Gunplay::deployables_revision() const {
	return impl_->gadgets->revision();
}

const GadgetStats &Gunplay::gadget_stats() const {
	return impl_->gadgets->stats();
}

std::optional<sim::Vec3> Gunplay::squad_beacon(uint8_t p_slot, uint8_t p_team) const {
	return impl_->gadgets->squad_beacon(p_slot, p_team, impl_->squads);
}

bool Gunplay::armed(uint8_t p_slot) const {
	return p_slot < impl_->slots.size() && impl_->slots[p_slot].armed;
}

uint8_t Gunplay::team(uint8_t p_slot) const {
	return p_slot < impl_->slots.size() ? impl_->slots[p_slot].team : uint8_t(0);
}

bool Gunplay::select_loadout(uint8_t p_slot, uint8_t p_team, uint8_t p_kit, const LoadoutChoice &p_choice) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size() || p_team < 1 || p_team > kKitTeams || p_kit >= kKitCount ||
			!resolve_loadout(m.level->loadouts, m.level->kits, p_team, p_kit, p_choice)) {
		++m.stats.loadout_refused;
		return false;
	}
	m.slots[p_slot].choices[p_team - 1][p_kit] = p_choice;
	++m.stats.loadout_select;
	return true;
}

void Gunplay::refuse_loadout() {
	++impl_->stats.loadout_refused;
}

void Gunplay::forget_loadouts(uint8_t p_slot) {
	if (p_slot < impl_->slots.size()) {
		impl_->slots[p_slot].choices = {};
		impl_->slots[p_slot].loadout = Loadout();
	}
}

const Loadout &Gunplay::loadout(uint8_t p_slot) const {
	static const Loadout kNone;
	return p_slot < impl_->slots.size() ? impl_->slots[p_slot].loadout : kNone;
}

LoadoutChoices Gunplay::loadout_choices(uint8_t p_slot) const {
	return p_slot < impl_->slots.size() ? impl_->slots[p_slot].choices : LoadoutChoices{};
}

uint8_t Gunplay::kit(uint8_t p_slot) const {
	return p_slot < impl_->slots.size() ? impl_->slots[p_slot].kit : uint8_t(0);
}

void Gunplay::note_command(uint8_t p_slot, const sim::Command &p_command, const AckState &p_ack) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size()) {
		return;
	}
	Impl::AckEntry &a = m.slots[p_slot].acks[p_command.tick % kAckRing];
	a.valid = true;
	a.tick = p_command.tick;
	a.ack = p_ack;
}

net::OtherSoldier Gunplay::other_record(uint8_t p_slot, const sim::SoldierState &p_state) const {
	const Impl &m = *impl_;
	net::OtherSoldier o = net::quantize_other(p_slot, p_state);
	if (p_slot < m.slots.size() && m.slots[p_slot].armed) {
		const Impl::Slot &s = m.slots[p_slot];
		const sw::WeaponSlotState &w = s.weapons.current();
		net::set_other_weapon(o, w.def, w.phase, s.firing, (w.flags & sw::slot_flags::kZoomWasDown) != 0, s.reload_count, s.burst_count);
	}
	net::set_other_team(o, p_slot < m.slots.size() ? m.slots[p_slot].team : uint8_t(0));
	return o;
}

void Gunplay::end_snapshot_interval() {
	Impl &m = *impl_;
	m.interval_shots.clear();
	for (Impl::Slot &s : m.slots) {
		s.firing = false;
	}
}

std::span<const sw::ShotEvent> Gunplay::interval_shots() const {
	return impl_->interval_shots;
}

net::TraceGunplayTick Gunplay::trace_tick(uint8_t p_slot, uint32_t p_tick, uint32_t &r_dropped) const {
	const Impl &m = *impl_;
	net::TraceGunplayTick t;
	t.tick = p_tick;
	if (p_slot >= m.slots.size()) {
		return t;
	}
	const Impl::Slot &s = m.slots[p_slot];
	if (s.out_tick == p_tick) {
		for (uint8_t i = 0; i < s.out.shot_count; ++i) {
			if (t.shots.size() < net::kMaxTraceShots) {
				t.shots.push_back(s.out.shots[i]);
			} else {
				++r_dropped;
			}
		}
	}
	for (const sw::Projectile &p : m.projectiles->live()) {
		if (p.player_slot != p_slot) {
			continue;
		}
		if (t.projectiles.size() < net::kMaxTraceProjectiles) {
			t.projectiles.push_back({ float(p.position[0]), float(p.position[1]), float(p.position[2]) });
			t.projectile_shots.push_back(p.shot_index);
		} else {
			++r_dropped;
		}
	}
	if (m.step_tick == p_tick) {
		for (const sw::DamageEvent &e : m.tick_damage) {
			if (e.attacker_slot != p_slot && e.target != p_slot) {
				continue;
			}
			if (t.damage.size() < net::kMaxTraceDamage) {
				t.damage.push_back(e);
			} else {
				++r_dropped;
			}
		}
		for (const sw::DetonationEvent &d : m.tick_detonations) {
			if (d.player_slot != p_slot) {
				continue;
			}
			if (t.detonations.size() < net::kMaxTraceDetonations) {
				t.detonations.push_back(d);
			} else {
				++r_dropped;
			}
		}
		for (const net::DeathEvent &d : m.tick_deaths) {
			if (d.victim != p_slot && d.killer != p_slot) {
				continue;
			}
			if (t.deaths.size() < net::kMaxTraceDeaths) {
				t.deaths.push_back({ d.victim, d.killer, d.shot_index });
			} else {
				++r_dropped;
			}
		}
	}
	return t;
}

std::optional<net::TargetInfoRow> Gunplay::target_info(uint8_t p_slot, const sim::SoldierState &p_state) const {
	const sw::HitTarget *t = impl_->rewind->present_target(p_slot);
	if (t == nullptr) {
		return std::nullopt;
	}
	net::TargetInfoRow r;
	r.slot = p_slot;
	r.pose = p_state.pose;
	r.feet = p_state.position;
	r.yaw = p_state.yaw;
	r.target = *t;
	return r;
}

void Gunplay::count_kit_select(bool p_ok, bool p_team_refused) {
	++(p_ok ? impl_->stats.kit_select : impl_->stats.kit_select_refused);
	impl_->stats.team_change_refused += p_team_refused ? 1u : 0u;
}

void Gunplay::count_others_dropped(size_t p_count) {
	impl_->stats.others_dropped += p_count;
}

void Gunplay::count_shot_events_dropped(size_t p_count) {
	impl_->stats.shot_events_dropped += p_count;
}

void Gunplay::count_combat_clamped(size_t p_count) {
	impl_->stats.combat_events_clamped += p_count;
}

void Gunplay::count_trace_dropped(uint32_t p_count) {
	impl_->stats.trace_dropped += p_count;
}

net::ShotQuantization shot_frame(const sim::StaticWorldShapes &p_world) {
	net::ShotQuantization q;
	const sim::Vec3 lo = p_world.min();
	for (size_t k = 0; k < 3; ++k) {
		q.origin_min[k] = std::isfinite(lo[k]) ? std::floor(lo[k]) - kShotFrameMargin : 0.0f;
	}
	return q;
}

}

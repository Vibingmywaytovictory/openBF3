#pragma once

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/host/kits.h"
#include "bf3/host/lag_comp.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/mechanics_tuning.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::core {
class Install;
}

namespace bf3::host {

class GunplayObserver {
public:
	virtual ~GunplayObserver() = default;

	virtual void weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sim::weapons::SoldierWeaponsState &p_after,
			const sim::weapons::WeaponTickOutput &p_out) {
		(void)p_slot, (void)p_command, (void)p_after, (void)p_out;
	}

	virtual void projectiles(uint32_t p_tick, const sim::weapons::ProjectileStepOutput &p_out, std::span<const sim::weapons::Projectile> p_live) {
		(void)p_tick, (void)p_out, (void)p_live;
	}

	virtual void damage(uint32_t p_tick, const sim::weapons::DamageEvent &p_event, bool p_applied, float p_health_after) {
		(void)p_tick, (void)p_event, (void)p_applied, (void)p_health_after;
	}

	virtual bool wants_rewind_rows() const { return false; }
	virtual void rewind_rows(uint32_t p_tick, std::span<const RewindTraceRow> p_rows) { (void)p_tick, (void)p_rows; }
};

struct GunplayConfig {
	std::string mechanics_cfg = std::string(sim::weapons::kMechanicsTuningPath);
	uint64_t match_seed = 0;
	LagCompConfig lag;

	bool exact_aim = false;
	bool infinite_magazine = false;

	bool friendly_fire = false;

	uint32_t target_bots = 0;
	uint8_t target_team = 2;

	uint8_t player_kit = 0;
	uint8_t player_team = 0;
	std::shared_ptr<GunplayObserver> observer;

	uint16_t defib_def = sim::weapons::kNoWeapon;
};

struct GunplayStats {
	double weapons_ms = 0, projectiles_ms = 0, rewind_ms = 0;
	uint64_t shots = 0, hits = 0, deaths = 0, damage_dropped = 0;
	uint64_t weapon_precondition_failures = 0;
	LagCompStats lag;

	uint64_t slot_field_clamped = 0;
	uint64_t slot_select_empty = 0;
	uint64_t fire_mode_changes = 0;
	uint64_t view_refused = 0;
	uint64_t no_ack_state = 0;
	uint64_t team_damage_dropped = 0;
	uint64_t dead_victim_dropped = 0;
	uint64_t protected_refused = 0;
	uint64_t blast_protected = 0;
	uint64_t projectiles_refused = 0;
	uint64_t budget_exhausted = 0;
	uint64_t kit_select = 0, kit_select_refused = 0, team_change_refused = 0;
	uint64_t shot_events_dropped = 0, combat_events_clamped = 0, others_dropped = 0;
	uint64_t trace_dropped = 0;
	uint32_t live_projectiles = 0;
	double tick_weapons_ms_max = 0;
};
static_assert(std::is_trivially_copyable_v<GunplayStats>);

class CombatEventSink {
public:
	virtual ~CombatEventSink() = default;
	virtual void hit(const net::HitEvent &p_event) = 0;
	virtual void death(const net::DeathEvent &p_event) = 0;
	virtual void detonation(const net::DetonationWire &p_event) = 0;

	virtual void revive(const net::ReviveEvent &p_event) { (void)p_event; }
};

class RespawnPolicy {
public:
	virtual ~RespawnPolicy() = default;
	virtual std::optional<uint32_t> respawn_tick(uint8_t p_slot, uint32_t p_death_tick) = 0;
};

struct GunplayLevel {
	std::vector<core::gameplay::WeaponDef> defs;
	std::shared_ptr<const sim::weapons::SurfaceMaterialTable> materials;
	core::gameplay::SoldierDef soldier;
	std::vector<core::gameplay::HitCapsule> capsules;
	core::gameplay::CapsulePoseTable capsule_table;
	bool capsule_standin = false;
	std::vector<std::string> notes;
	KitTable kits;
	float sprint_recover_seconds = 0.12f;
	float max_health = 100.0f;
	uint64_t level_hash = 0;
	double seconds_weapons = 0, seconds_kits = 0, seconds_capsules = 0;
};

uint64_t gunplay_level_hash(std::string_view p_level, std::string_view p_mode);

uint64_t derive_match_seed(uint64_t p_server_seed, uint64_t p_level_hash);

core::Result<std::shared_ptr<const GunplayLevel>> load_gunplay_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level_id,
		std::string_view p_mode);

struct GunplayLoadOptions {
	bool tdm_kits = false;
};
core::Result<std::shared_ptr<const GunplayLevel>> load_gunplay_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level_id,
		std::string_view p_mode, const GunplayLoadOptions &p_options);

core::Result<core::gameplay::CapsulePoseTable> standin_capsule_table(const core::gameplay::SoldierDef &p_soldier,
		std::span<const core::gameplay::HitCapsule> p_capsules, const sim::weapons::SurfaceMaterialTable &p_materials);

core::Result<sim::weapons::Mechanics> load_mechanics_cfg(const std::string &p_path);

core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> make_match_table(const GunplayLevel &p_level, const sim::weapons::Mechanics &p_mechanics,
		bool p_exact_aim, bool p_infinite_magazine);

uint64_t match_weapon_hash(const sim::weapons::WeaponTable &p_table, const core::gameplay::CapsulePoseTable &p_capsules);
uint8_t match_weapon_flags(bool p_exact_aim, bool p_infinite_magazine);

inline constexpr float kShotFrameMargin = 64.0f;
net::ShotQuantization shot_frame(const sim::StaticWorldShapes &p_world);

class Gunplay {
public:
	static core::Result<std::unique_ptr<Gunplay>> create(const SharedLevel &p_level, const GunplayConfig &p_config);
	~Gunplay();
	Gunplay(const Gunplay &) = delete;
	Gunplay &operator=(const Gunplay &) = delete;

	void on_spawn(uint8_t p_slot, uint8_t p_kit, uint8_t p_team, uint32_t p_tick);
	void step_soldier(uint8_t p_slot, const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after);
	void step_projectiles(uint32_t p_tick, std::span<const sim::SoldierState> p_states);
	void apply_rules(uint32_t p_tick, std::span<sim::SoldierState> p_states, CombatEventSink &r_sink);
	const sim::weapons::SoldierWeaponsState &weapons(uint8_t p_slot) const;
	uint64_t weapon_hash() const;
	uint64_t match_seed() const;
	uint8_t weapon_flags() const;
	const GunplayStats &stats() const;

	const GunplayConfig &config() const;
	const GunplayLevel &level() const;
	const sim::weapons::WeaponTable &table() const;
	uint64_t kit_hash() const;
	LagHistory &history();
	const RewindProvider &rewind() const;

	void clear(uint8_t p_slot);

	void set_world(const sim::CollisionWorld &p_world);
	void set_match_seed(uint64_t p_seed);
	bool armed(uint8_t p_slot) const;
	uint8_t team(uint8_t p_slot) const;
	uint8_t kit(uint8_t p_slot) const;

	void note_command(uint8_t p_slot, const sim::Command &p_command, const AckState &p_ack);

	net::OtherSoldier other_record(uint8_t p_slot, const sim::SoldierState &p_state) const;

	void end_snapshot_interval();

	std::span<const sim::weapons::ShotEvent> interval_shots() const;

	net::TraceGunplayTick trace_tick(uint8_t p_slot, uint32_t p_tick, uint32_t &r_dropped) const;

	std::optional<net::TargetInfoRow> target_info(uint8_t p_slot, const sim::SoldierState &p_state) const;
	void count_kit_select(bool p_ok, bool p_team_refused);
	void count_others_dropped(size_t p_count);
	void count_shot_events_dropped(size_t p_count);
	void count_combat_clamped(size_t p_count);
	void count_trace_dropped(uint32_t p_count);

private:
	Gunplay();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}

#pragma once
// Gunplay inside the GameServer (docs/design/m2a.md §4; owner sv): per-slot weapons, projectiles, damage, deaths and
// kits. Owned by GameServer, server thread only; linked by the dedicated server too (never by bf3_fx).
//
// Tick order (extends m1.md §8.1; GameServer calls the entry points at these steps):
//   4a  per live soldier, inside M1's per-soldier loop right after the motor step (sv's named insertion):
//       step_soldier(slot, cmd, before, after): make_weapon_context + sim::weapons::step_weapons_m2a; each shot ->
//       projectiles.spawn, the command's view sample stored in the shooter's shot-view ring under the shot index,
//       the shot appended to every client's shot accumulator. A soldier dead before this tick steps no weapon; its
//       projectiles in flight keep flying.
//   5   step_projectiles(tick, states): rewind.begin_tick, then ProjectileWorld::step once for all projectiles.
//   6   apply_rules(tick, states, sink): DamageEvents sorted by (time, shooter slot, shot index, pellet) and applied
//       with apply_damage; a hit on a soldier already dead in this order is dropped; Hit, Death and Detonation
//       events go to the CombatEventSink; a death asks the RespawnPolicy; (re)spawns call on_spawn with the slot's
//       kit and team.
// Deaths leave the server only through CombatEventSink, and the respawn decision goes through RespawnPolicy, so
// M2b plugs the S4 interpreter into both seams without touching this code (m2a.md §0).
//
// As built (sv, docs/design/m2a-sv.md):
//   * The level side is GunplayLevel (SharedLevel::gunplay, loaded by load_shared_level when
//     SharedLoadOptions::gunplay is set): the match's WeaponDefs (index = def id), the SurfaceMaterialTable, the
//     soldier, its hit capsules, the capsule pose table (lc's builder on the retail clips; per row a stand-in when a
//     clip is unusable; a whole stand-in table when the Ant library does not open) and the kit table. Both ends build
//     their WeaponTable from it with make_match_table and compare match_weapon_hash, so a client that builds its own
//     table from the same level and cfg gets the server's hash exactly.
//   * Spawn protection: wp's apply_damage refuses damage to a kSpawnProtected soldier. M2a's rule "until it ends or
//     the soldier fires" is applied without touching the SoldierState (which the client predicts): a soldier that has
//     fired in its life is damaged as if unprotected (apply_damage with spawn_protection_blocks off for it). Blasts
//     are tested by the provider with HitTarget::spawn_protected (protect_ticks > 0), so a protected soldier that
//     fired is still spared by a blast until protect_ticks runs out (counted as blast_protected; reported).
//   * Friendly fire is off by default (GunplayConfig::friendly_fire): a DamageEvent whose attacker and victim share a
//     team (1 or 2) is dropped and counted (team_damage_dropped). A shooter never damages itself with a bullet; a blast
//     does (BF3's own grenade).

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

// Server-thread callbacks for tests and tools (dev only; null in the game). Every call happens inside
// GameServer::tick on the server thread; an implementation that is read from another thread locks itself.
class GunplayObserver {
public:
	virtual ~GunplayObserver() = default;
	// One weapon step of a live soldier (after step_weapons_m2a): the command, the state after and the tick's output
	// (shots and the timed phase events at their exact times).
	virtual void weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sim::weapons::SoldierWeaponsState &p_after,
			const sim::weapons::WeaponTickOutput &p_out) {
		(void)p_slot, (void)p_command, (void)p_after, (void)p_out;
	}
	// One ProjectileWorld step (impacts incl. Expired, damage events, detonations) and the live projectiles after it.
	virtual void projectiles(uint32_t p_tick, const sim::weapons::ProjectileStepOutput &p_out, std::span<const sim::weapons::Projectile> p_live) {
		(void)p_tick, (void)p_out, (void)p_live;
	}
	// One damage event after the rules decided it: applied (health changed) or why not.
	virtual void damage(uint32_t p_tick, const sim::weapons::DamageEvent &p_event, bool p_applied, float p_health_after) {
		(void)p_tick, (void)p_event, (void)p_applied, (void)p_health_after;
	}
};

struct GunplayConfig {
	std::string mechanics_cfg = std::string(sim::weapons::kMechanicsTuningPath); // §5.5, parsed on both ends
	uint64_t match_seed = 0; // 0: splitmix64(GameServerConfig::seed ^ level hash) (§4.4)
	LagCompConfig lag; // §6
	// Match options, fixed at match start (dev_commands only), sent in MatchInfo::weapon_flags (§7.1) so the client
	// builds the identical table; both are WeaponTableOptions fields and therefore in weapon_hash.
	bool exact_aim = false; // ShotEvent direction = view without recoil, pellet_cone 0, as the range (E3, E4, E12)
	bool infinite_magazine = false; // wp's oracle option
	// sv additions (as built).
	bool friendly_fire = false; // false: damage between soldiers of one team (1 or 2) is dropped and counted
	// Stationary target bots (§10.1, dev): added in GameServer::init before the walkers and bots, so they take slots
	// 0 .. target_bots - 1; teams target_team (0: alternate 2 / 1 starting with 2), kit = slot % 4.
	uint32_t target_bots = 0;
	uint8_t target_team = 2;
	// The kit and team a client spawns with until its KitSelect (dev: a test client that sends none). Team 0: M1's 1.
	uint8_t player_kit = 0;
	uint8_t player_team = 0;
	std::shared_ptr<GunplayObserver> observer; // dev / tests
};

struct GunplayStats {
	double weapons_ms = 0, projectiles_ms = 0, rewind_ms = 0; // the last tick, wall ms on the server thread
	uint64_t shots = 0, hits = 0, deaths = 0, damage_dropped = 0; // since init
	uint64_t weapon_precondition_failures = 0; // weapon steps whose command tick != state tick + 1 (must stay 0)
	LagCompStats lag;
	// sv additions (as built): what the server clamped or refused, per kind (E17), and other counters.
	uint64_t slot_field_clamped = 0; // a wanted-slot field 9..15 (no such slot: keep the current one)
	uint64_t slot_select_empty = 0; // a wanted slot that holds no weapon (wp refuses the switch)
	uint64_t fire_mode_changes = 0; // fire-mode steps taken (counter spam shows here, bounded by 3 per command)
	uint64_t view_refused = 0; // shots whose view sample LagHistory::shot_view refused (tested at the present)
	uint64_t no_ack_state = 0; // shots of a client command without a stored arrival ack state (tested at the present)
	uint64_t team_damage_dropped = 0;
	uint64_t dead_victim_dropped = 0; // a hit on a soldier already dead in this tick's order
	uint64_t protected_refused = 0; // apply_damage refused (spawn protection)
	uint64_t blast_protected = 0; // reserved: blasts the provider spared (not separable from wp's step; 0)
	uint64_t projectiles_refused = 0; // ProjectileWorld::spawn refusals
	uint64_t budget_exhausted = 0; // projectile steps cut by the work budget
	uint64_t kit_select = 0, kit_select_refused = 0, team_change_refused = 0;
	uint64_t shot_events_dropped = 0, combat_events_clamped = 0, others_dropped = 0;
	uint64_t trace_dropped = 0;
	uint32_t live_projectiles = 0;
	double tick_weapons_ms_max = 0;
};
static_assert(std::is_trivially_copyable_v<GunplayStats>);

// Where the server's combat events go (GameServer: CombatEvents datagrams to every client; M2b: also the S4 rules).
class CombatEventSink {
public:
	virtual ~CombatEventSink() = default;
	virtual void hit(const net::HitEvent &p_event) = 0;
	virtual void death(const net::DeathEvent &p_event) = 0;
	virtual void detonation(const net::DetonationWire &p_event) = 0;
};

// When a dead slot respawns (M2a: M1's fixed GameServerConfig::respawn_ticks; M2b: the S4 interpreter).
class RespawnPolicy {
public:
	virtual ~RespawnPolicy() = default;
	virtual std::optional<uint32_t> respawn_tick(uint8_t p_slot, uint32_t p_death_tick) = 0;
};

// ---- the level side (sv, as built) -------------------------------------------------------------------------------

struct GunplayLevel {
	std::vector<core::gameplay::WeaponDef> defs; // the match's defs, sorted by variant: index = WeaponTable def id
	std::shared_ptr<const sim::weapons::SurfaceMaterialTable> materials;
	core::gameplay::SoldierDef soldier;
	std::vector<core::gameplay::HitCapsule> capsules;
	core::gameplay::CapsulePoseTable capsule_table;
	bool capsule_standin = false; // the whole table is wp's stand-in layout (the Ant library did not open)
	std::vector<std::string> notes; // capsule rows, skipped kit slots, fallbacks
	KitTable kits;
	float sprint_recover_seconds = 0.12f;
	float max_health = 100.0f;
	uint64_t level_hash = 0; // gunplay_level_hash(level, mode)
	double seconds_weapons = 0, seconds_kits = 0, seconds_capsules = 0;
};

// FNV-1a 64 of "<level>/<mode>" (the level part of the match seed).
uint64_t gunplay_level_hash(std::string_view p_level, std::string_view p_mode);
// splitmix64(p_server_seed ^ p_level_hash) (§4.4).
uint64_t derive_match_seed(uint64_t p_server_seed, uint64_t p_level_hash);

// Reads the level's weapons (WeaponCatalog over the level superbundle), the mode's kits (TeamDefs, the progression
// tables, KitDef::default_unlock per slot), the soldier, hit capsules and grid, and builds the capsule pose table from
// the retail clips (lc's builder). Read-only on the install; nothing is cached.
core::Result<std::shared_ptr<const GunplayLevel>> load_gunplay_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level_id,
		std::string_view p_mode);
// The whole-table stand-in: every stance row at every pitch is wp's standin_target layout of that stance (local frame),
// every transition row the lerp of its two stance rows by the fraction; every row tagged kStandIn.
core::Result<core::gameplay::CapsulePoseTable> standin_capsule_table(const core::gameplay::SoldierDef &p_soldier,
		std::span<const core::gameplay::HitCapsule> p_capsules, const sim::weapons::SurfaceMaterialTable &p_materials);

// p_path empty: wp's defaults; otherwise the file (refused above kMaxMechanicsTuningBytes) through parse_mechanics_tuning.
core::Result<sim::weapons::Mechanics> load_mechanics_cfg(const std::string &p_path);
// The match's WeaponTable (both ends): make_weapon_table(defs, materials, {mechanics, exact_aim, infinite_magazine,
// sprint recover}).
core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> make_match_table(const GunplayLevel &p_level, const sim::weapons::Mechanics &p_mechanics,
		bool p_exact_aim, bool p_infinite_magazine);
// MatchInfo::weapon_hash: weapon_table_hash ^ the capsule table's hash.
uint64_t match_weapon_hash(const sim::weapons::WeaponTable &p_table, const core::gameplay::CapsulePoseTable &p_capsules);
uint8_t match_weapon_flags(bool p_exact_aim, bool p_infinite_magazine);
// The level's quantisation frame of shot origins and combat positions (protocol.h ShotQuantization): the static
// world's bounds' minimum, floored to the metre, minus kShotFrameMargin on every axis. Both ends call it.
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
	uint64_t weapon_hash() const; // weapon_table_hash(table) ^ capsule table hash, into MatchInfo
	uint64_t match_seed() const;
	uint8_t weapon_flags() const; // net::weapon_flags:: of the match options
	const GunplayStats &stats() const;

	// sv additions (as built).
	const GunplayConfig &config() const;
	const GunplayLevel &level() const;
	const sim::weapons::WeaponTable &table() const;
	uint64_t kit_hash() const;
	LagHistory &history();
	const RewindProvider &rewind() const;
	// The slot is not a soldier any more (death, disconnect, removal): its weapons are cleared, projectiles fly on.
	void clear(uint8_t p_slot);
	// The server's CollisionWorld (the projectiles fly against it); set by GameServer::init before the first tick.
	void set_world(const sim::CollisionWorld &p_world);
	void set_match_seed(uint64_t p_seed); // GameServer: GunplayConfig::match_seed or derive_match_seed
	bool armed(uint8_t p_slot) const; // the slot has a live weapons state (spawned with a kit, not dead)
	uint8_t team(uint8_t p_slot) const;
	uint8_t kit(uint8_t p_slot) const;
	// Command receive (§4.1 step 1): a command newly buffered for its tick keeps the client's ack state at its arrival.
	void note_command(uint8_t p_slot, const sim::Command &p_command, const AckState &p_ack);
	// The quantised record of a soldier with its weapon fields, exactly as a receiver decodes it (§7.3; the lag
	// history's shared records and every client's others use it).
	net::OtherSoldier other_record(uint8_t p_slot, const sim::SoldierState &p_state) const;
	// Called once per snapshot tick after every client's snapshot is built: the per-interval shot list, the firing
	// flags and the combat events start over.
	void end_snapshot_interval();
	// Every shot since the previous snapshot tick (all shooters, in step order).
	std::span<const sim::weapons::ShotEvent> interval_shots() const;
	// The traced part of the last tick for p_slot as shooter (shots, its live projectiles, damage it dealt or took,
	// detonations of its shots, deaths it was in), bounded as protocol.h says; r_dropped counts the excess.
	net::TraceGunplayTick trace_tick(uint8_t p_slot, uint32_t p_tick, uint32_t &r_dropped) const;
	// The present-tick capsules of a slot as the provider answers them (TargetInfo); nullopt when not present.
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

} // namespace bf3::host

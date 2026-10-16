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

#include "bf3/core/result.h"
#include "bf3/host/lag_comp.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/mechanics_tuning.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

namespace bf3::host {

struct GunplayConfig {
	std::string mechanics_cfg = std::string(sim::weapons::kMechanicsTuningPath); // §5.5, parsed on both ends
	uint64_t match_seed = 0; // 0: splitmix64(GameServerConfig::seed ^ level hash) (§4.4)
	LagCompConfig lag; // §6
	// Match options, fixed at match start (dev_commands only), sent in MatchInfo::weapon_flags (§7.1) so the client
	// builds the identical table; both are WeaponTableOptions fields and therefore in weapon_hash.
	bool exact_aim = false; // ShotEvent direction = view without recoil, pellet_cone 0, as the range (E3, E4, E12)
	bool infinite_magazine = false; // wp's oracle option
};

struct GunplayStats {
	double weapons_ms = 0, projectiles_ms = 0, rewind_ms = 0; // the last tick, wall ms on the server thread
	uint64_t shots = 0, hits = 0, deaths = 0, damage_dropped = 0; // since init
	uint64_t weapon_precondition_failures = 0; // weapon steps whose command tick != state tick + 1 (must stay 0)
	LagCompStats lag;
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

private:
	Gunplay();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host

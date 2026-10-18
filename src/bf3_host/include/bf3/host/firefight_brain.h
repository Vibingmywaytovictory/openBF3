#pragma once
// The firefight stub bots (docs/design/m2a.md §10.2; owner bt; W3.0 contract: docs/design/m2a-w3.md G3). Only for the
// budget rows (E9, E13, E14) and the playable scene; M2b replaces them behind the same CommandSource seam.
//
// How they get into the server (G3): GameServerConfig::firefight (a shared FirefightConfig, null = none). After the
// target bots, the walkers and S1's walker bots, GameServer::init calls add_firefight_bots(*this, config) (the named
// insertion "M2a w3 [G3 firefight]" in game_server.cpp). It adds `count` server-side soldiers with
// GameServer::add_local_soldier, each a FirefightBrain that keeps the GameServer& (place_soldier, view(), gunplay()).
// Server thread only, serial like S1's brains (§3).
//
// Brains (bt fills the bodies; the rules below are m2a.md §10.2 with the §16 defaults as named fields):
//   * Pairs: count / 2 opposing pairs on lanes computed once from the observer pose (firefight_layout): lanes in a fan
//     of +-lane_fan_deg around the view direction, lane_min_m .. lane_max_m out, each pair pair_min_m .. pair_max_m
//     apart, both ends checked for a clear eye-to-chest ray and a nav-mesh foothold, a rejected lane retried with a
//     seeded offset at most lane_retries times. Placed with place_soldier at (re)spawn.
//   * Free: S1's bot fill spawns (choose_spawn); a target is the nearest enemy with a clear eye-to-chest ray, checked
//     every retarget_ticks (staggered by slot); with none for no_target_s, fire along the longest clear heading. Never
//     place_soldier a dead slot in this mode: placing a dead slot respawns it at once, skipping respawn_ticks and the
//     RespawnPolicy (sv's game_server.cpp place handling); free-mode deaths wait for the RespawnPolicy and choose_spawn.
//   * Aim: the direction to the target's chest plus a seeded error with sigma aim_sigma_start_deg decaying to
//     aim_sigma_end_deg over aim_sigma_decay_s on target; bursts of burst_min_rounds .. burst_max_rounds with
//     burst_pause_min_s .. burst_pause_max_s pauses; semi-automatic weapons at semi_rate_hz; reload at an empty
//     magazine or, without a target, below reload_fraction of the magazine.
//   * A dead brain's source is still asked every tick (game_server.cpp): it checks ServerView::alive and sends idle.
//   * Seeds: firefight_seed(match_seed, slot) = match_seed ^ slot, the stream of the brain; a life's draws mix in the
//     life (splitmix64 of the seed and the life, m2a.md §4.4), so the firefight is reproducible (E15).
// Kits are sv's (kit = slot % 4, game_server.cpp); teams alternate 1 / 2 (Pairs: the two ends of a lane differ).

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/sim/sim_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::host {

enum class FirefightMode : uint8_t { Pairs = 0, Free = 1 };

// The observer pose (§10.2): gx's m2a_check --m2a-firefight passes the fixed camera spot of m2a_metro.tscn; without
// one (valid false) the layout starts at Metro's range lane (find_range_lane, spawn 87, m2a.md §1).
struct FirefightObserver {
	bool valid = false;
	sim::Vec3 position{}; // eye, world
	float yaw = 0.0f; // radians, sim convention (0 looks down +Z)
};

// What the brains counted (server thread writes, any thread reads; relaxed).
struct FirefightStats {
	std::atomic<uint32_t> bots{ 0 };
	std::atomic<uint32_t> lanes{ 0 }; // Pairs: lanes accepted
	std::atomic<uint32_t> lanes_rejected{ 0 }; // Pairs: lane tries that failed the ray or foothold check
	std::atomic<uint64_t> bursts{ 0 };
	std::atomic<uint64_t> reloads{ 0 };
	std::atomic<uint64_t> retargets{ 0 };
	std::atomic<uint64_t> no_target_fire{ 0 }; // Free: bursts along the longest clear heading
	std::atomic<uint64_t> placements{ 0 }; // place_soldier calls
	// bt (as built, docs/design/m2a-bt.md).
	std::atomic<uint64_t> los_rays{ 0 }; // eye-to-chest rays cast (perception and target checks)
	std::atomic<uint64_t> hunts{ 0 }; // Free: hunt goals handed to the walker (the nearest enemy, snapped onto the mesh)
	std::atomic<uint64_t> strafes{ 0 }; // strafe legs started while engaged
	std::atomic<uint64_t> mode_switches{ 0 }; // fire-mode counter steps sent to reach the weapon's automatic mode
	std::atomic<uint64_t> sidearm_switches{ 0 }; // primary out of ammo: the sidearm selected
	std::atomic<uint32_t> walk_unavailable{ 0 }; // 1: walk was asked for without GameServerConfig::bots' BotLevel
	std::atomic<uint32_t> lanes_shared{ 0 }; // Pairs: pairs placed on an already used lane (fewer lanes than pairs)
};

struct FirefightConfig {
	FirefightMode mode = FirefightMode::Pairs;
	uint32_t count = 64; // bots (Pairs: count / 2 pairs; an odd count leaves the last bot out)
	FirefightObserver observer;
	// Aim (§10.2, [inferred]).
	float aim_sigma_start_deg = 1.5f;
	float aim_sigma_end_deg = 0.5f;
	float aim_sigma_decay_s = 1.0f;
	// Fire.
	uint32_t burst_min_rounds = 3;
	uint32_t burst_max_rounds = 6;
	float burst_pause_min_s = 0.3f;
	float burst_pause_max_s = 0.6f;
	float semi_rate_hz = 2.0f;
	float reload_fraction = 0.5f; // without a target, reload below this part of the magazine
	// Pairs layout.
	float lane_fan_deg = 35.0f; // +- around the observer's view direction
	float lane_min_m = 15.0f; // lane centre distance from the observer
	float lane_max_m = 60.0f;
	float pair_min_m = 20.0f; // distance between the two ends of a lane
	float pair_max_m = 40.0f;
	uint32_t lane_retries = 8;
	// Free mode.
	uint32_t retarget_ticks = 30;
	float no_target_s = 2.0f;
	bool walk = true; // move on the nav mesh between fights (S1's WalkerBrain; needs GameServerConfig::bots' BotLevel)
	// Shared counters (null = none). The pointee is written by the server thread.
	std::shared_ptr<FirefightStats> stats;
};

// m2a.md §4.4 as the task pins it: the brain stream of a slot.
constexpr uint64_t firefight_seed(uint64_t p_match_seed, uint8_t p_slot) {
	return p_match_seed ^ uint64_t(p_slot);
}

// One pair lane (Pairs mode): both ends' feet and facing (each end faces the other).
struct FirefightLane {
	sim::Vec3 a{}, b{};
	float yaw_a = 0.0f, yaw_b = 0.0f;
	uint32_t tries = 0; // tries this lane needed (1 = the first)
};
// The Pairs layout from the observer (bt): at most p_config.count / 2 lanes, each end with a clear eye-to-chest ray to
// the other and a foothold (p_world rays down; the nav check is bt's). Deterministic for a seed. Fails when no lane
// holds at all.
// As built (bt): p_config.observer must be valid (add_firefight_bots resolves the default first). Lane i of n is aimed
// at the fan angle -fan + 2 fan i / (n - 1) and the distance lane_min + (lane_max - lane_min) frac(0.618 i); retries
// draw both from the seed. Even tries lay the lane across the view ray, odd tries along it. An end is accepted when a
// ray down from 2 m above the observer's eye meets ground within 3 m of the observer's floor, the standing capsule
// fits there, the two ends see each other eye to chest, and (first half of the tries only) the observer's eye sees
// both chests. With p_nav (the BotLevel's query) each end also needs a poly of the main component within 1 m.
core::Result<std::vector<FirefightLane>> firefight_layout(const sim::CollisionWorld &p_world, const FirefightConfig &p_config, uint64_t p_seed);
struct BotNav;
core::Result<std::vector<FirefightLane>> firefight_layout(const sim::CollisionWorld &p_world, const FirefightConfig &p_config, uint64_t p_seed,
		BotNav *p_nav, uint32_t *r_rejected = nullptr);

// The brains of one GameServer share this (bt; firefight_brain.cpp): the server, the config, the soldier's eye
// heights, the nav hook (bot_fill.h BotNav; null = no walking) and the lanes.
struct FirefightShared;

// One bot (bt). Holds the GameServer for place_soldier, view() and gunplay().
class FirefightBrain final : public CommandSource {
public:
	FirefightBrain(GameServer &r_server, std::shared_ptr<const FirefightConfig> p_config, uint32_t p_index, const FirefightLane *p_lane);
	// add_firefight_bots' path (bt): every brain of the server shares p_shared; p_end 0 / 1 = the lane's a / b end;
	// p_offset moves the placement along the lane's normal (a pair that shares a lane).
	FirefightBrain(std::shared_ptr<FirefightShared> p_shared, uint32_t p_index, const FirefightLane *p_lane, uint8_t p_end, float p_offset);
	~FirefightBrain() override;
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;
	void set_slot(uint8_t p_slot); // add_firefight_bots tells the brain its slot after add_local_soldier

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The G3 hook (body bt; W3.0's stub succeeds without GameServerConfig::firefight and refuses with one, so a run that
// asks for firefight bots before bt lands fails loudly instead of running without them).
core::Status add_firefight_bots(GameServer &r_server, const GameServerConfig &p_config);

} // namespace bf3::host

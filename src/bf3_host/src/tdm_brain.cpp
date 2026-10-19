// The TDM bots (tdm_brain.h; docs/design/m2b.md §5; as built: docs/design/m2b-bb.md). Server thread only, serial in
// slot order. The fighting core started from M2a bt's FirefightBrain Free mode (firefight_brain.cpp: the turn limits,
// the fire-mode switch, bursts, taps, the sidearm, the strafe legs, WalkerBrain movement re-expressed in the brain's
// own view yaw); TDM adds perception (perception.h), target scores with a reaction delay, the aim model
// (aim_model.h), hunting the last known enemy, the contact heat grid, retreat and regen waits, the M67, the knife,
// the defibrillator revive and the pre-round idle.
//
// One TdmShared per GameServer holds what every brain reads: the view (ServerView: states, teams, the rules' downed
// list, phase, restriction and kits), the gunplay (weapons, the tick's shots), the nav (bot_fill.h BotNav), the
// config, and a per-tick scan made by the first brain of each tick: teams, alive flags, firing flags, a ring of the
// last kRingTicks ticks' shots (each shot once, by a per-slot shot_index cursor) and the contact heat (shots and
// deaths in heat_cell_m cells, decayed over heat_decay_s).
//
// A brain per tick (alive):
//   restricted  ServerView::input_restricted() or a phase other than InRound: an idle command (the server drops the
//               inputs anyway, [cmd]);
//   perceive    on its perception tick (every update_ticks, staggered by slot): Perception::update; a shot passing
//               within kShotAtMetres of its chest marks "shot at" (and the shooter), a health drop "damaged";
//   target      among the detected enemies with a clear ray in the last kVisibleFireTicks: the best score
//               (w_distance / d + w_aim + w_damage + w_current), switched only at a switch_margin better score; a new
//               target starts the reaction delay and the aim error's clock;
//   activity    Revive (assault, not engaged for revive_idle_s, a downed teammate in reach of its window) > Retreat /
//               RegenWait (below retreat_below_hp) > Fight (a visible target) > Grenade (a lost target behind cover
//               15-35 m away) > Hunt (a known enemy: the last seen or heard position) > Roam (a heat cell or S1's random
//               nav point; without a walker the bot stands and looks around, a new heading every 1.5-3 s, held while an enemy in sight is being detected);
//   aim         the target's chest (perception_chest) led by the weapon's muzzle speed and gravity, plus a seeded
//               error of sigma = aim_error(skill, time on target, moving, crossing speed), redrawn every kErrorTicks;
//               the view turns at most kTurnRateDegS; the command subtracts skill x the weapon's recoil offset;
//   fire        within kFireConeDeg of the aim, the target's last clear ray at most kVisibleFireTicks old (E2 allows
//               30), the reaction elapsed: bursts by range (burst_min .. burst_max rounds within 60 m, single shots
//               beyond), taps for other fire modes; reload at an empty magazine or below 30 % with nothing in sight;
//               the sidearm when the primary is empty; one blind burst at a lost target's last-known position
//               (blind_fire_share, counted apart); the knife within knife_within_m from behind.
// Determinism: the bot seed is match_seed ^ slot; each life draws its skill and streams from splitmix64(seed ^ life);
// no wall clock reaches a decision (the per-tick nanoseconds are only measured), no unordered container.

#include "bf3/host/tdm_brain.h"

#include "bf3/host/bot_fill.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;

// Named constants ([inferred] unless m2b.md §5 gives them; docs/design/m2b-bb.md "Constants").
constexpr float kTurnRateDegS = 540.0f; // S1's WalkerParams::yaw_rate_deg_s
constexpr float kPitchTurnRateDegS = 360.0f;
constexpr float kMaxPitchDeg = 80.0f;
constexpr float kFireConeDeg = 4.0f; // fire only when the view is this close to the aim (error included)
constexpr uint32_t kVisibleFireTicks = 12; // aimed fire needs a clear ray this recent (two perception updates)
constexpr uint32_t kLoseTicks = 90; // a target unseen this long is dropped (then hunted at its last position)
constexpr uint32_t kErrorTicks = 6; // a new aim error draw
constexpr float kErrorPitchShare = 0.6f;
constexpr uint32_t kModeDelayTicks = 3;
constexpr uint32_t kModeWaitTicks = 10;
constexpr float kStrafeAxis = 0.8f;
constexpr uint32_t kStrafeMinTicks = 60, kStrafeMaxTicks = 150;
constexpr uint32_t kStrafeGroundTicks = 6;
// Without a walker (no nav on the level) an idle bot looks around: a new heading 60-150 degrees to either side every
// 1.5-3 s [inferred] (the spawn heading first, held while an enemy in sight is being detected), so it still sees what
// its spawn faces away from.
constexpr uint32_t kLookMinTicks = 90, kLookMaxTicks = 180;
constexpr float kLookMinDeg = 60.0f, kLookMaxDeg = 150.0f;
constexpr float kSprintConeDeg = 20.0f;
constexpr float kSingleShotBeyondMetres = 60.0f; // §5.5: single shots beyond 60 m
constexpr float kShotAtMetres = 1.5f; // an enemy shot passing this close to the chest "shoots at" the bot
constexpr float kShotRayMetres = 150.0f;
constexpr uint32_t kRingTicks = 30; // shots kept for hearing and "shot at"
constexpr size_t kMaxRing = 1024;
constexpr uint32_t kFiringTicks = 30; // a slot that shot within this many ticks is "firing" (detection boost)
constexpr uint32_t kGoalResetTicks = 90; // a new walker goal at most this often (unless forced)
constexpr float kGoalMoveMetres = 8.0f; // a goal this far from the walker's current one replaces it
constexpr float kHuntArriveMetres = 3.0f;
constexpr uint32_t kRoamGoalTicks = 600; // a roam goal from the heat grid at most every 10 s
constexpr float kHeatShot = 0.05f, kHeatDeath = 1.0f, kHeatMin = 0.2f;
constexpr int kHeatCells = 64;
constexpr float kHeatRadiusMetres = 160.0f; // heat goals within this of the bot
constexpr uint32_t kShotAtTicks = 120; // "shot at" counts this long (give up a revive, retreat trigger)
constexpr float kRetreatMetres = 14.0f;
constexpr uint32_t kRetreatMaxTicks = 360;
constexpr uint32_t kRetreatClearTicks = 60; // out of sight this long: wait for regen
constexpr float kRunSpeed = 4.5f; // m/s for the revive travel estimate
constexpr float kPathFactor = 1.3f; // path length over straight distance
constexpr float kReviveDirectMetres = 2.5f; // closer: steer straight at the corpse
constexpr float kReviveStandMetres = 0.8f;
constexpr float kReviveCrouchMetres = 2.0f;
constexpr float kReviveCentreUp = 0.3f; // the rules' corpse sphere centre above the feet (§3.6)
constexpr float kDefibReach = 1.7f; // eye to the corpse centre (the defib reaches 1.8 m)
constexpr float kDefibConeDeg = 6.0f;
constexpr uint32_t kDefibRetryTicks = 66; // 60 rpm + margin
constexpr uint32_t kReviveTimeoutTicks = 900;
constexpr uint32_t kGrenadeLostMinTicks = 30, kGrenadeLostMaxTicks = 240;
constexpr uint32_t kGrenadeSwitchTicks = 120, kGrenadeAfterTicks = 45, kGrenadeHoldExtraTicks = 4;
constexpr float kKnifeBehindDeg = 100.0f;
constexpr uint32_t kKnifeTicks = 45;
constexpr uint32_t kBlindTicks = 40;
constexpr float kTacticalReload = 0.3f; // §5.5: reload below 30 % with no target
constexpr uint8_t kSlotPrimary = 0, kSlotSidearm = 1, kSlotGrenade = 6, kSlotKnife = 7;
constexpr size_t kMaxBrainSlots = 256;

constexpr float kDeg = sim::kPi / 180.0f;

uint64_t mix(uint64_t p_x) {
	return sw::splitmix64(p_x);
}
uint64_t next(uint64_t &r_state) {
	r_state = mix(r_state);
	return r_state;
}
float unit(uint64_t &r_state) {
	return float(next(r_state) >> 40) / float(1ull << 24);
}
float uniform(uint64_t &r_state, float p_min, float p_max) {
	return p_min + (p_max - p_min) * unit(r_state);
}
uint32_t uniform_u(uint64_t &r_state, uint32_t p_min, uint32_t p_max) {
	if (p_max <= p_min) {
		return p_min;
	}
	return p_min + uint32_t(next(r_state) % uint64_t(p_max - p_min + 1));
}
float gauss(uint64_t &r_state) {
	const float u1 = std::max(unit(r_state), 1e-7f);
	const float u2 = unit(r_state);
	return std::sqrt(-2.0f * std::log(u1)) * std::cos(sim::kTwoPi * u2);
}
float wrap(float p_radians) {
	float a = std::fmod(p_radians, sim::kTwoPi);
	if (a > sim::kPi) {
		a -= sim::kTwoPi;
	} else if (a < -sim::kPi) {
		a += sim::kTwoPi;
	}
	return a;
}
float yaw_of(float p_dx, float p_dz) {
	return std::atan2(p_dx, p_dz); // yaw 0 looks down +Z (command.h)
}
float horizontal(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_b[0] - p_a[0], dz = p_b[2] - p_a[2];
	return std::sqrt(dx * dx + dz * dz);
}
float distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_b[0] - p_a[0], dy = p_b[1] - p_a[1], dz = p_b[2] - p_a[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}
sim::Vec3 add(const sim::Vec3 &p_a, float p_x, float p_y, float p_z) {
	return { p_a[0] + p_x, p_a[1] + p_y, p_a[2] + p_z };
}
sim::Vec3 sub(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}
bool finite3(const sim::Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}
bool is_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}
sim::Vec3 view_dir(float p_yaw, float p_pitch) {
	const float cp = std::cos(p_pitch);
	return { std::sin(p_yaw) * cp, std::sin(p_pitch), std::cos(p_yaw) * cp };
}
// The distance from p_point to the segment p_origin + t p_dir, t in [0, p_length] (p_dir unit).
float ray_point_distance(const sim::Vec3 &p_origin, const sim::Vec3 &p_dir, float p_length, const sim::Vec3 &p_point) {
	const sim::Vec3 d = sub(p_point, p_origin);
	const float t = std::clamp(d[0] * p_dir[0] + d[1] * p_dir[1] + d[2] * p_dir[2], 0.0f, p_length);
	return distance(add(p_origin, p_dir[0] * t, p_dir[1] * t, p_dir[2] * t), p_point);
}
uint64_t now_ns() {
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct RecentShot {
	uint8_t shooter = 0;
	sim::Tick tick = 0;
	sim::Vec3 origin{};
	sim::Vec3 direction{};
};

} // namespace

const char *tdm_bot_activity_name(TdmBotActivity p_activity) {
	switch (p_activity) {
		case TdmBotActivity::Dead:
			return "dead";
		case TdmBotActivity::Restricted:
			return "restricted";
		case TdmBotActivity::Roam:
			return "roam";
		case TdmBotActivity::Hunt:
			return "hunt";
		case TdmBotActivity::Fight:
			return "fight";
		case TdmBotActivity::Retreat:
			return "retreat";
		case TdmBotActivity::RegenWait:
			return "regen_wait";
		case TdmBotActivity::Revive:
			return "revive";
		case TdmBotActivity::Grenade:
			return "grenade";
	}
	return "?";
}

// ---- the shared part -----------------------------------------------------------------------------------------------

struct TdmShared {
	TdmBrainHost host;
	std::shared_ptr<const TdmBotConfig> config;
	TdmBotStats *stats = nullptr;
	TdmBotObserver *observer = nullptr;
	std::unique_ptr<BotNav> nav; // null: no walking
	sim::bots::WalkerParams walker;
	// The per-tick scan (the first brain of each tick).
	sim::Tick scan_tick = UINT32_MAX;
	std::vector<uint8_t> teams, alive, was_alive, firing;
	std::vector<sim::Tick> fired_tick;
	std::vector<uint32_t> shot_cursor;
	std::vector<RecentShot> ring;
	std::vector<sim::bots::HeardShot> heard; // the ring as Perception reads it
	// Contact heat.
	std::array<float, kHeatCells * kHeatCells> heat{};
	std::array<sim::Tick, kHeatCells * kHeatCells> heat_tick{};
	// Timing of the current tick (wall nanoseconds; measured only).
	sim::Tick time_tick = UINT32_MAX;
	uint64_t perception_ns = 0, brains_ns = 0;

	template <typename T>
	void count(std::atomic<T> TdmBotStats::*p_field, T p_amount = 1) {
		if (stats != nullptr) {
			(stats->*p_field).fetch_add(p_amount, std::memory_order_relaxed);
		}
	}
	float eye_of(sim::Pose p_pose) const {
		const size_t i = size_t(p_pose) < 3 ? size_t(p_pose) : 0;
		return host.eye_heights[i] > 0.0f ? host.eye_heights[i] : 1.6f;
	}
	sim::Vec3 eye_point(const sim::SoldierState &p_state) const { return add(p_state.position, 0.0f, eye_of(p_state.pose), 0.0f); }
	sim::Vec3 chest_point(const sim::SoldierState &p_state) const { return sim::bots::perception_chest(config->perception, p_state, host.eye_heights); }

	int heat_cell(const sim::Vec3 &p_at) const {
		const float cell = std::max(config->heat_cell_m, 1.0f);
		const float fx = (p_at[0] - host.heat_origin[0]) / cell, fz = (p_at[2] - host.heat_origin[2]) / cell;
		if (!std::isfinite(fx) || !std::isfinite(fz) || fx < 0.0f || fz < 0.0f || fx >= float(kHeatCells) || fz >= float(kHeatCells)) {
			return -1;
		}
		return int(fz) * kHeatCells + int(fx);
	}
	float heat_at(int p_cell, sim::Tick p_tick) {
		const uint32_t decay_ticks = std::max<uint32_t>(sim::seconds_to_ticks(config->heat_decay_s), 1u);
		const uint32_t age = p_tick >= heat_tick[size_t(p_cell)] ? p_tick - heat_tick[size_t(p_cell)] : 0u;
		if (age > 0) {
			heat[size_t(p_cell)] *= std::exp(-float(age) / float(decay_ticks));
			heat_tick[size_t(p_cell)] = p_tick;
		}
		return heat[size_t(p_cell)];
	}
	void add_heat(const sim::Vec3 &p_at, float p_amount, sim::Tick p_tick) {
		const int c = heat_cell(p_at);
		if (c >= 0) {
			heat[size_t(c)] = std::min(heat_at(c, p_tick) + p_amount, 1000.0f);
		}
	}
	sim::Vec3 heat_centre(int p_cell) const {
		const float cell = std::max(config->heat_cell_m, 1.0f);
		return { host.heat_origin[0] + (float(p_cell % kHeatCells) + 0.5f) * cell, 0.0f, host.heat_origin[2] + (float(p_cell / kHeatCells) + 0.5f) * cell };
	}

	void begin_tick(sim::Tick p_tick) {
		if (time_tick != p_tick) {
			time_tick = p_tick;
			perception_ns = brains_ns = 0;
		}
		if (nav != nullptr) {
			nav->run_once(p_tick); // the first brain of the tick runs the path searches
		}
		if (scan_tick == p_tick) {
			return;
		}
		scan_tick = p_tick;
		const ServerView &view = *host.view;
		const std::span<const sim::SoldierState> states = view.states();
		const size_t n = std::min(states.size(), kMaxBrainSlots);
		teams.assign(n, 0);
		alive.assign(n, 0);
		if (was_alive.size() < n) {
			was_alive.resize(n, 0);
			fired_tick.resize(n, 0);
			shot_cursor.resize(n, 0);
		}
		firing.assign(n, 0);
		for (size_t s = 0; s < n; ++s) {
			teams[s] = view.team(uint8_t(s));
			alive[s] = view.alive(uint8_t(s)) ? 1 : 0;
			if (was_alive[s] != 0 && alive[s] == 0 && finite3(states[s].position)) {
				add_heat(states[s].position, kHeatDeath, p_tick);
			}
			was_alive[s] = alive[s];
		}
		// The shots since the last scan (each once: the slot's shot_index never resets, weapon_sim.h).
		if (host.gunplay != nullptr) {
			for (const sw::ShotEvent &shot : host.gunplay->interval_shots()) {
				const size_t s = shot.player_slot;
				if (s >= n || shot.shot_index < shot_cursor[s] || !finite3(shot.origin) || !finite3(shot.direction)) {
					continue;
				}
				shot_cursor[s] = shot.shot_index + 1u;
				if (ring.size() >= kMaxRing) {
					ring.erase(ring.begin());
				}
				ring.push_back({ uint8_t(s), p_tick, shot.origin, shot.direction });
				fired_tick[s] = p_tick;
				add_heat(shot.origin, kHeatShot, p_tick);
			}
		}
		ring.erase(std::remove_if(ring.begin(), ring.end(), [&](const RecentShot &p_s) { return p_s.tick + kRingTicks < p_tick; }), ring.end());
		heard.clear();
		for (const RecentShot &r : ring) {
			heard.push_back({ r.shooter, r.origin, r.tick });
		}
		for (size_t s = 0; s < n; ++s) {
			firing[s] = fired_tick[s] != 0 && p_tick - fired_tick[s] <= kFiringTicks ? 1 : 0;
		}
	}
};

core::Result<std::shared_ptr<TdmShared>> make_tdm_shared(const TdmBrainHost &p_host, std::shared_ptr<const TdmBotConfig> p_config) {
	if (p_host.view == nullptr) {
		return core::make_error("tdm bots: no server view");
	}
	auto sh = std::make_shared<TdmShared>();
	sh->host = p_host;
	sh->config = p_config != nullptr ? std::move(p_config) : std::make_shared<const TdmBotConfig>();
	sh->stats = sh->config->stats.get();
	sh->observer = sh->config->observer.get();
	sh->walker = p_host.walker;
	sh->walker.objective_share = std::clamp(sh->config->hunt_share, 0.0f, 1.0f);
	if (p_host.nav != nullptr && p_host.nav->nav != nullptr) {
		core::Result<std::unique_ptr<BotNav>> nav = make_bot_nav(p_host.nav);
		if (!nav) {
			return core::error_at("tdm bots", nav.error());
		}
		sh->nav = std::move(*nav);
	} else if (sh->stats != nullptr) {
		sh->stats->walk_unavailable.store(1, std::memory_order_relaxed);
	}
	return sh;
}

// ---- the brain -------------------------------------------------------------------------------------------------------

struct TdmBrain::Impl {
	std::shared_ptr<TdmShared> shared;
	uint32_t index = 0;
	uint8_t slot = 0xFF;
	uint64_t seed = 0;
	uint64_t rng = 0;
	sim::bots::Perception perception;
	std::optional<sim::bots::WalkerBrain> walker;
	std::vector<sim::bots::BotGoal> goals;
	uint32_t walker_resets = 0;
	bool have_goal = false;
	sim::Vec3 goal_pos{};
	sim::Tick goal_tick = 0;
	sim::Tick roam_tick = 0;

	// The life.
	uint32_t life = UINT32_MAX;
	sim::Tick life_tick = 0;
	uint8_t kit = 0;
	float skill = 0.5f, skill01 = 0.5f;
	float view_yaw = 0.0f, view_pitch = 0.0f;
	TdmBotActivity activity = TdmBotActivity::Dead;
	float last_health = 100.0f;
	sim::Tick damaged_tick = 0;
	sim::Tick shot_at_tick = 0;
	int shot_by = -1;
	sim::Vec3 shot_from{};
	sim::Tick shots_checked = 0;
	// The target.
	int target = -1;
	sim::Tick target_since = 0, react_at = 0, last_engaged = 0;
	float err_yaw = 0.0f, err_pitch = 0.0f;
	sim::Tick err_next = 0;
	int lost_target = -1; // the target of the last loss (blind fire, grenade decided once)
	bool loss_handled = false;
	sim::Tick blind_until = 0;
	sim::Vec3 blind_at{};
	sim::Tick searched_tick = 0; // known enemies last seen before this were searched
	// Fire.
	bool bursting = false;
	uint32_t burst_rounds = 0, burst_start = 0;
	sim::Tick pause_until = 0, next_tap = 0;
	bool tapped = false;
	uint8_t mode_counter = 0;
	sim::Tick mode_wait = 0;
	bool reload_held = false;
	bool sidearm = false;
	// Strafe.
	bool strafing = false;
	float strafe_dir = 1.0f, strafe_half = 1.0f;
	sim::Vec3 strafe_anchor{};
	float strafe_ax = 1.0f, strafe_az = 0.0f;
	sim::Tick strafe_until = 0;
	// Look around (no walker).
	float look_yaw = 0.0f;
	sim::Tick look_until = 0;
	// Retreat / regen.
	sim::Tick retreat_since = 0, regen_since = 0, clear_since = 0;
	sim::Vec3 threat{};
	// Revive.
	int revive_slot = -1;
	sim::Tick revive_since = 0, defib_next = 0;
	// Grenade.
	int grenade_phase = 0; // 0 none, 1 switch, 2 hold, 3 after
	sim::Tick grenade_phase_tick = 0, grenade_next = 0;
	uint32_t grenade_hold = 0;
	sim::Vec3 grenade_at{};
	// Knife.
	sim::Tick knife_until = 0;

	const TdmBotConfig &cfg() const { return *shared->config; }
	enum class Trigger : uint8_t { None, Aimed, Blind, Defib, Grenade, Knife };

	void new_life(sim::Tick p_tick, const sim::SoldierState &p_state) {
		const TdmBotConfig &c = cfg();
		life = p_state.life;
		life_tick = p_tick;
		rng = mix(seed ^ (uint64_t(life) << 8));
		const float lo = std::min(c.skill_min, c.skill_max), hi = std::max(c.skill_min, c.skill_max);
		skill01 = unit(rng);
		skill = std::clamp(lo + (hi - lo) * skill01, 0.0f, 1.0f);
		const ServerView &view = *shared->host.view;
		kit = shared->host.rules ? view.kit(slot) : (shared->host.gunplay != nullptr ? shared->host.gunplay->kit(slot) : uint8_t(0));
		view_yaw = sim::yaw_radians(p_state.yaw);
		view_pitch = 0.0f;
		perception.reset();
		last_health = p_state.health;
		damaged_tick = shot_at_tick = 0;
		shot_by = -1;
		shots_checked = p_tick;
		target = -1;
		target_since = react_at = last_engaged = 0;
		err_yaw = err_pitch = 0.0f;
		err_next = 0;
		lost_target = -1;
		loss_handled = true;
		blind_until = 0;
		searched_tick = 0;
		bursting = false;
		pause_until = next_tap = p_tick;
		tapped = false;
		mode_wait = 0;
		reload_held = false;
		sidearm = false;
		strafing = false;
		look_until = 0;
		retreat_since = regen_since = clear_since = 0;
		revive_slot = -1;
		grenade_phase = 0;
		knife_until = 0;
		have_goal = false;
		goals.clear();
		roam_tick = 0;
		activity = TdmBotActivity::Roam;
	}

	// ---- movement goals ----------------------------------------------------------------------------------------------

	// Hands the walker a goal near p_at (snapped onto the main component). Returns false without a nav or a foothold.
	bool set_goal(sim::Tick p_tick, const sim::Vec3 &p_at, float p_arrive, bool p_force) {
		TdmShared &sh = *shared;
		if (sh.nav == nullptr || !finite3(p_at)) {
			return false;
		}
		BotNav &nav = *sh.nav;
		sim::NavPolyRef poly = 0;
		sim::Vec3 at{};
		if (!nav.query->nearest(p_at, { 3.0f, 4.0f, 3.0f }, &poly, &at) || poly == 0) {
			return false;
		}
		if (sim::nav_poly_component(nav.query->world(), nav.level->components, poly) != nav.level->main_component) {
			return false;
		}
		if (have_goal && walker.has_value() && horizontal(at, goal_pos) < kGoalMoveMetres) {
			return true; // the walker already walks there
		}
		if (!p_force && have_goal && p_tick - goal_tick < kGoalResetTicks) {
			return true; // keep the old goal a little longer (no churn)
		}
		sim::bots::BotGoal g;
		g.kind = sim::bots::BotGoal::Kind::NavPoint;
		g.position = at;
		g.poly = poly;
		g.arrive_radius = p_arrive;
		goals.assign(1, g);
		have_goal = true;
		goal_pos = at;
		goal_tick = p_tick;
		++walker_resets;
		walker.emplace(slot, mix(seed ^ (uint64_t(walker_resets) << 40)), sh.walker);
		sh.count(&TdmBotStats::walker_resets, uint64_t(1));
		return true;
	}

	void clear_goal() {
		have_goal = false;
		goals.clear();
	}

	// The walker's move this tick in world (x, z), its yaw and sprint / pose wishes; false without a walker.
	bool walk(sim::Tick p_tick, const sim::SoldierState &p_state, float &r_dx, float &r_dz, float &r_yaw, bool &r_sprint, sim::Pose &r_pose) {
		TdmShared &sh = *shared;
		if (sh.nav == nullptr) {
			return false;
		}
		if (!walker.has_value()) {
			walker.emplace(slot, mix(seed ^ 0x57414C4Bull), sh.walker); // "WALK"
		}
		const sim::bots::WalkerView wv = sh.nav->view(p_tick, slot, p_state, goals, sh.eye_of(sim::Pose::Stand));
		const sim::Command wc = walker->command(wv);
		const float wy = sim::yaw_radians(wc.yaw);
		const float f = sim::axis_value(wc.move_forward), r = sim::axis_value(wc.move_right);
		r_dx = f * std::sin(wy) - r * std::cos(wy);
		r_dz = f * std::cos(wy) + r * std::sin(wy);
		r_yaw = wy;
		r_sprint = (wc.buttons & sim::buttons::kSprint) != 0;
		r_pose = wc.pose;
		return true;
	}

	void strafe(sim::Tick p_tick, const sim::CollisionWorld &p_world, const sim::SoldierState &p_self, float &r_dx, float &r_dz) {
		const TdmBotConfig &c = cfg();
		if (!strafing) {
			strafing = true;
			strafe_anchor = p_self.position;
			strafe_ax = -std::cos(view_yaw); // the view's right (command.h motor axes)
			strafe_az = std::sin(view_yaw);
			strafe_dir = unit(rng) < 0.5f ? -1.0f : 1.0f;
			strafe_half = 0.5f * uniform(rng, std::max(c.strafe_min_m, 0.5f), std::max(c.strafe_max_m, std::max(c.strafe_min_m, 0.5f)));
			strafe_until = p_tick + uniform_u(rng, kStrafeMinTicks, kStrafeMaxTicks);
		} else if (p_tick >= strafe_until) {
			strafe_dir = -strafe_dir;
			strafe_until = p_tick + uniform_u(rng, kStrafeMinTicks, kStrafeMaxTicks);
		}
		const sim::Vec3 off = sub(p_self.position, strafe_anchor);
		const float along = off[0] * strafe_ax + off[2] * strafe_az;
		if (along * strafe_dir > strafe_half) {
			strafe_dir = -strafe_dir;
		} else if ((p_tick + slot) % kStrafeGroundTicks == 0) {
			const sim::Vec3 ahead = add(p_self.position, strafe_dir * strafe_ax * 0.8f, 0.5f, strafe_dir * strafe_az * 0.8f);
			if (!p_world.ground_below(ahead, 1.2f, sim::query_mask::kMovement, nullptr)) {
				strafe_dir = -strafe_dir; // no floor there
			}
		}
		r_dx = strafe_dir * strafe_ax * kStrafeAxis;
		r_dz = strafe_dir * strafe_az * kStrafeAxis;
	}

	// A heat cell near the bot drawn by weight (or none).
	std::optional<sim::Vec3> heat_goal(sim::Tick p_tick, const sim::Vec3 &p_at) {
		TdmShared &sh = *shared;
		float total = 0.0f;
		std::array<std::pair<int, float>, 64> picks{};
		size_t n = 0;
		for (int c = 0; c < kHeatCells * kHeatCells; ++c) {
			if (sh.heat[size_t(c)] < kHeatMin) {
				continue; // cheap reject on the undecayed value
			}
			const float h = sh.heat_at(c, p_tick);
			if (h < kHeatMin) {
				continue;
			}
			const sim::Vec3 centre = sh.heat_centre(c);
			const float d = horizontal(centre, p_at);
			if (d > kHeatRadiusMetres || d < kHuntArriveMetres * 3.0f) {
				continue;
			}
			if (n < picks.size()) {
				picks[n++] = { c, h };
				total += h;
			}
		}
		if (n == 0 || !(total > 0.0f)) {
			return std::nullopt;
		}
		float u = unit(rng) * total;
		for (size_t i = 0; i < n; ++i) {
			u -= picks[i].second;
			if (u <= 0.0f || i + 1 == n) {
				sim::Vec3 at = sh.heat_centre(picks[i].first);
				at[1] = p_at[1];
				return at;
			}
		}
		return std::nullopt;
	}

	// ---- perception and targets --------------------------------------------------------------------------------------

	bool aims_at_me(const sim::SoldierState &p_other, const sim::SoldierState &p_self) const {
		const TdmShared &sh = *shared;
		const sim::Vec3 from = sh.eye_point(p_other);
		const sim::Vec3 to = sh.chest_point(p_self);
		const float d = distance(from, to);
		if (!(d > 0.1f)) {
			return true;
		}
		const sim::Vec3 dir = view_dir(sim::yaw_radians(p_other.yaw), sim::pitch_radians(p_other.pitch));
		const float cosang = ((to[0] - from[0]) * dir[0] + (to[1] - from[1]) * dir[1] + (to[2] - from[2]) * dir[2]) / d;
		return cosang >= std::cos(std::clamp(cfg().aims_at_me_deg, 0.0f, 90.0f) * kDeg);
	}

	// The enemy shots of the ring since the last check that passed near the chest: shot at (and by whom).
	void check_shots(sim::Tick p_tick, const sim::SoldierState &p_self) {
		TdmShared &sh = *shared;
		const sim::Vec3 chest = sh.chest_point(p_self);
		const uint8_t team = slot < sh.teams.size() ? sh.teams[slot] : 0;
		for (const RecentShot &r : sh.ring) {
			if (r.tick <= shots_checked || r.shooter == slot || r.shooter >= sh.teams.size()) {
				continue;
			}
			const uint8_t other = sh.teams[r.shooter];
			if (other == 0 || other == team) {
				continue;
			}
			if (ray_point_distance(r.origin, r.direction, kShotRayMetres, chest) <= kShotAtMetres) {
				shot_at_tick = p_tick;
				shot_by = r.shooter;
				shot_from = r.origin;
			}
		}
		shots_checked = p_tick;
		if (p_self.health < last_health - 0.01f) {
			damaged_tick = p_tick;
		}
		last_health = p_self.health;
	}

	void perceive(sim::Tick p_tick, const sim::SoldierState &p_self, std::span<const sim::SoldierState> p_states) {
		TdmShared &sh = *shared;
		const ServerView &view = *sh.host.view;
		const uint64_t t0 = now_ns();
		sim::bots::PerceptionInput in;
		in.self = slot;
		in.team = slot < sh.teams.size() ? sh.teams[slot] : 0;
		in.tick = p_tick;
		in.eye = sh.eye_point(p_self);
		in.yaw = sim::quantize_yaw(view_yaw);
		in.pitch = sim::quantize_pitch(view_pitch);
		in.states = p_states;
		in.teams = sh.teams;
		in.alive = sh.alive;
		in.firing = sh.firing;
		in.shots = sh.heard;
		in.world = &view.world();
		in.seed = seed ^ uint64_t(life);
		for (size_t i = 0; i < 3; ++i) {
			in.eye_heights[i] = sh.host.eye_heights[i];
		}
		const uint32_t rays = perception.update(in);
		sh.perception_ns += now_ns() - t0;
		sh.count(&TdmBotStats::perception_updates, uint64_t(1));
		sh.count(&TdmBotStats::rays, uint64_t(rays));
		if (perception.heard_last() > 0) {
			sh.count(&TdmBotStats::heard, uint64_t(perception.heard_last()));
		}
		for (const uint8_t s : perception.newly_detected()) {
			sh.count(&TdmBotStats::detections, uint64_t(1));
			if (sh.observer != nullptr) {
				sh.observer->detected(view, p_tick, slot, s, in.eye);
			}
		}
	}

	bool visible_recent(const sim::bots::PerceivedEnemy *p_e, sim::Tick p_tick) const {
		return p_e != nullptr && p_e->detected && p_e->ever_visible && p_tick - p_e->last_visible <= kVisibleFireTicks;
	}

	void choose_target(sim::Tick p_tick, const sim::SoldierState &p_self, std::span<const sim::SoldierState> p_states) {
		TdmShared &sh = *shared;
		const TdmBotConfig &c = cfg();
		const sim::Vec3 eye = sh.eye_point(p_self);
		const uint32_t memory = sim::seconds_to_ticks(std::max(c.damage_memory_s, 0.0f));
		int best = -1;
		float best_score = -1.0f, cur_score = -1.0f;
		for (const sim::bots::PerceivedEnemy &e : perception.enemies()) {
			if (!visible_recent(&e, p_tick) || e.slot >= p_states.size()) {
				continue;
			}
			const float d = std::max(distance(eye, sh.chest_point(p_states[e.slot])), 1.0f);
			float score = c.w_distance / d;
			if (aims_at_me(p_states[e.slot], p_self)) {
				score += c.w_aim;
			}
			if (shot_by == int(e.slot) && p_tick - shot_at_tick <= memory) {
				score += c.w_damage;
			}
			if (int(e.slot) == target) {
				score += c.w_current;
				cur_score = score;
			}
			if (score > best_score) {
				best_score = score;
				best = int(e.slot);
			}
		}
		if (best < 0 || best == target) {
			return;
		}
		if (target >= 0 && cur_score >= 0.0f && best_score <= cur_score * (1.0f + std::max(c.switch_margin, 0.0f))) {
			return; // not enough better
		}
		const bool had = target >= 0 && cur_score >= 0.0f;
		target = best;
		target_since = p_tick;
		err_next = p_tick;
		const float lo = std::min(c.reaction_min_s, c.reaction_max_s), hi = std::max(c.reaction_min_s, c.reaction_max_s);
		// Uniform by skill: the faster end for the better bots, +-15 % of the range drawn from the life's stream.
		const float base = hi - (hi - lo) * skill01 + (unit(rng) - 0.5f) * 0.3f * (hi - lo);
		const float reaction = std::clamp(base, lo, hi) * (had ? 0.5f : 1.0f); // a switch mid-fight reacts faster
		react_at = p_tick + sim::seconds_to_ticks(reaction);
		loss_handled = false;
		lost_target = -1;
		sh.count(&TdmBotStats::retargets, uint64_t(1));
	}

	// ---- the command ------------------------------------------------------------------------------------------------

	void set_activity(sim::Tick p_tick, TdmBotActivity p_activity) {
		activity = p_activity;
		TdmShared &sh = *shared;
		if (sh.stats != nullptr) {
			sh.stats->activity_ticks[size_t(p_activity)].fetch_add(1, std::memory_order_relaxed);
		}
		if (sh.observer != nullptr) {
			sh.observer->activity(p_tick, slot, p_activity);
		}
	}

	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) {
		TdmShared &sh = *shared;
		const uint64_t t0 = now_ns();
		sh.begin_tick(p_tick);
		sim::Command c = run(p_tick, p_state);
		sh.brains_ns += now_ns() - t0;
		if (sh.stats != nullptr) {
			sh.stats->last_perception_ns.store(sh.perception_ns, std::memory_order_relaxed);
			sh.stats->last_brains_ns.store(sh.brains_ns, std::memory_order_relaxed);
		}
		return c;
	}

	sim::Command run(sim::Tick p_tick, const sim::SoldierState &p_state) {
		TdmShared &sh = *shared;
		const TdmBotConfig &c = cfg();
		sim::Command cmd = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		cmd.flags = 0;
		if (slot == 0xFF || !is_alive(p_state)) {
			target = -1;
			set_activity(p_tick, TdmBotActivity::Dead);
			return cmd; // a dead soldier's command is never stepped; the policy respawns it
		}
		if (p_state.life != life) {
			new_life(p_tick, p_state);
		}
		const ServerView &view = *sh.host.view;
		if (view.input_restricted() || view.phase() != net::MatchPhase::InRound) {
			// Pre-round freeze and round end: idle (the server drops the inputs anyway, [cmd]).
			set_activity(p_tick, TdmBotActivity::Restricted);
			cmd.yaw = sim::quantize_yaw(view_yaw);
			cmd.pitch = sim::quantize_pitch(view_pitch);
			bursting = false;
			return cmd;
		}
		const std::span<const sim::SoldierState> states = view.states();
		const sim::CollisionWorld &world = view.world();
		const sim::Vec3 eye = sh.eye_point(p_state);
		check_shots(p_tick, p_state);
		if (sim::bots::perception_tick(slot, p_tick, c.perception.update_ticks)) {
			perceive(p_tick, p_state, states);
			choose_target(p_tick, p_state, states);
		}
		// The target's state.
		const sim::bots::PerceivedEnemy *te = target >= 0 ? perception.find(uint8_t(target)) : nullptr;
		if (target >= 0 && (te == nullptr || size_t(target) >= states.size() || !view.alive(uint8_t(target)) || p_tick - te->last_visible > kLoseTicks)) {
			target = -1;
			te = nullptr;
		}
		const bool visible = visible_recent(te, p_tick);
		if (visible) {
			last_engaged = p_tick;
		} else if (target >= 0 && !loss_handled) {
			// Just lost: one blind burst at the last-known position (drawn once), or a grenade if behind cover.
			loss_handled = true;
			lost_target = target;
			blind_at = sh.chest_point(states[size_t(target)]);
			blind_at = add(te->last_position, 0.0f, blind_at[1] - states[size_t(target)].position[1], 0.0f);
			if (unit(rng) < c.blind_fire_share) {
				blind_until = p_tick + kBlindTicks;
				sh.count(&TdmBotStats::blind_fire, uint64_t(1));
			}
		}
		const bool shot_recent = shot_at_tick != 0 && p_tick - shot_at_tick <= kShotAtTicks;
		const sw::SoldierWeaponsState *ws = nullptr;
		const sw::WeaponTable *table = nullptr;
		if (sh.host.gunplay != nullptr && sh.host.gunplay->armed(slot)) {
			ws = &sh.host.gunplay->weapons(slot);
			table = &sh.host.gunplay->table();
		}
		const auto slot_def = [&](uint8_t p_slot) -> const sw::WeaponSimDef * {
			if (ws == nullptr || p_slot >= sw::kMaxWeaponSlots) {
				return nullptr;
			}
			const uint16_t d = ws->slots[p_slot].def;
			return d != sw::kNoWeapon && d < table->defs.size() ? &table->defs[d] : nullptr;
		};
		const auto slot_rounds = [&](uint8_t p_slot) -> int32_t {
			const sw::WeaponSimDef *d = slot_def(p_slot);
			if (d == nullptr) {
				return 0;
			}
			return d->unlimited_ammo ? 1000 : ws->slots[p_slot].magazine + ws->slots[p_slot].reserve;
		};

		// ---- activity -----------------------------------------------------------------------------------------------
		TdmBotActivity act = TdmBotActivity::Roam;
		// Revive (§5.6): continue, or start one.
		if (revive_slot >= 0) {
			const DownedSoldier *d = nullptr;
			for (const DownedSoldier &x : view.downed()) {
				if (x.slot == revive_slot) {
					d = &x;
				}
			}
			const bool over = d == nullptr || d->until_tick <= p_tick || p_tick - revive_since > kReviveTimeoutTicks || shot_recent ||
					(visible && te != nullptr && distance(eye, sh.chest_point(states[size_t(target)])) < 40.0f);
			if (over) {
				if (d != nullptr) {
					sh.count(&TdmBotStats::revive_aborts, uint64_t(1));
				}
				revive_slot = -1;
				clear_goal();
			}
		}
		const uint32_t idle_ticks = sim::seconds_to_ticks(std::max(c.revive_idle_s, 0.0f));
		if (revive_slot < 0 && kit == uint8_t(KitRole::Assault) && slot_def(kKitGadgetSlot) != nullptr && p_tick - last_engaged >= idle_ticks &&
				(shot_at_tick == 0 || p_tick - shot_at_tick >= idle_ticks) && state_health_ok(p_state)) {
			float best = 1e30f;
			const uint8_t team = slot < sh.teams.size() ? sh.teams[slot] : 0;
			for (const DownedSoldier &d : view.downed()) {
				if (d.team != team || d.slot == slot || d.until_tick <= p_tick || !finite3(d.corpse)) {
					continue;
				}
				const float dist = distance(p_state.position, d.corpse);
				const float travel = dist * kPathFactor / kRunSpeed + std::max(c.revive_margin_s, 0.0f);
				if (dist > c.revive_radius_m || float(d.until_tick - p_tick) * sim::kTickSeconds <= travel) {
					continue;
				}
				if (dist < best) {
					best = dist;
					revive_slot = d.slot;
				}
			}
			if (revive_slot >= 0) {
				revive_since = p_tick;
				defib_next = p_tick;
				sh.count(&TdmBotStats::revive_attempts, uint64_t(1));
			}
		}
		const DownedSoldier *revive = nullptr;
		if (revive_slot >= 0) {
			for (const DownedSoldier &x : view.downed()) {
				if (x.slot == revive_slot) {
					revive = &x;
				}
			}
		}
		// Retreat and regen (§5.4).
		if (activity == TdmBotActivity::Retreat || activity == TdmBotActivity::RegenWait) {
			if (p_state.health >= c.regen_until_hp || (activity == TdmBotActivity::RegenWait && p_tick - regen_since > sim::seconds_to_ticks(c.regen_wait_s))) {
				activity = TdmBotActivity::Roam; // healed or waited long enough
			}
		}
		const bool low = p_state.health < c.retreat_below_hp;
		if (revive != nullptr) {
			act = TdmBotActivity::Revive;
		} else if (grenade_phase != 0) {
			act = TdmBotActivity::Grenade;
		} else if (activity == TdmBotActivity::Retreat && !visible && p_tick - clear_since >= kRetreatClearTicks) {
			act = TdmBotActivity::RegenWait;
			regen_since = p_tick;
		} else if (activity == TdmBotActivity::Retreat && p_tick - retreat_since < kRetreatMaxTicks && low) {
			act = TdmBotActivity::Retreat;
		} else if (activity == TdmBotActivity::RegenWait && !visible && !shot_recent) {
			act = TdmBotActivity::RegenWait;
		} else if (low && (visible || shot_recent) && sh.nav != nullptr && activity != TdmBotActivity::Retreat && activity != TdmBotActivity::RegenWait &&
				start_retreat(p_tick, p_state, visible ? states[size_t(target)].position : shot_from)) {
			act = TdmBotActivity::Retreat;
		} else if (visible) {
			act = TdmBotActivity::Fight;
		} else if (start_grenade(p_tick, p_state, states, slot_rounds(kSlotGrenade) > 0)) {
			act = TdmBotActivity::Grenade;
		} else if (blind_until > p_tick) {
			act = TdmBotActivity::Fight;
		} else {
			act = known_enemy(p_tick, p_state) >= 0 ? TdmBotActivity::Hunt : TdmBotActivity::Roam;
		}
		if (act == TdmBotActivity::Retreat && visible) {
			clear_since = p_tick;
		}
		set_activity(p_tick, act);

		// ---- movement and aim ---------------------------------------------------------------------------------------
		float move_dx = 0.0f, move_dz = 0.0f;
		bool sprint = false;
		sim::Pose pose = sim::Pose::Stand;
		float want_yaw = view_yaw, want_pitch = 0.0f;
		bool have_aim = false; // aimed at a point (aligned check)
		float target_distance = 0.0f;
		uint8_t want_slot = sidearm ? kSlotSidearm : kSlotPrimary;
		Trigger trigger = Trigger::None;
		const auto aim_at = [&](const sim::Vec3 &p_point) {
			const float dx = p_point[0] - eye[0], dy = p_point[1] - eye[1], dz = p_point[2] - eye[2];
			const float h = std::sqrt(dx * dx + dz * dz);
			want_yaw = yaw_of(dx, dz);
			want_pitch = std::atan2(dy, std::max(h, 1e-3f));
			have_aim = true;
			return std::sqrt(h * h + dy * dy);
		};
		float walk_dx = 0.0f, walk_dz = 0.0f, walk_yaw = view_yaw;
		bool walk_sprint = false;
		sim::Pose walk_pose = sim::Pose::Stand;
		const auto do_walk = [&]() {
			if (walk(p_tick, p_state, walk_dx, walk_dz, walk_yaw, walk_sprint, walk_pose)) {
				move_dx = walk_dx;
				move_dz = walk_dz;
				pose = walk_pose;
				return true;
			}
			bool noticing = false; // an enemy in sight but not yet detected: hold the heading
			for (const sim::bots::PerceivedEnemy &e : perception.enemies()) {
				noticing = noticing || (e.visible && !e.detected);
			}
			if (look_until == 0) {
				look_yaw = view_yaw; // the spawn's heading first
				look_until = p_tick + uniform_u(rng, kLookMinTicks, kLookMaxTicks);
			} else if (noticing) {
				look_yaw = view_yaw;
				look_until = std::max(look_until, p_tick + kLookMinTicks);
			} else if (p_tick >= look_until) {
				const float turn = uniform(rng, kLookMinDeg, kLookMaxDeg) * kDeg;
				look_yaw = wrap(view_yaw + (unit(rng) < 0.5f ? -turn : turn));
				look_until = p_tick + uniform_u(rng, kLookMinTicks, kLookMaxTicks);
			}
			walk_yaw = look_yaw;
			return false;
		};
		if (act != TdmBotActivity::Fight) {
			strafing = false;
		}
		switch (act) {
			case TdmBotActivity::Fight: {
				if (visible) {
					const sim::SoldierState &ts = states[size_t(target)];
					const sim::Vec3 chest = sh.chest_point(ts);
					const sw::WeaponSimDef *d = slot_def(ws != nullptr ? ws->active : 0);
					sim::Vec3 point = chest;
					if (d != nullptr && !d->projectile.instant_hit) {
						point = sim::bots::lead_point(eye, chest, ts.velocity, d->projectile.muzzle_speed, d->projectile.gravity);
					}
					target_distance = aim_at(point);
					// The error cone (§5.3).
					const sim::Vec3 los = sub(chest, eye);
					const float l = std::max(distance(eye, chest), 1e-3f);
					const float along = (ts.velocity[0] * los[0] + ts.velocity[1] * los[1] + ts.velocity[2] * los[2]) / l;
					const float vx = ts.velocity[0] - along * los[0] / l, vy = ts.velocity[1] - along * los[1] / l, vz = ts.velocity[2] - along * los[2] / l;
					const float crossing = std::sqrt(vx * vx + vy * vy + vz * vz);
					const float own = std::sqrt(p_state.velocity[0] * p_state.velocity[0] + p_state.velocity[2] * p_state.velocity[2]);
					const float on = float(p_tick - target_since) * sim::kTickSeconds;
					const float sigma = sim::bots::aim_error(c.aim, skill, on, own > 0.5f, std::isfinite(crossing) ? crossing : 0.0f) * kDeg;
					if (p_tick >= err_next) {
						err_yaw = gauss(rng) * sigma;
						err_pitch = gauss(rng) * sigma * kErrorPitchShare;
						err_next = p_tick + kErrorTicks;
					}
					want_yaw = wrap(want_yaw + err_yaw);
					want_pitch += err_pitch;
					trigger = Trigger::Aimed;
					// Knife (§5.5): within reach from behind.
					const float hd = horizontal(p_state.position, ts.position);
					if (hd <= c.knife_within_m && slot_def(kSlotKnife) != nullptr) {
						const float behind = std::fabs(wrap(yaw_of(p_state.position[0] - ts.position[0], p_state.position[2] - ts.position[2]) - sim::yaw_radians(ts.yaw)));
						if (behind >= kKnifeBehindDeg * kDeg && knife_until <= p_tick) {
							knife_until = p_tick + kKnifeTicks;
							sh.count(&TdmBotStats::knife_attacks, uint64_t(1));
						}
					}
					if (knife_until > p_tick) {
						want_slot = kSlotKnife;
						trigger = Trigger::Knife;
						aim_at(chest);
					}
					// Movement while engaged: strafe close, push in from range, hold and crouch far away (skilled).
					if (target_distance < c.engage_strafe_m) {
						strafe(p_tick, world, p_state, move_dx, move_dz);
					} else {
						strafing = false;
						if (target_distance > c.crouch_beyond_m && skill > 0.5f) {
							pose = sim::Pose::Crouch;
						} else if (set_goal(p_tick, ts.position, kHuntArriveMetres, false)) {
							do_walk();
							pose = sim::Pose::Stand;
						}
					}
				} else {
					// Blind fire at the lost target's last-known chest.
					target_distance = aim_at(blind_at);
					trigger = Trigger::Blind;
				}
				break;
			}
			case TdmBotActivity::Revive: {
				const sim::Vec3 centre = add(revive->corpse, 0.0f, kReviveCentreUp, 0.0f);
				const float hd = horizontal(p_state.position, revive->corpse);
				if (hd > kReviveDirectMetres) {
					set_goal(p_tick, revive->corpse, 1.0f, true);
					do_walk();
					want_yaw = walk_yaw;
				} else {
					if (hd > kReviveStandMetres) {
						const float inv = 0.6f / std::max(hd, 1e-3f);
						move_dx = (revive->corpse[0] - p_state.position[0]) * inv;
						move_dz = (revive->corpse[2] - p_state.position[2]) * inv;
					}
					pose = hd < kReviveCrouchMetres ? sim::Pose::Crouch : sim::Pose::Stand;
					want_slot = kKitGadgetSlot;
					const float reach = aim_at(centre);
					if (reach <= kDefibReach) {
						trigger = Trigger::Defib;
					}
				}
				break;
			}
			case TdmBotActivity::Retreat:
				do_walk();
				want_yaw = walk_yaw;
				sprint = walk_sprint;
				break;
			case TdmBotActivity::RegenWait:
				pose = sim::Pose::Crouch;
				want_yaw = yaw_of(threat[0] - p_state.position[0], threat[2] - p_state.position[2]);
				break;
			case TdmBotActivity::Grenade:
				grenade_step(p_tick, eye, want_slot, trigger, want_yaw, want_pitch, have_aim);
				break;
			case TdmBotActivity::Hunt: {
				const int k = known_enemy(p_tick, p_state);
				if (k >= 0) {
					const sim::bots::PerceivedEnemy *e = perception.find(uint8_t(k));
					if (e != nullptr) {
						if (horizontal(p_state.position, e->last_position) <= kHuntArriveMetres) {
							searched_tick = p_tick; // nobody here: forget what was known before now
						} else {
							set_goal(p_tick, e->last_position, kHuntArriveMetres, false);
						}
					}
				}
				do_walk();
				want_yaw = walk_yaw;
				sprint = walk_sprint;
				break;
			}
			case TdmBotActivity::Roam:
			default: {
				if (!have_goal || roam_tick == 0 || p_tick - roam_tick >= kRoamGoalTicks) {
					roam_tick = p_tick;
					if (const std::optional<sim::Vec3> h = heat_goal(p_tick, p_state.position)) {
						if (set_goal(p_tick, *h, kHuntArriveMetres, true)) {
							sh.count(&TdmBotStats::heat_goals, uint64_t(1));
						}
					} else {
						clear_goal(); // S1's random nav points
					}
				}
				do_walk();
				want_yaw = walk_yaw;
				sprint = walk_sprint;
				break;
			}
		}
		// Turn towards gunfire that came from out of sight (so the next perception sees the shooter).
		if (!have_aim && shot_recent && act != TdmBotActivity::Retreat && act != TdmBotActivity::Revive) {
			want_yaw = yaw_of(shot_from[0] - p_state.position[0], shot_from[2] - p_state.position[2]);
			sprint = false;
		}

		// ---- turn ---------------------------------------------------------------------------------------------------
		const float yaw_step = kTurnRateDegS * kDeg * sim::kTickSeconds;
		const float pitch_step = kPitchTurnRateDegS * kDeg * sim::kTickSeconds;
		view_yaw = wrap(view_yaw + std::clamp(wrap(want_yaw - view_yaw), -yaw_step, yaw_step));
		want_pitch = std::clamp(want_pitch, -kMaxPitchDeg * kDeg, kMaxPitchDeg * kDeg);
		view_pitch += std::clamp(want_pitch - view_pitch, -pitch_step, pitch_step);
		if (!std::isfinite(view_yaw) || !std::isfinite(view_pitch)) {
			view_yaw = sim::yaw_radians(p_state.yaw);
			view_pitch = 0.0f;
		}
		const float cone = (trigger == Trigger::Defib ? kDefibConeDeg : kFireConeDeg) * kDeg;
		const bool aligned = have_aim && std::fabs(wrap(want_yaw - view_yaw)) <= cone && std::fabs(want_pitch - view_pitch) <= cone;
		if (sprint) {
			const float len = std::sqrt(move_dx * move_dx + move_dz * move_dz);
			sprint = !have_aim && len > 0.5f && std::fabs(wrap(yaw_of(move_dx, move_dz) - view_yaw)) <= kSprintConeDeg * kDeg;
		}

		// ---- weapon -------------------------------------------------------------------------------------------------
		sw::WeaponInput in;
		in.fire_mode_counter = mode_counter;
		float comp_yaw = 0.0f, comp_pitch = 0.0f;
		if (ws != nullptr) {
			// The sidearm when the primary is empty for good; back to the primary never (one life).
			if (!sidearm && slot_def(kSlotPrimary) != nullptr && slot_rounds(kSlotPrimary) <= 0 && slot_rounds(kSlotSidearm) > 0) {
				sidearm = true;
				sh.count(&TdmBotStats::sidearm_switches, uint64_t(1));
				if (want_slot == kSlotPrimary) {
					want_slot = kSlotSidearm;
				}
			}
			if (slot_def(want_slot) == nullptr) {
				want_slot = slot_def(kSlotPrimary) != nullptr ? kSlotPrimary : ws->active;
				if (trigger == Trigger::Defib || trigger == Trigger::Knife || trigger == Trigger::Grenade) {
					trigger = Trigger::None;
				}
			}
			in.select_slot = want_slot;
			const sw::WeaponSlotState &ss = ws->slots[ws->active < sw::kMaxWeaponSlots ? ws->active : 0];
			const sw::WeaponSimDef *d = slot_def(ws->active);
			if (d != nullptr) {
				const uint8_t modes = std::min<uint8_t>(d->mode_count, uint8_t(d->modes.size()));
				int auto_mode = -1;
				for (uint8_t k = 0; k < modes; ++k) {
					if (d->modes[k].logic == core::gameplay::FireLogic::Automatic) {
						auto_mode = k;
						break;
					}
				}
				const sw::WeaponPhase phase = ss.phase;
				if ((ws->active == kSlotPrimary || ws->active == kSlotSidearm) && auto_mode >= 0 && ss.mode != uint8_t(auto_mode) && phase == sw::WeaponPhase::Ready &&
						p_tick > life_tick + kModeDelayTicks && p_tick >= mode_wait && modes > 1) {
					const uint8_t steps = uint8_t((uint32_t(auto_mode) + modes - ss.mode) % modes);
					if (steps > 0 && steps <= 3) {
						mode_counter = uint8_t((mode_counter + steps) & 3u);
						in.fire_mode_counter = mode_counter;
						mode_wait = p_tick + kModeWaitTicks;
					}
				}
				const sw::FireModeDef &fm = d->modes[ss.mode < modes ? ss.mode : 0];
				const bool automatic = fm.logic == core::gameplay::FireLogic::Automatic;
				const bool busy = phase == sw::WeaponPhase::Deploying || phase == sw::WeaponPhase::Reloading || phase == sw::WeaponPhase::ReloadShell ||
						phase == sw::WeaponPhase::None;
				const bool in_hand = ws->active == want_slot && ws->pending == 0xFF;
				const bool reserve = ss.reserve > 0 || d->unlimited_ammo;
				const bool gun = ws->active == kSlotPrimary || ws->active == kSlotSidearm;
				const bool need = gun && !busy && ss.magazine <= 0 && reserve;
				const float capacity = float(std::max(d->magazine_capacity, 1));
				const bool quiet = !visible && p_tick - last_engaged > 120u && act != TdmBotActivity::Revive;
				const bool tactical = gun && quiet && !busy && reserve && float(ss.magazine) < kTacticalReload * capacity;
				const bool reload = need || tactical;
				if (reload && !reload_held) {
					sh.count(&TdmBotStats::reloads, uint64_t(1));
				}
				reload_held = reload;
				in.reload = reload;
				bool fire = false;
				if (trigger == Trigger::Grenade) {
					fire = grenade_phase == 2 && in_hand; // held through the hold phase, released after
				} else if (trigger == Trigger::Defib) {
					if (in_hand && aligned && phase == sw::WeaponPhase::Ready && p_tick >= defib_next && !tapped) {
						fire = true;
						tapped = true;
						defib_next = p_tick + kDefibRetryTicks;
						sh.count(&TdmBotStats::revive_shots, uint64_t(1));
					} else {
						tapped = false;
					}
				} else if (trigger == Trigger::Knife) {
					if (in_hand && aligned && phase == sw::WeaponPhase::Ready && !tapped) {
						fire = true;
						tapped = true;
					} else {
						tapped = false;
					}
				} else if (trigger == Trigger::Aimed || trigger == Trigger::Blind) {
					const bool ready = trigger == Trigger::Blind || p_tick >= react_at;
					const bool want_fire = in_hand && aligned && ready && !busy && !reload && ss.magazine > 0 && !sprint;
					const bool single = target_distance > kSingleShotBeyondMetres || trigger == Trigger::Blind;
					if (automatic && !single) {
						if (bursting) {
							const uint32_t fired = ws->next_shot_index - burst_start;
							if (fired >= burst_rounds || !want_fire) {
								bursting = false;
								pause_until = p_tick + sim::seconds_to_ticks(uniform(rng, 0.3f, 0.6f));
							} else {
								fire = true;
							}
						} else if (want_fire && p_tick >= pause_until) {
							// Bursts by range: burst_max rounds close, burst_min at 60 m.
							const float share = std::clamp(target_distance / kSingleShotBeyondMetres, 0.0f, 1.0f);
							const uint32_t lo = std::max(c.burst_min_rounds, 1u), hi = std::max(c.burst_max_rounds, lo);
							const uint32_t top = std::max(lo, uint32_t(std::lround(float(hi) - share * float(hi - lo))));
							burst_rounds = uniform_u(rng, lo, top);
							burst_start = ws->next_shot_index;
							bursting = true;
							fire = true;
							sh.count(&TdmBotStats::bursts, uint64_t(1));
						}
					} else if (tapped) {
						tapped = false; // the release tick
					} else if (want_fire && p_tick >= next_tap) {
						fire = true;
						tapped = true;
						const float rate = target_distance > kSingleShotBeyondMetres ? 1.5f : 3.0f;
						next_tap = p_tick + std::max<uint32_t>(2u, uint32_t(std::lround(float(sim::kSimHz) / rate)));
						sh.count(&TdmBotStats::bursts, uint64_t(1));
					}
					if (fire && sh.observer != nullptr) {
						sh.observer->fired(view, p_tick, slot, trigger == Trigger::Aimed ? target : -1, te != nullptr ? te->last_visible : 0, trigger == Trigger::Blind);
					}
				}
				in.fire = fire;
				// Recoil compensation (§5.3): skill x the kick, on the command only (the view keeps its own aim).
				if (gun) {
					comp_yaw = skill * ss.recoil.yaw * kDeg;
					comp_pitch = skill * ss.recoil.pitch * kDeg;
				}
			}
		}

		// ---- the command --------------------------------------------------------------------------------------------
		cmd.yaw = sim::quantize_yaw(wrap(view_yaw - comp_yaw));
		cmd.pitch = sim::quantize_pitch(std::clamp(view_pitch - comp_pitch, -kMaxPitchDeg * kDeg, kMaxPitchDeg * kDeg));
		cmd.pose = pose;
		const float sy = std::sin(view_yaw), cy = std::cos(view_yaw);
		const float fwd = move_dx * sy + move_dz * cy;
		const float rgt = -move_dx * cy + move_dz * sy;
		cmd.move_forward = sim::quantize_axis(std::clamp(std::isfinite(fwd) ? fwd : 0.0f, -1.0f, 1.0f));
		cmd.move_right = sim::quantize_axis(std::clamp(std::isfinite(rgt) ? rgt : 0.0f, -1.0f, 1.0f));
		uint32_t b = sprint ? sim::buttons::kSprint : 0u;
		b = sw::with_weapon_input(b, in);
		cmd.buttons = b;
		return cmd;
	}

	bool state_health_ok(const sim::SoldierState &p_state) const { return p_state.health >= cfg().retreat_below_hp; }

	// The nearest known enemy (seen or heard) not searched since; -1 for none.
	int known_enemy(sim::Tick p_tick, const sim::SoldierState &p_self) const {
		(void)p_tick;
		float best = 1e30f;
		int pick = -1;
		for (const sim::bots::PerceivedEnemy &e : perception.enemies()) {
			if (!(e.detected || e.heard) || e.last_seen < searched_tick || !finite3(e.last_position)) {
				continue;
			}
			const float d = horizontal(p_self.position, e.last_position);
			if (d < best) {
				best = d;
				pick = int(e.slot);
			}
		}
		return pick;
	}

	bool start_retreat(sim::Tick p_tick, const sim::SoldierState &p_self, const sim::Vec3 &p_threat) {
		if (!finite3(p_threat)) {
			return false;
		}
		float dx = p_self.position[0] - p_threat[0], dz = p_self.position[2] - p_threat[2];
		const float l = std::sqrt(dx * dx + dz * dz);
		if (!(l > 0.1f)) {
			const float a = unit(rng) * sim::kTwoPi;
			dx = std::sin(a);
			dz = std::cos(a);
		} else {
			dx /= l;
			dz /= l;
		}
		const sim::Vec3 away = add(p_self.position, dx * kRetreatMetres, 0.0f, dz * kRetreatMetres);
		if (!set_goal(p_tick, away, 2.0f, true)) {
			return false;
		}
		threat = p_threat;
		retreat_since = p_tick;
		clear_since = p_tick;
		shared->count(&TdmBotStats::retreats, uint64_t(1));
		return true;
	}

	// §5.5: an M67 at a lost target behind cover 15-35 m away, at most once per grenade_every_s.
	bool start_grenade(sim::Tick p_tick, const sim::SoldierState &p_self, std::span<const sim::SoldierState> p_states, bool p_have) {
		const TdmBotConfig &c = cfg();
		if (!p_have || lost_target < 0 || p_tick < grenade_next || size_t(lost_target) >= p_states.size()) {
			return false;
		}
		const sim::bots::PerceivedEnemy *e = perception.find(uint8_t(lost_target));
		if (e == nullptr || !e->ever_visible) {
			return false;
		}
		const uint32_t lost = p_tick - e->last_visible;
		if (lost < kGrenadeLostMinTicks || lost > kGrenadeLostMaxTicks) {
			return false;
		}
		const float d = horizontal(p_self.position, e->last_position);
		if (d < c.grenade_min_m || d > c.grenade_max_m) {
			return false;
		}
		grenade_next = p_tick + sim::seconds_to_ticks(std::max(c.grenade_every_s, 1.0f));
		grenade_phase = 1;
		grenade_phase_tick = p_tick;
		grenade_at = e->last_position;
		lost_target = -1;
		shared->count(&TdmBotStats::grenades, uint64_t(1));
		return true;
	}

	void grenade_step(sim::Tick p_tick, const sim::Vec3 &p_eye, uint8_t &r_slot, Trigger &r_trigger, float &r_yaw, float &r_pitch, bool &r_aim) {
		TdmShared &sh = *shared;
		const Gunplay *g = sh.host.gunplay;
		const sw::SoldierWeaponsState *ws = g != nullptr && g->armed(slot) ? &g->weapons(slot) : nullptr;
		const sw::WeaponSimDef *d = nullptr;
		if (ws != nullptr && ws->slots[kSlotGrenade].def != sw::kNoWeapon && ws->slots[kSlotGrenade].def < g->table().defs.size()) {
			d = &g->table().defs[ws->slots[kSlotGrenade].def];
		}
		if (d == nullptr) {
			grenade_phase = 0;
			return;
		}
		// The throw angle for the distance (the low solution of the drag-free arc, 10..45 degrees).
		const float dx = grenade_at[0] - p_eye[0], dz = grenade_at[2] - p_eye[2];
		const float h = std::sqrt(dx * dx + dz * dz);
		const float v = std::max(d->projectile.muzzle_speed * std::max(d->hold_min_power, 0.1f), 1.0f);
		const float gr = std::max(-d->projectile.gravity, 0.1f);
		const float s = std::clamp(gr * h / (v * v), 0.0f, 1.0f);
		r_yaw = yaw_of(dx, dz);
		r_pitch = std::clamp(0.5f * std::asin(s), 10.0f * kDeg, 45.0f * kDeg);
		r_aim = true;
		r_slot = kSlotGrenade;
		r_trigger = Trigger::Grenade;
		const sw::WeaponSlotState &ss = ws->slots[kSlotGrenade];
		const bool in_hand = ws->active == kSlotGrenade && ws->pending == 0xFF;
		if (grenade_phase == 1) {
			if (in_hand && ss.phase == sw::WeaponPhase::Ready) {
				grenade_phase = 2;
				grenade_phase_tick = p_tick;
				grenade_hold = uint32_t(std::max<sw::SimTime>(d->hold_delay, 0) * sim::kSimHz / sw::kTimeUnitsPerSecond) + kGrenadeHoldExtraTicks;
			} else if (p_tick - grenade_phase_tick > kGrenadeSwitchTicks) {
				grenade_phase = 0; // never came up: back to the gun
			}
		} else if (grenade_phase == 2) {
			if (p_tick - grenade_phase_tick >= grenade_hold) {
				grenade_phase = 3; // release: the throw
				grenade_phase_tick = p_tick;
			}
		} else if (grenade_phase == 3 && p_tick - grenade_phase_tick >= kGrenadeAfterTicks) {
			grenade_phase = 0;
		}
	}
};

TdmBrain::TdmBrain(std::shared_ptr<TdmShared> p_shared, uint32_t p_index) :
		impl_(std::make_unique<Impl>()) {
	impl_->shared = std::move(p_shared);
	impl_->index = p_index;
	if (impl_->shared != nullptr) {
		impl_->perception = sim::bots::Perception(impl_->shared->config->perception);
	}
}

TdmBrain::~TdmBrain() = default;

sim::Command TdmBrain::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	if (impl_->shared == nullptr) {
		return sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
	}
	return impl_->command(p_tick, p_state);
}

void TdmBrain::set_slot(uint8_t p_slot) {
	Impl &m = *impl_;
	m.slot = p_slot;
	m.seed = (m.shared != nullptr ? m.shared->host.match_seed : 0) ^ uint64_t(p_slot); // §5.7: bot seed = match_seed ^ slot
	m.walker.reset();
	m.life = UINT32_MAX;
}

// ---- config/bot_tuning.cfg -------------------------------------------------------------------------------------------

namespace {

struct FloatKey {
	const char *name;
	float TdmBotConfig::*field;
	float lo, hi;
};
struct NestedFloatKey {
	const char *name;
	bool aim; // aim.* (else perception.*)
	float sim::bots::PerceptionParams::*pfield;
	float sim::bots::AimParams::*afield;
	float lo, hi;
};

constexpr FloatKey kFloatKeys[] = {
	{ "skill_min", &TdmBotConfig::skill_min, 0.0f, 1.0f },
	{ "skill_max", &TdmBotConfig::skill_max, 0.0f, 1.0f },
	{ "reaction_min_s", &TdmBotConfig::reaction_min_s, 0.0f, 10.0f },
	{ "reaction_max_s", &TdmBotConfig::reaction_max_s, 0.0f, 10.0f },
	{ "switch_margin", &TdmBotConfig::switch_margin, 0.0f, 10.0f },
	{ "strafe_min_m", &TdmBotConfig::strafe_min_m, 0.0f, 50.0f },
	{ "strafe_max_m", &TdmBotConfig::strafe_max_m, 0.0f, 50.0f },
	{ "crouch_beyond_m", &TdmBotConfig::crouch_beyond_m, 0.0f, 1000.0f },
	{ "retreat_below_hp", &TdmBotConfig::retreat_below_hp, 0.0f, 100.0f },
	{ "grenade_min_m", &TdmBotConfig::grenade_min_m, 0.0f, 200.0f },
	{ "grenade_max_m", &TdmBotConfig::grenade_max_m, 0.0f, 200.0f },
	{ "grenade_every_s", &TdmBotConfig::grenade_every_s, 1.0f, 3600.0f },
	{ "knife_within_m", &TdmBotConfig::knife_within_m, 0.0f, 10.0f },
	{ "heat_cell_m", &TdmBotConfig::heat_cell_m, 1.0f, 1000.0f },
	{ "heat_decay_s", &TdmBotConfig::heat_decay_s, 0.1f, 3600.0f },
	{ "revive_radius_m", &TdmBotConfig::revive_radius_m, 0.0f, 500.0f },
	{ "revive_idle_s", &TdmBotConfig::revive_idle_s, 0.0f, 60.0f },
	{ "revive_margin_s", &TdmBotConfig::revive_margin_s, 0.0f, 60.0f },
	{ "w_distance", &TdmBotConfig::w_distance, 0.0f, 1000.0f },
	{ "w_aim", &TdmBotConfig::w_aim, 0.0f, 1000.0f },
	{ "w_damage", &TdmBotConfig::w_damage, 0.0f, 1000.0f },
	{ "w_current", &TdmBotConfig::w_current, 0.0f, 1000.0f },
	{ "damage_memory_s", &TdmBotConfig::damage_memory_s, 0.0f, 60.0f },
	{ "aims_at_me_deg", &TdmBotConfig::aims_at_me_deg, 0.0f, 90.0f },
	{ "engage_strafe_m", &TdmBotConfig::engage_strafe_m, 0.0f, 1000.0f },
	{ "hunt_share", &TdmBotConfig::hunt_share, 0.0f, 1.0f },
	{ "blind_fire_share", &TdmBotConfig::blind_fire_share, 0.0f, 1.0f },
	{ "regen_wait_s", &TdmBotConfig::regen_wait_s, 0.0f, 600.0f },
	{ "regen_until_hp", &TdmBotConfig::regen_until_hp, 0.0f, 100.0f },
};

using PP = sim::bots::PerceptionParams;
using AP = sim::bots::AimParams;
constexpr NestedFloatKey kNestedKeys[] = {
	{ "perception.fov_h_deg", false, &PP::fov_h_deg, nullptr, 1.0f, 360.0f },
	{ "perception.fov_v_deg", false, &PP::fov_v_deg, nullptr, 1.0f, 180.0f },
	{ "perception.max_range_m", false, &PP::max_range_m, nullptr, 0.0f, 2000.0f },
	{ "perception.near_m", false, &PP::near_m, nullptr, 0.0f, 2000.0f },
	{ "perception.detect_near_s", false, &PP::detect_near_s, nullptr, 0.0f, 60.0f },
	{ "perception.far_m", false, &PP::far_m, nullptr, 0.0f, 2000.0f },
	{ "perception.detect_far_s", false, &PP::detect_far_s, nullptr, 0.0f, 60.0f },
	{ "perception.forget_s", false, &PP::forget_s, nullptr, 0.0f, 600.0f },
	{ "perception.hearing_radius_m", false, &PP::hearing_radius_m, nullptr, 0.0f, 2000.0f },
	{ "perception.hearing_error_m", false, &PP::hearing_error_m, nullptr, 0.0f, 200.0f },
	{ "perception.head_height_m", false, &PP::head_height_m, nullptr, 0.0f, 3.0f },
	{ "perception.chest_height_m", false, &PP::chest_height_m, nullptr, 0.0f, 3.0f },
	{ "aim.e0_deg", true, nullptr, &AP::e0_deg, 0.0f, 45.0f },
	{ "aim.e_inf_deg", true, nullptr, &AP::e_inf_deg, 0.0f, 45.0f },
	{ "aim.tau_s", true, nullptr, &AP::tau_s, 0.001f, 60.0f },
	{ "aim.moving_penalty_deg", true, nullptr, &AP::moving_penalty_deg, 0.0f, 45.0f },
	{ "aim.crossing_deg_per_mps", true, nullptr, &AP::crossing_deg_per_mps, 0.0f, 45.0f },
	{ "aim.skill_scale", true, nullptr, &AP::skill_scale, 0.0f, 10.0f },
};

std::string_view trim(std::string_view p_s) {
	while (!p_s.empty() && (p_s.front() == ' ' || p_s.front() == '\t' || p_s.front() == '\r')) {
		p_s.remove_prefix(1);
	}
	while (!p_s.empty() && (p_s.back() == ' ' || p_s.back() == '\t' || p_s.back() == '\r')) {
		p_s.remove_suffix(1);
	}
	return p_s;
}

bool parse_number(std::string_view p_text, double &r_value) {
	if (p_text.empty() || p_text.size() > 32) {
		return false;
	}
	char buf[40];
	std::memcpy(buf, p_text.data(), p_text.size());
	buf[p_text.size()] = '\0';
	char *end = nullptr;
	const double v = std::strtod(buf, &end);
	if (end == buf || *end != '\0' || !std::isfinite(v)) {
		return false;
	}
	r_value = v;
	return true;
}

} // namespace

core::Status parse_bot_tuning(std::string_view p_text, TdmBotConfig &r_config) {
	if (p_text.size() > kMaxBotTuningBytes) {
		return core::make_error("bot_tuning: " + std::to_string(p_text.size()) + " bytes, more than " + std::to_string(kMaxBotTuningBytes));
	}
	TdmBotConfig cfg = r_config;
	size_t line_no = 0;
	while (!p_text.empty()) {
		const size_t nl = p_text.find('\n');
		std::string_view line = p_text.substr(0, nl);
		p_text = nl == std::string_view::npos ? std::string_view() : p_text.substr(nl + 1);
		++line_no;
		if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
			line = line.substr(0, hash);
		}
		line = trim(line);
		if (line.empty()) {
			continue;
		}
		const size_t eq = line.find('=');
		const std::string where = "bot_tuning line " + std::to_string(line_no);
		if (eq == std::string_view::npos) {
			return core::make_error(where + ": no '='");
		}
		const std::string_view key = trim(line.substr(0, eq));
		double v = 0.0;
		if (!parse_number(trim(line.substr(eq + 1)), v)) {
			return core::make_error(where + ": '" + std::string(key) + "' needs a finite number");
		}
		bool done = false;
		const auto range = [&](double p_lo, double p_hi) -> core::Status {
			if (v < p_lo || v > p_hi) {
				return core::make_error(where + ": '" + std::string(key) + "' = " + std::to_string(v) + " is outside [" + std::to_string(p_lo) + ", " +
						std::to_string(p_hi) + "]");
			}
			return core::Status::success();
		};
		for (const FloatKey &k : kFloatKeys) {
			if (key == k.name) {
				BF3_TRY(range(k.lo, k.hi));
				cfg.*(k.field) = float(v);
				done = true;
			}
		}
		for (const NestedFloatKey &k : kNestedKeys) {
			if (key == k.name) {
				BF3_TRY(range(k.lo, k.hi));
				if (k.aim) {
					cfg.aim.*(k.afield) = float(v);
				} else {
					cfg.perception.*(k.pfield) = float(v);
				}
				done = true;
			}
		}
		if (key == "count") {
			BF3_TRY(range(0, 64));
			cfg.count = uint32_t(v);
			done = true;
		} else if (key == "burst_min_rounds") {
			BF3_TRY(range(1, 100));
			cfg.burst_min_rounds = uint32_t(v);
			done = true;
		} else if (key == "burst_max_rounds") {
			BF3_TRY(range(1, 100));
			cfg.burst_max_rounds = uint32_t(v);
			done = true;
		} else if (key == "perception.update_ticks") {
			BF3_TRY(range(1, 600));
			cfg.perception.update_ticks = uint32_t(v);
			done = true;
		} else if (key == "perception.max_candidates") {
			BF3_TRY(range(1, 64));
			cfg.perception.max_candidates = uint32_t(v);
			done = true;
		}
		if (!done) {
			return core::make_error(where + ": unknown key '" + std::string(key) + "'");
		}
	}
	r_config = std::move(cfg);
	return core::Status::success();
}

core::Status load_bot_tuning(const std::string &p_path, TdmBotConfig &r_config) {
	std::ifstream f(p_path, std::ios::binary);
	if (!f) {
		return core::make_error("bot_tuning: cannot open " + p_path);
	}
	std::string text;
	text.resize(kMaxBotTuningBytes + 1);
	f.read(text.data(), std::streamsize(text.size()));
	text.resize(size_t(f.gcount()));
	const core::Status st = parse_bot_tuning(text, r_config);
	if (!st.ok()) {
		return core::error_at(p_path, st.error());
	}
	return core::Status::success();
}

// ---- the hook --------------------------------------------------------------------------------------------------------

core::Status add_tdm_bots(GameServer &r_server, const GameServerConfig &p_config) {
	if (p_config.tdm_bots == nullptr || p_config.tdm_bots->count == 0) {
		return core::Status::success();
	}
	if (p_config.level == nullptr) {
		return core::make_error("tdm bots: no level");
	}
	std::shared_ptr<const TdmBotConfig> config = p_config.tdm_bots;
	if (!config->tuning_cfg.empty()) {
		auto tuned = std::make_shared<TdmBotConfig>(*config);
		BF3_TRY(load_bot_tuning(config->tuning_cfg, *tuned));
		tuned->count = config->count; // the caller's count wins over the file's
		config = tuned;
	}
	TdmBrainHost host;
	host.view = &r_server.view();
	host.gunplay = r_server.gunplay();
	host.match_seed = r_server.gunplay() != nullptr ? r_server.gunplay()->match_seed() : p_config.seed;
	host.rules = p_config.rules.mode == MatchRulesConfig::Mode::Tdm;
	for (size_t i = 0; i < 3; ++i) {
		if (p_config.level->motor.poses[i].eye_height > 0.0f) {
			host.eye_heights[i] = p_config.level->motor.poses[i].eye_height;
		}
	}
	if (p_config.bots != nullptr && p_config.bots->level != nullptr) {
		host.nav = p_config.bots->level;
		host.walker = p_config.bots->walker;
	}
	// The heat grid centred on the level's spawn points.
	if (!p_config.level->spawns.empty()) {
		double sx = 0.0, sz = 0.0;
		for (const sim::SpawnPoint &p : p_config.level->spawns) {
			sx += p.position[0];
			sz += p.position[2];
		}
		const float half = 0.5f * float(kHeatCells) * std::max(config->heat_cell_m, 1.0f);
		const float cx = float(sx / double(p_config.level->spawns.size())), cz = float(sz / double(p_config.level->spawns.size()));
		if (std::isfinite(cx) && std::isfinite(cz)) {
			host.heat_origin = { cx - half, 0.0f, cz - half };
		}
	}
	core::Result<std::shared_ptr<TdmShared>> made = make_tdm_shared(host, config);
	if (!made) {
		return made.error();
	}
	std::shared_ptr<TdmShared> shared = std::move(*made);
	// Slots below 64 only (spectators above with rules on, §3.2).
	const GameServerStats st = r_server.stats();
	const size_t limit = std::min<size_t>({ st.slots.size(), size_t(p_config.max_slots), size_t(net::kMaxPlayerRows) });
	uint32_t free_below = 0;
	for (size_t s = 0; s < limit; ++s) {
		free_below += st.slots[s].used ? 0u : 1u;
	}
	if (st.slots.empty()) {
		free_below = std::min<uint32_t>(p_config.max_slots, uint32_t(net::kMaxPlayerRows));
	}
	const uint32_t count = std::min(config->count, free_below);
	for (uint32_t i = 0; i < count; ++i) {
		auto brain = std::make_unique<TdmBrain>(shared, i);
		TdmBrain *raw = brain.get();
		// Rules: team 0 = AutoTeam's choice ([add]). Without rules M2a's firefight teams (every team gets every kit).
		const int team = host.rules ? 0 : 1 + int((i + i / 4u) % 2u);
		const int slot = r_server.add_local_soldier(std::move(brain), team);
		if (slot < 0) {
			return core::make_error("tdm bots: no slot for bot " + std::to_string(i) + " of " + std::to_string(count));
		}
		raw->set_slot(uint8_t(slot));
		if (shared->stats != nullptr) {
			shared->stats->bots.fetch_add(1, std::memory_order_relaxed);
		}
	}
	return core::Status::success();
}

} // namespace bf3::host

#pragma once
// The S1 walker brain (spike S1, plan §3.6 "Brains ... reads an immutable ISimView, writes Commands only", D11; docs/
// design/s1.md §7): a server-side bot that walks between reachable goals on the navmesh, with no combat. Godot-free.
//
// Contract: a brain reads a WalkerView (its own authoritative state, the tick, the static goal set and the navmesh
// through the server thread's shared NavQuery and PathService) and returns the Command of the next tick, the same
// struct a human's client sends (command.h). It never writes simulation state, never teleports, never reads another
// soldier. The motor then steps the command like any other (game_server.h tick step 3-4).
//
// Behaviour (docs §7):
//   goals      alternately a random nav point of the main component (NavQuery::random_point) and a mode objective
//              (flags, HQs, MCOMs, spawn points; objective_share), only goals whose nearest poly is in the main
//              component, so every goal is reachable from every other (the component is strongly connected);
//   path       a PathService request (sliced A*, a shared per-tick iteration budget across all bots), then a
//              NavCorridor along it;
//   steering   every tick, cheap (no nav query): the yaw towards the corridor's next corner, turned at most
//              yaw_rate_deg_s, and the move axes = the remaining direction in the yaw's frame (forward and right, so a
//              bot walks while it turns); sprint when the next corners are straight and on kNavAreaWalk, crouch on
//              kNavAreaCrouch ahead (and a crouch_share of the legs anyway), stand otherwise;
//   think      on the brain's staggered tick (think_hz, phase = slot % period): move the corridor to the bot's
//              position (one move_position), refresh the corners, check arrival, progress and stuck;
//   stuck      commanded movement with less than stuck_progress metres of progress over stuck_seconds: repath from
//              the current position; after max_repaths in a row, a new goal; a bot off the mesh (no poly within 2 m)
//              walks back to the nearest poly. Every event is counted (WalkerStats), never hidden by a teleport.
// Determinism: a brain's choices come from its own splitmix64 state (seed = match seed + slot) and the view; the
// server runs brains in slot order, so a run reproduces bit for bit (host.scale.unit checks the command hash).

#include "bf3/sim/command.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim::bots {

struct BotGoal {
	enum class Kind : uint8_t { NavPoint = 0, Flag = 1, Hq = 2, Mcom = 3, Spawn = 4 };
	Kind kind = Kind::NavPoint;
	Vec3 position{}; // on the mesh (the nearest point of the objective's poly)
	NavPolyRef poly = 0;
	float arrive_radius = 1.5f;
	uint32_t index = 0; // the objective's index in its source list
};

struct WalkerParams {
	float think_hz = 10.0f; // 5..10 (plan §3.6); period = round(kSimHz / think_hz) ticks, phase = slot % period
	float objective_share = 0.5f; // goals that are mode objectives (else random nav points)
	float sprint_share = 0.4f; // legs walked sprinting where the path allows it
	float crouch_share = 0.15f; // legs walked crouching
	float yaw_rate_deg_s = 540.0f; // view turn limit (research topics/06 §1: about 550 deg/s)
	float corner_lookahead = 1.5f; // metres: start turning towards the next corner this far before the corner
	float stuck_seconds = 2.0f; // IW4L bots controller.rs STUCK_TICKS 40 at 20 Hz
	float stuck_progress = 0.5f; // metres of progress expected over stuck_seconds while moving
	uint32_t max_repaths = 3; // repaths in a row before the goal is abandoned
	float off_mesh_radius = 2.0f; // farther from any poly = off the mesh
	float goal_min_distance = 20.0f; // a new random goal is at least this far (straight line)

	bool operator==(const WalkerParams &) const = default;
};

struct WalkerStats {
	uint64_t ticks = 0; // commands written
	uint64_t thinks = 0;
	uint64_t goals_started = 0;
	uint64_t goals_reached = 0;
	uint64_t goals_abandoned = 0; // max_repaths reached
	uint64_t path_requests = 0;
	uint64_t path_partial = 0;
	uint64_t path_failed = 0;
	uint64_t repaths = 0;
	uint64_t stuck_events = 0;
	uint64_t off_mesh_ticks = 0;
	uint64_t sprint_ticks = 0;
	uint64_t crouch_ticks = 0;
	double metres = 0.0; // horizontal distance walked (from the states)

	void add(const WalkerStats &p_other);
};

// Sliced path searches for every bot of one server thread, one at a time on the shared NavQuery (Detour allows one
// sliced search per query object), at most iterations_per_tick A* iterations per tick across all requests; a request
// is served first come first served and a bot asks again only after its answer. Server thread.
class PathService {
public:
	struct Options {
		uint32_t iterations_per_tick = 4096;
	};
	PathService(NavQuery &p_query, const Options &p_options);
	~PathService();
	PathService(const PathService &) = delete;
	PathService &operator=(const PathService &) = delete;

	// Queues (or replaces) slot p_slot's request; the answer comes in a later (or this) tick's run().
	void request(uint32_t p_slot, NavPolyRef p_start, const Vec3 &p_start_point, NavPolyRef p_goal, const Vec3 &p_goal_point);
	// Advances the searches by up to the budget; called once per tick before the brains.
	void run();
	// The finished answer of p_slot, if any (then forgotten): Done / Partial with polys, or Failed.
	NavPathStatus take(uint32_t p_slot, std::vector<NavPolyRef> &r_polys);
	size_t pending() const;
	uint64_t iterations() const; // total

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// What a brain reads. Everything is immutable during the brain's call; the NavQuery and PathService are the server
// thread's (the brains run on that thread one after another, so sharing them is safe).
struct WalkerView {
	Tick tick = 0; // the tick the returned command drives (= self.tick + 1)
	uint32_t slot = 0;
	const SoldierState *self = nullptr; // the bot's authoritative state after tick - 1
	float stand_eye_height = 1.6f;
	std::span<const BotGoal> objectives; // the mode's objectives on the mesh, main component only
	const NavComponents *components = nullptr;
	uint32_t main_component = 0;
	NavQuery *query = nullptr;
	PathService *paths = nullptr;
};

class WalkerBrain {
public:
	WalkerBrain(uint32_t p_slot, uint64_t p_seed, const WalkerParams &p_params);
	~WalkerBrain();
	WalkerBrain(WalkerBrain &&) noexcept;
	WalkerBrain &operator=(WalkerBrain &&) noexcept;

	// The command for p_view.tick (Command::tick set; sequence 0; flags are the server's). A dead bot gets an idle
	// command; a new life (SoldierState::life changed) drops the goal and the corridor.
	Command command(const WalkerView &p_view);
	const WalkerStats &stats() const;
	// The current goal (for the debug overlay), nullptr when none.
	const BotGoal *goal() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::sim::bots

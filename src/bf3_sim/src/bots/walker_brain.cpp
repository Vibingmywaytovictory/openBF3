// WalkerBrain (bots/walker_brain.h; docs/design/s1.md §7): goals on the main component, a path through the shared
// PathService, a NavCorridor, cheap per-tick steering and a staggered think. Writes Commands only.
#include "bf3/sim/bots/walker_brain.h"

#include "../nav_internal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bf3::sim::bots {

namespace {

uint64_t splitmix64(uint64_t &r_state) {
	uint64_t z = (r_state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

float unit(uint64_t &r_state) {
	return float(splitmix64(r_state) >> 40) / float(1ull << 24);
}

float horizontal(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_b[0] - p_a[0];
	const float dz = p_b[2] - p_a[2];
	return std::sqrt(dx * dx + dz * dz);
}

float wrap_angle(float p_radians) {
	float a = std::fmod(p_radians, kTwoPi);
	if (a > kPi) {
		a -= kTwoPi;
	} else if (a < -kPi) {
		a += kTwoPi;
	}
	return a;
}

// Yaw 0 looks down +Z, a quarter turn down +X (command.h): the yaw of a horizontal direction.
float yaw_of(float p_dx, float p_dz) {
	return std::atan2(p_dx, p_dz);
}

constexpr float kDropReach = 0.6f; // metres from a drop's start corner at which the walker commits to the drop
constexpr float kDropTimeout = 3.0f; // seconds to reach a drop's landing before repathing
constexpr float kArriveVertical = 1.5f;
constexpr float kBackRadius = 16.0f; // an off-mesh walker looks this far for the mesh
constexpr float kSprintStraight = 6.0f; // sprint only when the next corner is at least this far ...
constexpr float kSprintCone = 20.0f * kPi / 180.0f; // ... and within this angle of the view

} // namespace

struct WalkerBrain::Impl {
	uint32_t slot = 0;
	uint64_t rng = 0;
	WalkerParams params;
	uint32_t period = 6;
	uint32_t phase = 0;
	uint32_t stuck_ticks = 120;
	float turn_per_tick = 0.0f;
	WalkerStats stats;

	uint32_t life = UINT32_MAX;
	bool have_last = false;
	Vec3 last_position{};
	float yaw = 0.0f;
	bool yaw_set = false;

	bool have_goal = false;
	BotGoal goal;
	bool next_objective = false;
	bool sprint_leg = false;
	bool crouch_leg = false;
	uint32_t repaths_in_row = 0; // stuck repaths without progress since
	uint32_t partial_in_row = 0; // partial / failed answers for this goal

	bool need_path = false;
	bool pending = false;
	NavCorridor corridor;
	bool corridor_partial = false;
	std::array<NavCorner, kMaxNavCorners> corners{};
	uint32_t corner_count = 0;
	uint32_t corner_index = 0;
	std::vector<NavPolyRef> polys;

	bool dropping = false;
	Vec3 drop_end{};
	Tick drop_tick = 0;

	bool off_mesh = false;
	bool back_target_valid = false;
	Vec3 back_target{};

	Vec3 anchor{};
	Tick anchor_tick = 0;
	uint32_t moving_ticks = 0; // ticks with commanded movement since the anchor

	void forget_goal() {
		have_goal = false;
		need_path = false;
		pending = false;
		corridor.clear();
		corridor_partial = false;
		corner_count = 0;
		corner_index = 0;
		dropping = false;
		repaths_in_row = 0;
		partial_in_row = 0;
	}

	void abandon() {
		++stats.goals_abandoned;
		forget_goal();
	}

	void repath() {
		++stats.repaths;
		need_path = true;
		corridor.clear();
		corner_count = 0;
		corner_index = 0;
		dropping = false;
	}

	bool pick_goal(const WalkerView &p_view, const Vec3 &p_position) {
		const bool want_objective = !p_view.objectives.empty() && unit(rng) < params.objective_share;
		BotGoal g;
		bool found = false;
		if (want_objective) {
			const size_t i = size_t(splitmix64(rng) % p_view.objectives.size());
			g = p_view.objectives[i];
			found = horizontal(g.position, p_position) > g.arrive_radius;
		}
		if (!found && p_view.components != nullptr && p_view.query != nullptr) {
			for (int attempt = 0; attempt < 8 && !found; ++attempt) {
				NavPolyRef poly = 0;
				Vec3 point{};
				if (!p_view.query->random_point(rng, *p_view.components, p_view.main_component, &poly, &point)) {
					break;
				}
				g = BotGoal();
				g.kind = BotGoal::Kind::NavPoint;
				g.position = point;
				g.poly = poly;
				found = horizontal(point, p_position) >= params.goal_min_distance || attempt == 7;
			}
		}
		if (!found && !p_view.objectives.empty()) {
			g = p_view.objectives[size_t(splitmix64(rng) % p_view.objectives.size())];
			found = horizontal(g.position, p_position) > g.arrive_radius;
		}
		if (!found || g.poly == 0) {
			return false;
		}
		goal = g;
		have_goal = true;
		need_path = true;
		pending = false;
		corridor.clear();
		corridor_partial = false;
		corner_count = 0;
		corner_index = 0;
		dropping = false;
		repaths_in_row = 0;
		partial_in_row = 0;
		sprint_leg = unit(rng) < params.sprint_share;
		crouch_leg = !sprint_leg && unit(rng) < params.crouch_share;
		++stats.goals_started;
		return true;
	}

	// Takes the path service's answer (every tick while one is pending: a hash lookup).
	void take_answer(const WalkerView &p_view, const Vec3 &p_position) {
		if (!pending || p_view.paths == nullptr) {
			return;
		}
		const NavPathStatus st = p_view.paths->take(slot, polys);
		if (st == NavPathStatus::Idle) {
			return;
		}
		pending = false;
		if ((st != NavPathStatus::Done && st != NavPathStatus::Partial) || polys.empty()) {
			++stats.path_failed;
			if (++partial_in_row > params.max_repaths) {
				abandon();
			} else {
				need_path = true;
			}
			return;
		}
		Vec3 end = goal.position;
		corridor_partial = st == NavPathStatus::Partial || polys.back() != goal.poly;
		if (corridor_partial) {
			++stats.path_partial;
			nav_closest_point(*p_view.query, polys.back(), goal.position, &end);
		}
		corridor.reset(polys, p_position, end);
		corner_count = corridor.corners(*p_view.query, corners.data(), kMaxNavCorners);
		corner_index = 0;
	}

	// The staggered think: corridor, corners, arrival, progress, stuck, path requests.
	void think(const WalkerView &p_view, const SoldierState &p_self) {
		++stats.thinks;
		NavQuery &query = *p_view.query;
		const Vec3 &pos = p_self.position;
		const Vec3 extents{ params.off_mesh_radius, 2.5f, params.off_mesh_radius };

		if (dropping) {
			const bool landed = (p_self.flags & soldier_flags::kGrounded) != 0 && horizontal(pos, drop_end) <= 1.0f &&
					std::fabs(pos[1] - drop_end[1]) <= 2.5f;
			if (landed) {
				dropping = false;
			} else if (float(p_view.tick - drop_tick) * kTickSeconds > kDropTimeout) {
				repath();
			} else {
				return;
			}
		}

		if (!have_goal) {
			if (!pick_goal(p_view, pos)) {
				return;
			}
		}

		// Arrival (docs §7: within 1.5 m horizontally and vertically).
		if (horizontal(pos, goal.position) <= goal.arrive_radius && std::fabs(pos[1] - goal.position[1]) <= kArriveVertical) {
			++stats.goals_reached;
			forget_goal();
			if (!pick_goal(p_view, pos)) {
				return;
			}
		}

		// Progress and stuck: commanded movement over stuck_seconds with less than stuck_progress metres.
		if (p_view.tick - anchor_tick >= stuck_ticks) {
			const bool moved_enough = horizontal(pos, anchor) >= params.stuck_progress;
			if (moving_ticks * 2 >= stuck_ticks && !moved_enough) {
				++stats.stuck_events;
				if (repaths_in_row >= params.max_repaths) {
					abandon();
					anchor = pos;
					anchor_tick = p_view.tick;
					moving_ticks = 0;
					return;
				}
				++repaths_in_row;
				repath();
			} else if (moved_enough) {
				repaths_in_row = 0;
			}
			anchor = pos;
			anchor_tick = p_view.tick;
			moving_ticks = 0;
		}

		if (pending) {
			return;
		}

		if (!corridor.empty()) {
			if (!corridor.move_position(query, pos)) {
				repath();
			} else {
				corner_count = corridor.corners(query, corners.data(), kMaxNavCorners);
				corner_index = 0;
				off_mesh = false;
				const bool at_end = corner_count == 1 && corners[0].end && horizontal(pos, corners[0].position) <= goal.arrive_radius;
				if (at_end && !corridor_partial) {
					// The corridor's end is the goal (its closest point on the goal's poly): arrived.
					++stats.goals_reached;
					forget_goal();
					return;
				}
				if (corner_count == 0 || at_end) {
					// The partial corridor's end is reached (or nothing is left): ask again from here.
					if (++partial_in_row > params.max_repaths) {
						abandon();
						return;
					}
					repath();
				}
			}
		}

		if (need_path) {
			NavPolyRef start = 0;
			Vec3 start_point{};
			if (!query.nearest(pos, extents, &start, &start_point)) {
				off_mesh = true;
				const Vec3 back{ kBackRadius, 8.0f, kBackRadius };
				back_target_valid = query.nearest(pos, back, &start, &back_target);
				return;
			}
			off_mesh = false;
			back_target_valid = false;
			need_path = false;
			pending = true;
			++stats.path_requests;
			p_view.paths->request(slot, start, start_point, goal.poly, goal.position);
		}
	}

	// Per-tick steering towards the current target (no nav query except committing to a drop).
	Command steer(const WalkerView &p_view, const SoldierState &p_self) {
		Command c;
		c.tick = p_view.tick;
		const Vec3 &pos = p_self.position;
		bool have_target = false;
		Vec3 target{};
		uint8_t area = kNavAreaWalk;
		float straight = 0.0f;
		if (dropping) {
			target = drop_end;
			have_target = true;
		} else if (off_mesh) {
			if (back_target_valid) {
				target = back_target;
				have_target = true;
			}
		} else if (!pending && !need_path && corner_count > 0) {
			while (corner_index + 1 < corner_count && horizontal(pos, corners[corner_index].position) < params.corner_lookahead &&
					(corners[corner_index].flags & nav_flags::kDrop) == 0) {
				++corner_index;
			}
			const NavCorner &corner = corners[corner_index];
			if ((corner.flags & nav_flags::kDrop) != 0 && horizontal(pos, corner.position) <= kDropReach) {
				Vec3 end{};
				if (corridor.move_over_drop(*p_view.query, corner, &end)) {
					dropping = true;
					drop_end = end;
					drop_tick = p_view.tick;
					target = end;
					have_target = true;
				} else {
					repath();
				}
			} else {
				target = corner.position;
				have_target = true;
				area = corner.area;
				straight = horizontal(pos, corner.position);
			}
			uint8_t first_area = 0;
			uint16_t first_flags = 0;
			nav_poly_area_flags(p_view.query->world(), corridor.first_poly(), &first_area, &first_flags);
			if (first_area == kNavAreaCrouch) {
				area = kNavAreaCrouch;
			}
		}

		if (!yaw_set) {
			yaw = yaw_radians(p_self.yaw);
			yaw_set = true;
		}
		float rel = 0.0f;
		bool move = false;
		if (have_target) {
			const float dx = target[0] - pos[0];
			const float dz = target[2] - pos[2];
			if (dx * dx + dz * dz > 0.0025f) {
				const float want = yaw_of(dx, dz);
				const float turn = std::clamp(wrap_angle(want - yaw), -turn_per_tick, turn_per_tick);
				yaw = wrap_angle(yaw + turn);
				// The remaining direction in the new yaw's frame: forward (sin, cos), right (-cos, sin) (motor axes).
				const float len = std::sqrt(dx * dx + dz * dz);
				const float ux = dx / len, uz = dz / len;
				const float sy = std::sin(yaw), cy = std::cos(yaw);
				const float fwd = ux * sy + uz * cy;
				const float rgt = -ux * cy + uz * sy;
				c.move_forward = quantize_axis(fwd);
				c.move_right = quantize_axis(rgt);
				rel = wrap_angle(want - yaw);
				move = c.move_forward != 0 || c.move_right != 0;
			}
		}
		c.yaw = quantize_yaw(yaw);
		c.pitch = 0;
		const bool crouch = have_target && !dropping && (area == kNavAreaCrouch || crouch_leg);
		c.pose = crouch ? Pose::Crouch : Pose::Stand;
		if (move && !crouch && sprint_leg && !dropping && !off_mesh && area == kNavAreaWalk && straight >= kSprintStraight &&
				std::fabs(rel) <= kSprintCone && p_self.pose == Pose::Stand) {
			c.buttons |= buttons::kSprint;
			++stats.sprint_ticks;
		}
		if (crouch) {
			++stats.crouch_ticks;
		}
		if (off_mesh) {
			++stats.off_mesh_ticks;
		}
		if (move) {
			++moving_ticks;
		}
		return c;
	}
};

WalkerBrain::WalkerBrain(uint32_t p_slot, uint64_t p_seed, const WalkerParams &p_params) :
		impl_(std::make_unique<Impl>()) {
	Impl &m = *impl_;
	m.slot = p_slot;
	m.rng = p_seed ^ (0xA24BAED4963EE407ull * (uint64_t(p_slot) + 1));
	m.params = p_params;
	const float hz = std::clamp(p_params.think_hz, 1.0f, float(kSimHz));
	m.period = std::max<uint32_t>(1, uint32_t(std::lround(float(kSimHz) / hz)));
	m.phase = p_slot % m.period;
	m.stuck_ticks = std::max<uint32_t>(m.period, seconds_to_ticks(std::max(p_params.stuck_seconds, 0.1f)));
	m.turn_per_tick = std::max(p_params.yaw_rate_deg_s, 1.0f) * (kPi / 180.0f) * kTickSeconds;
}

WalkerBrain::~WalkerBrain() = default;
WalkerBrain::WalkerBrain(WalkerBrain &&) noexcept = default;
WalkerBrain &WalkerBrain::operator=(WalkerBrain &&) noexcept = default;

Command WalkerBrain::command(const WalkerView &p_view) {
	Impl &m = *impl_;
	++m.stats.ticks;
	const SoldierState *self = p_view.self;
	if (self == nullptr || (self->flags & soldier_flags::kAlive) == 0 || self->mode == MoveMode::Dead || p_view.query == nullptr ||
			p_view.paths == nullptr) {
		if (self != nullptr) {
			Command c = idle_command(p_view.tick, self->yaw, self->pitch, self->target_pose);
			c.flags = 0;
			return c;
		}
		Command c;
		c.tick = p_view.tick;
		return c;
	}
	if (self->life != m.life) {
		m.life = self->life;
		m.forget_goal();
		m.have_last = false;
		m.yaw_set = false;
		m.off_mesh = false;
		m.anchor = self->position;
		m.anchor_tick = p_view.tick;
		m.moving_ticks = 0;
	}
	if (m.have_last) {
		const float d = horizontal(m.last_position, self->position);
		if (d < 5.0f) {
			m.stats.metres += double(d);
		}
	}
	m.last_position = self->position;
	m.have_last = true;

	m.take_answer(p_view, self->position);
	if ((p_view.tick % m.period) == m.phase) {
		m.think(p_view, *self);
	}
	return m.steer(p_view, *self);
}

const WalkerStats &WalkerBrain::stats() const {
	return impl_->stats;
}

const BotGoal *WalkerBrain::goal() const {
	return impl_->have_goal ? &impl_->goal : nullptr;
}

} // namespace bf3::sim::bots

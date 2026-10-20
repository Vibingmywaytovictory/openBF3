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

float yaw_of(float p_dx, float p_dz) {
	return std::atan2(p_dx, p_dz);
}

constexpr float kDropReach = 0.6f;
constexpr float kDropTimeout = 3.0f;
constexpr float kArriveVertical = 1.5f;
constexpr float kBackRadius = 16.0f;
constexpr float kSprintStraight = 6.0f;
constexpr float kSprintCone = 20.0f * kPi / 180.0f;

}

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
	uint32_t repaths_in_row = 0;
	uint32_t partial_in_row = 0;

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

	uint32_t home = UINT32_MAX;
	bool back_target_valid = false;
	Vec3 back_target{};

	Vec3 anchor{};
	Tick anchor_tick = 0;
	uint32_t moving_ticks = 0;

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
		const uint32_t component = home == UINT32_MAX ? p_view.main_component : home;
		const bool in_main = component == p_view.main_component;
		const bool want_objective = in_main && !p_view.objectives.empty() && unit(rng) < params.objective_share;
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
				if (!p_view.query->random_point(rng, *p_view.components, component, &poly, &point)) {
					break;
				}
				g = BotGoal();
				g.kind = BotGoal::Kind::NavPoint;
				g.position = point;
				g.poly = poly;
				found = horizontal(point, p_position) >= params.goal_min_distance || attempt == 7;
			}
		}
		if (!found && in_main && !p_view.objectives.empty()) {
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

		if (horizontal(pos, goal.position) <= goal.arrive_radius && std::fabs(pos[1] - goal.position[1]) <= kArriveVertical) {
			++stats.goals_reached;
			forget_goal();
			if (!pick_goal(p_view, pos)) {
				return;
			}
		}

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

					++stats.goals_reached;
					forget_goal();
					return;
				}
				if (corner_count == 0 || at_end) {

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
			if (p_view.components != nullptr) {
				const uint32_t c = nav_poly_component(query.world(), *p_view.components, start);
				const uint32_t want = home == UINT32_MAX ? p_view.main_component : home;
				if (c != UINT32_MAX && c != want) {

					++stats.starts_outside_main;
					home = c == p_view.main_component ? UINT32_MAX : c;
					forget_goal();
					return;
				}
			}
			need_path = false;
			pending = true;
			++stats.path_requests;
			p_view.paths->request(slot, start, start_point, goal.poly, goal.position);
		}
	}

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
		m.home = UINT32_MAX;
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

}

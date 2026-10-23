#include "tdm_bot_movement.h"

#include "bf3/host/bot_fill.h"
#include "bf3/sim/command.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"

#include <array>
#include <cmath>
#include <utility>

namespace bf3::host::tdm_bot {

namespace {

constexpr uint32_t kGoalResetTicks = 90;
constexpr float kGoalMoveMetres = 8.0f;
constexpr uint32_t kRoamGoalTicks = 600;
constexpr float kHeatMin = 0.2f;
constexpr float kHeatRadiusMetres = 160.0f;

constexpr uint32_t kIslandCheckTicks = 30;
constexpr uint32_t kIslandJumpTicks = 45;
constexpr float kIslandProgressMetres = 0.5f;
constexpr float kIslandRings[] = { 2.0f, 4.0f, 6.0f, 9.0f, 12.0f, 16.0f, 24.0f, 32.0f };

constexpr float kStrafeAxis = 0.8f;
constexpr uint32_t kStrafeMinTicks = 60, kStrafeMaxTicks = 150;
constexpr uint32_t kStrafeGroundTicks = 6;

}

void BotNavigation::set_slot(uint8_t p_slot, uint64_t p_seed) {
	slot_ = p_slot;
	seed_ = p_seed;
	walker_.reset();
}

void BotNavigation::new_life() {
	off_main_ = false;
	exit_progress_tick_ = 0;
	have_goal_ = false;
	goals_.clear();
	roam_tick_ = 0;
}

bool BotNavigation::set_goal(TdmShared &r_shared, sim::Tick p_tick, const sim::Vec3 &p_at, float p_arrive, bool p_force) {
	TdmShared &sh = r_shared;
	if (sh.nav == nullptr || !finite(p_at)) {
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
	if (have_goal_ && walker_.has_value() && horizontal(at, goal_pos_) < kGoalMoveMetres) {
		return true;
	}
	if (!p_force && have_goal_ && p_tick - goal_tick_ < kGoalResetTicks) {
		return true;
	}
	sim::bots::BotGoal g;
	g.kind = sim::bots::BotGoal::Kind::NavPoint;
	g.position = at;
	g.poly = poly;
	g.arrive_radius = p_arrive;
	goals_.assign(1, g);
	have_goal_ = true;
	goal_pos_ = at;
	goal_tick_ = p_tick;
	++walker_resets_;
	walker_.emplace(slot_, mix(seed_ ^ (uint64_t(walker_resets_) << 40)), sh.walker);
	sh.count(&TdmBotStats::walker_resets, uint64_t(1));
	return true;
}

void BotNavigation::clear_goal() {
	have_goal_ = false;
	goals_.clear();
}

void BotNavigation::pick_roam_goal(TdmShared &r_shared, sim::Tick p_tick, const sim::Vec3 &p_at, uint64_t &r_rng) {
	if (!have_goal_ || roam_tick_ == 0 || p_tick - roam_tick_ >= kRoamGoalTicks) {
		roam_tick_ = p_tick;
		if (const std::optional<sim::Vec3> h = heat_goal(r_shared, p_tick, p_at, r_rng)) {
			if (set_goal(r_shared, p_tick, *h, kHuntArriveMetres, true)) {
				r_shared.count(&TdmBotStats::heat_goals, uint64_t(1));
			}
		} else {
			clear_goal();
		}
	}
}

void BotNavigation::check_island(TdmShared &r_shared, sim::Tick p_tick, const sim::SoldierState &p_state) {
	TdmShared &sh = r_shared;
	BotNav &nav = *sh.nav;
	sim::NavPolyRef poly = 0;
	sim::Vec3 at{};
	bool on_main = true;
	if (nav.query->nearest(p_state.position, { 1.0f, 2.5f, 1.0f }, &poly, &at) && poly != 0) {
		on_main = sim::nav_poly_component(nav.query->world(), nav.level->components, poly) == nav.level->main_component;
	}
	if (on_main || nav.level->spawns.points.empty()) {
		off_main_ = false;
		return;
	}
	if (!off_main_) {

		float best = 1e30f;
		for (const float r : kIslandRings) {
			for (int k = 0; k < 16; ++k) {
				const float a = float(k) * (sim::kTwoPi / 16.0f);
				for (const float dy : { 0.0f, -2.5f, -5.0f }) {
					const sim::Vec3 probe = add(p_state.position, r * std::sin(a), dy, r * std::cos(a));
					sim::NavPolyRef q = 0;
					sim::Vec3 qa{};
					if (!nav.query->nearest(probe, { 1.0f, 1.5f, 1.0f }, &q, &qa) || q == 0 ||
							sim::nav_poly_component(nav.query->world(), nav.level->components, q) != nav.level->main_component) {
						continue;
					}
					const float d = distance(qa, p_state.position);
					if (d < best) {
						best = d;
						exit_at_ = qa;
					}
				}
			}
			if (best < 1e30f) {
				break;
			}
		}
		const bool found = best < 1e30f;
		for (const sim::SpawnPoint &sp : nav.level->spawns.points) {
			const float d = horizontal(sp.position, p_state.position);
			if (!found && finite(sp.position) && d < best) {
				best = d;
				exit_at_ = sp.position;
			}
		}
		exit_from_ = p_state.position;
		exit_progress_tick_ = p_tick;
		clear_goal();
	}
	off_main_ = true;
}

bool BotNavigation::walk(TdmShared &r_shared, sim::Tick p_tick, const sim::SoldierState &p_state, float &r_dx, float &r_dz, float &r_yaw, bool &r_sprint,
		sim::Pose &r_pose, bool &r_jump) {
	r_jump = false;
	TdmShared &sh = r_shared;
	if (sh.nav == nullptr) {
		return false;
	}
	if (!walker_.has_value()) {
		walker_.emplace(slot_, mix(seed_ ^ 0x57414C4Bull), sh.walker);
	}
	if ((p_tick + slot_) % kIslandCheckTicks == 0) {
		check_island(sh, p_tick, p_state);
	}
	if (off_main_) {
		sh.count(&TdmBotStats::off_main_ticks, uint64_t(1));
		const float dx = exit_at_[0] - p_state.position[0], dz = exit_at_[2] - p_state.position[2];
		const float l = std::sqrt(dx * dx + dz * dz);
		r_dx = l > 0.1f ? dx / l : 0.0f;
		r_dz = l > 0.1f ? dz / l : 0.0f;
		r_yaw = yaw_of(dx, dz);
		r_sprint = true;
		r_pose = sim::Pose::Stand;
		if (horizontal(p_state.position, exit_from_) >= kIslandProgressMetres) {
			exit_from_ = p_state.position;
			exit_progress_tick_ = p_tick;
		}
		r_jump = p_tick - exit_progress_tick_ >= kIslandJumpTicks && (p_tick - exit_progress_tick_) % kIslandJumpTicks == 0;
		return true;
	}
	const sim::bots::WalkerView wv = sh.nav->view(p_tick, slot_, p_state, goals_, sh.eye_of(sim::Pose::Stand));
	const sim::Command wc = walker_->command(wv);
	const float wy = sim::yaw_radians(wc.yaw);
	const float f = sim::axis_value(wc.move_forward), r = sim::axis_value(wc.move_right);
	r_dx = f * std::sin(wy) - r * std::cos(wy);
	r_dz = f * std::cos(wy) + r * std::sin(wy);
	r_yaw = wy;
	r_sprint = (wc.buttons & sim::buttons::kSprint) != 0;
	r_pose = wc.pose;
	return true;
}

std::optional<sim::Vec3> BotNavigation::heat_goal(TdmShared &r_shared, sim::Tick p_tick, const sim::Vec3 &p_at, uint64_t &r_rng) {
	TdmShared &sh = r_shared;
	float total = 0.0f;
	std::array<std::pair<int, float>, 64> picks{};
	size_t n = 0;
	for (int c = 0; c < kHeatCells * kHeatCells; ++c) {
		if (sh.heat[size_t(c)] < kHeatMin) {
			continue;
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
	float u = unit(r_rng) * total;
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

void BotStrafe::step(const TdmBotConfig &p_config, uint8_t p_slot, sim::Tick p_tick, const sim::CollisionWorld &p_world, const sim::SoldierState &p_self,
		float p_view_yaw, uint64_t &r_rng, float &r_dx, float &r_dz) {
	const TdmBotConfig &c = p_config;
	if (!strafing_) {
		strafing_ = true;
		anchor_ = p_self.position;
		ax_ = -std::cos(p_view_yaw);
		az_ = std::sin(p_view_yaw);
		dir_ = unit(r_rng) < 0.5f ? -1.0f : 1.0f;
		half_ = 0.5f * uniform(r_rng, std::max(c.strafe_min_m, 0.5f), std::max(c.strafe_max_m, std::max(c.strafe_min_m, 0.5f)));
		until_ = p_tick + uniform_u(r_rng, kStrafeMinTicks, kStrafeMaxTicks);
	} else if (p_tick >= until_) {
		dir_ = -dir_;
		until_ = p_tick + uniform_u(r_rng, kStrafeMinTicks, kStrafeMaxTicks);
	}
	const sim::Vec3 off = sub(p_self.position, anchor_);
	const float along = off[0] * ax_ + off[2] * az_;
	if (along * dir_ > half_) {
		dir_ = -dir_;
	} else if ((p_tick + p_slot) % kStrafeGroundTicks == 0) {
		const sim::Vec3 ahead = add(p_self.position, dir_ * ax_ * 0.8f, 0.5f, dir_ * az_ * 0.8f);
		if (!p_world.ground_below(ahead, 1.2f, sim::query_mask::kMovement, nullptr)) {
			dir_ = -dir_;
		}
	}
	r_dx = dir_ * ax_ * kStrafeAxis;
	r_dz = dir_ * az_ * kStrafeAxis;
}

}

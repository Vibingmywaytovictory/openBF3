#include "bf3/sim/bots/perception.h"

#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bf3::sim::bots {

namespace {

constexpr float kDeg = kPi / 180.0f;
constexpr float kMovingSpeed = 0.5f;
constexpr size_t kMaxSlots = 256;

bool finite3(const Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

float length3(float p_x, float p_y, float p_z) {
	return std::sqrt(p_x * p_x + p_y * p_y + p_z * p_z);
}

float wrap(float p_radians) {
	float a = std::fmod(p_radians, kTwoPi);
	if (a > kPi) {
		a -= kTwoPi;
	} else if (a < -kPi) {
		a += kTwoPi;
	}
	return a;
}

float eye_of(const float p_eye[3], Pose p_pose) {
	const size_t i = size_t(p_pose) < 3 ? size_t(p_pose) : 0;
	return p_eye[i] > 0.0f && std::isfinite(p_eye[i]) ? p_eye[i] : 1.6f;
}

float pose_ratio(const float p_eye[3], Pose p_pose) {
	return eye_of(p_eye, p_pose) / eye_of(p_eye, Pose::Stand);
}

bool alive_state(const SoldierState &p_state) {
	return (p_state.flags & soldier_flags::kAlive) != 0 && p_state.mode != MoveMode::Dead;
}

}

bool perception_tick(uint8_t p_slot, Tick p_tick, uint32_t p_update_ticks) {
	const uint32_t u = std::max<uint32_t>(p_update_ticks, 1u);
	return (uint64_t(p_tick) + p_slot) % u == 0;
}

float detection_time(const PerceptionParams &p_params, float p_distance) {
	const float near_m = std::max(p_params.near_m, 0.0f);
	const float far_m = std::max(p_params.far_m, near_m + 1e-3f);
	const float t = std::clamp((p_distance - near_m) / (far_m - near_m), 0.0f, 1.0f);
	const float v = p_params.detect_near_s + (p_params.detect_far_s - p_params.detect_near_s) * t;
	return std::isfinite(v) ? std::max(v, kTickSeconds) : 1.0f;
}

Vec3 perception_head(const PerceptionParams &p_params, const SoldierState &p_state, const float p_eye_heights[3]) {
	const float r = pose_ratio(p_eye_heights, p_state.pose);
	return { p_state.position[0], p_state.position[1] + p_params.head_height_m * r, p_state.position[2] };
}

Vec3 perception_chest(const PerceptionParams &p_params, const SoldierState &p_state, const float p_eye_heights[3]) {
	const float r = pose_ratio(p_eye_heights, p_state.pose);
	return { p_state.position[0], p_state.position[1] + p_params.chest_height_m * r, p_state.position[2] };
}

Perception::Perception(const PerceptionParams &p_params) :
		params_(p_params) {
}

void Perception::reset() {
	enemies_.clear();
	newly_.clear();
	heard_ = 0;
}

std::span<const PerceivedEnemy> Perception::enemies() const {
	return enemies_;
}

const PerceivedEnemy *Perception::find(uint8_t p_slot) const {
	const auto it = std::lower_bound(enemies_.begin(), enemies_.end(), p_slot, [](const PerceivedEnemy &p_e, uint8_t p_s) { return p_e.slot < p_s; });
	return it != enemies_.end() && it->slot == p_slot ? &*it : nullptr;
}

const PerceptionParams &Perception::params() const {
	return params_;
}

std::span<const uint8_t> Perception::newly_detected() const {
	return newly_;
}

uint32_t Perception::heard_last() const {
	return heard_;
}

uint32_t Perception::update(const PerceptionInput &p_in) {
	newly_.clear();
	heard_ = 0;
	const uint32_t update_ticks = std::max<uint32_t>(params_.update_ticks, 1u);
	const float dt = float(update_ticks) * kTickSeconds;
	const size_t n = std::min({ p_in.states.size(), p_in.teams.size(), kMaxSlots });
	const auto entry = [&](uint8_t p_slot) -> PerceivedEnemy & {
		auto it = std::lower_bound(enemies_.begin(), enemies_.end(), p_slot, [](const PerceivedEnemy &p_e, uint8_t p_s) { return p_e.slot < p_s; });
		if (it == enemies_.end() || it->slot != p_slot) {
			PerceivedEnemy e;
			e.slot = p_slot;
			it = enemies_.insert(it, e);
		}
		return *it;
	};
	const auto is_enemy = [&](size_t p_slot) {
		if (p_slot >= n || p_slot == p_in.self) {
			return false;
		}
		const uint8_t t = p_in.teams[p_slot];
		return t != 0 && t != p_in.team;
	};
	const auto is_alive = [&](size_t p_slot) {
		if (p_slot < p_in.alive.size()) {
			return p_in.alive[p_slot] != 0 && alive_state(p_in.states[p_slot]);
		}
		return alive_state(p_in.states[p_slot]);
	};
	for (PerceivedEnemy &e : enemies_) {
		e.visible = false;
	}

	struct Cand {
		float d;
		uint8_t detected;
		uint8_t slot;
	};
	std::array<Cand, kMaxSlots> cands{};
	size_t nc = 0;
	const bool eye_ok = finite3(p_in.eye);
	const float view_yaw = yaw_radians(p_in.yaw);
	const float view_pitch = pitch_radians(p_in.pitch);
	const float half_h = std::clamp(params_.fov_h_deg, 0.0f, 360.0f) * 0.5f * kDeg;
	const float half_v = std::clamp(params_.fov_v_deg, 0.0f, 180.0f) * 0.5f * kDeg;
	for (size_t s = 0; s < n && eye_ok; ++s) {
		if (!is_enemy(s) || !is_alive(s) || !finite3(p_in.states[s].position)) {
			continue;
		}
		const Vec3 chest = perception_chest(params_, p_in.states[s], p_in.eye_heights);
		const float dx = chest[0] - p_in.eye[0], dy = chest[1] - p_in.eye[1], dz = chest[2] - p_in.eye[2];
		const float h = std::sqrt(dx * dx + dz * dz);
		const float d = length3(dx, dy, dz);
		if (!std::isfinite(d) || d > params_.max_range_m) {
			continue;
		}
		if (d > 0.5f) {
			const float a_h = std::fabs(wrap(std::atan2(dx, dz) - view_yaw));
			const float a_v = std::fabs(std::atan2(dy, std::max(h, 1e-3f)) - view_pitch);
			if (a_h > half_h || a_v > half_v) {
				continue;
			}
		}
		const PerceivedEnemy *known = find(uint8_t(s));
		cands[nc++] = { d, uint8_t(known != nullptr && known->detected ? 0 : 1), uint8_t(s) };
	}
	std::sort(cands.begin(), cands.begin() + ptrdiff_t(nc), [](const Cand &p_a, const Cand &p_b) {
		if (p_a.d != p_b.d) {
			return p_a.d < p_b.d;
		}
		if (p_a.detected != p_b.detected) {
			return p_a.detected < p_b.detected;
		}
		return p_a.slot < p_b.slot;
	});
	const uint32_t mask = p_in.ray_mask != 0 ? p_in.ray_mask : query_mask::kProjectile;
	uint32_t rays = 0;
	const size_t take = std::min<size_t>(nc, params_.max_candidates);
	for (size_t k = 0; k < take; ++k) {
		const uint8_t s = cands[k].slot;
		const SoldierState &st = p_in.states[s];
		bool clear = false;
		bool chest_clear = true;
		if (p_in.world != nullptr) {
			const Vec3 chest = perception_chest(params_, st, p_in.eye_heights);
			++rays;
			clear = !p_in.world->ray_any(p_in.eye, { chest[0] - p_in.eye[0], chest[1] - p_in.eye[1], chest[2] - p_in.eye[2] }, mask);
			if (!clear) {
				chest_clear = false;
				const Vec3 head = perception_head(params_, st, p_in.eye_heights);
				++rays;
				clear = !p_in.world->ray_any(p_in.eye, { head[0] - p_in.eye[0], head[1] - p_in.eye[1], head[2] - p_in.eye[2] }, mask);
			}
		} else {
			clear = true;
		}
		if (!clear) {
			continue;
		}
		PerceivedEnemy &e = entry(s);
		const float speed = std::sqrt(st.velocity[0] * st.velocity[0] + st.velocity[2] * st.velocity[2]);
		const float moving = std::isfinite(speed) && speed > kMovingSpeed ? 1.0f : 0.0f;
		const float firing = s < p_in.firing.size() && p_in.firing[s] != 0 ? 1.0f : 0.0f;
		const bool was = e.detected;
		e.detection = std::min(1.0f, e.detection + dt * (1.0f + moving + firing) / detection_time(params_, cands[k].d));
		if (e.detection >= 1.0f) {
			e.detected = true;
		}
		if (e.detected && !was) {
			newly_.push_back(s);
		}
		e.visible = true;
		e.chest_visible = chest_clear;
		e.heard = false;
		e.last_seen = p_in.tick;
		e.last_visible = p_in.tick;
		e.ever_visible = true;
		e.last_position = st.position;
		e.last_velocity = finite3(st.velocity) ? st.velocity : Vec3{};
	}

	uint64_t rng = weapons::splitmix64(p_in.seed ^ (uint64_t(p_in.tick) << 20) ^ 0x48454152ull);
	for (const HeardShot &shot : p_in.shots) {
		if (!is_enemy(shot.shooter) || !finite3(shot.origin) || !eye_ok) {
			continue;
		}
		if (shot.tick > p_in.tick || p_in.tick - shot.tick >= update_ticks) {
			continue;
		}
		const float d = length3(shot.origin[0] - p_in.eye[0], shot.origin[1] - p_in.eye[1], shot.origin[2] - p_in.eye[2]);
		if (!(d <= params_.hearing_radius_m)) {
			continue;
		}
		PerceivedEnemy &e = entry(shot.shooter);
		++heard_;
		if (e.visible) {
			continue;
		}
		rng = weapons::splitmix64(rng);
		const float a = float(rng >> 40) / float(1ull << 24) * kTwoPi;
		rng = weapons::splitmix64(rng);
		const float r = std::sqrt(float(rng >> 40) / float(1ull << 24)) * std::max(params_.hearing_error_m, 0.0f);
		e.heard = true;
		e.last_seen = std::max(e.last_seen, shot.tick);
		e.last_position = { shot.origin[0] + r * std::sin(a), shot.origin[1], shot.origin[2] + r * std::cos(a) };
	}

	const uint32_t forget = seconds_to_ticks(std::max(params_.forget_s, 0.0f));
	const uint32_t partial = std::max<uint32_t>(forget / 3u, update_ticks);
	enemies_.erase(std::remove_if(enemies_.begin(), enemies_.end(),
						   [&](const PerceivedEnemy &p_e) {
							   if (!is_enemy(p_e.slot) || !is_alive(p_e.slot)) {
								   return true;
							   }
							   if (p_e.visible) {
								   return false;
							   }
							   const uint32_t age = p_in.tick >= p_e.last_seen ? p_in.tick - p_e.last_seen : 0u;
							   return age > (p_e.detected || p_e.heard ? forget : partial);
						   }),
			enemies_.end());
	return rays;
}

}

#include "tdm_bot_awareness.h"

#include "bf3/core/math.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/command.h"

#include <algorithm>
#include <cmath>

namespace bf3::host::tdm_bot {

namespace {

constexpr float kShotAtMetres = 1.5f;
constexpr float kShotRayMetres = 150.0f;
constexpr uint32_t kShotAtTicks = 120;

}

void BotAwareness::new_life(sim::Tick p_tick, const sim::SoldierState &p_state) {
	perception_.reset();
	last_health_ = p_state.health;
	damaged_tick_ = shot_at_tick_ = 0;
	shot_by_ = -1;
	shots_checked_ = p_tick;
}

bool BotAwareness::shot_recent(sim::Tick p_tick) const {
	return shot_at_tick_ != 0 && p_tick - shot_at_tick_ <= kShotAtTicks;
}

void BotAwareness::check_shots(const TdmShared &p_shared, uint8_t p_slot, sim::Tick p_tick, const sim::SoldierState &p_self) {
	const TdmShared &sh = p_shared;
	const sim::Vec3 chest = sh.chest_point(p_self);
	const uint8_t team = p_slot < sh.teams.size() ? sh.teams[p_slot] : 0;
	for (const RecentShot &r : sh.ring) {
		if (r.tick <= shots_checked_ || r.shooter == p_slot || r.shooter >= sh.teams.size()) {
			continue;
		}
		const uint8_t other = sh.teams[r.shooter];
		if (other == 0 || other == team) {
			continue;
		}
		if (ray_point_distance(r.origin, r.direction, kShotRayMetres, chest) <= kShotAtMetres) {
			shot_at_tick_ = p_tick;
			shot_by_ = r.shooter;
			shot_from_ = r.origin;
		}
	}
	shots_checked_ = p_tick;
	if (p_self.health < last_health_ - 0.01f) {
		damaged_tick_ = p_tick;
	}
	last_health_ = p_self.health;
}

void BotAwareness::perceive(TdmShared &r_shared, uint8_t p_slot, uint64_t p_seed, sim::Tick p_tick, const sim::SoldierState &p_self,
		std::span<const sim::SoldierState> p_states, float p_yaw, float p_pitch) {
	TdmShared &sh = r_shared;
	const ServerView &view = *sh.host.view;
	const uint64_t t0 = now_ns();
	sim::bots::PerceptionInput in;
	in.self = p_slot;
	in.team = p_slot < sh.teams.size() ? sh.teams[p_slot] : 0;
	in.tick = p_tick;
	in.eye = sh.eye_point(p_self);
	in.yaw = sim::quantize_yaw(p_yaw);
	in.pitch = sim::quantize_pitch(p_pitch);
	in.states = p_states;
	in.teams = sh.teams;
	in.alive = sh.alive;
	in.firing = sh.firing;
	in.shots = sh.heard;
	in.world = &view.world();
	in.seed = p_seed;
	for (size_t i = 0; i < 3; ++i) {
		in.eye_heights[i] = sh.host.eye_heights[i];
	}
	const uint32_t rays = perception_.update(in);
	sh.perception_ns += now_ns() - t0;
	sh.count(&TdmBotStats::perception_updates, uint64_t(1));
	sh.count(&TdmBotStats::rays, uint64_t(rays));
	if (perception_.heard_last() > 0) {
		sh.count(&TdmBotStats::heard, uint64_t(perception_.heard_last()));
	}
	for (const uint8_t s : perception_.newly_detected()) {
		sh.count(&TdmBotStats::detections, uint64_t(1));
		if (sh.observer != nullptr) {
			sh.observer->detected(view, p_tick, p_slot, s, in.eye);
		}
	}
}

bool aims_at(const TdmShared &p_shared, const sim::SoldierState &p_other, const sim::SoldierState &p_self) {
	const TdmShared &sh = p_shared;
	const sim::Vec3 from = sh.eye_point(p_other);
	const sim::Vec3 to = sh.chest_point(p_self);
	const float d = distance(from, to);
	if (!(d > 0.1f)) {
		return true;
	}
	const sim::Vec3 dir = view_dir(sim::yaw_radians(p_other.yaw), sim::pitch_radians(p_other.pitch));
	const float cosang = ((to[0] - from[0]) * dir[0] + (to[1] - from[1]) * dir[1] + (to[2] - from[2]) * dir[2]) / d;
	return cosang >= std::cos(std::clamp(sh.config->aims_at_me_deg, 0.0f, 90.0f) * core::kDegToRadF);
}

}

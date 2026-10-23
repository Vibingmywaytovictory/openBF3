#include "tdm_bot_targeting.h"

#include "bf3/host/server_view.h"

#include <algorithm>

namespace bf3::host::tdm_bot {

namespace {

constexpr uint32_t kVisibleFireTicks = 12;
constexpr uint32_t kLoseTicks = 90;
constexpr uint32_t kErrorTicks = 6;
constexpr float kErrorPitchShare = 0.6f;
constexpr uint32_t kBlindTicks = 40;

}

bool visible_recent(const sim::bots::PerceivedEnemy *p_enemy, sim::Tick p_tick) {
	return p_enemy != nullptr && p_enemy->detected && p_enemy->ever_visible && p_tick - p_enemy->last_visible <= kVisibleFireTicks;
}

void BotTargeting::new_life() {
	target_ = -1;
	target_since_ = react_at_ = last_engaged_ = 0;
	err_yaw_ = err_pitch_ = 0.0f;
	err_next_ = 0;
	lost_target_ = -1;
	loss_handled_ = true;
	blind_until_ = 0;
}

void BotTargeting::choose(TdmShared &r_shared, const BotAwareness &p_awareness, sim::Tick p_tick, const sim::SoldierState &p_self,
		std::span<const sim::SoldierState> p_states, float p_skill01, uint64_t &r_rng) {
	TdmShared &sh = r_shared;
	const TdmBotConfig &c = *sh.config;
	const sim::Vec3 eye = sh.eye_point(p_self);
	const uint32_t memory = sim::seconds_to_ticks(std::max(c.damage_memory_s, 0.0f));
	int best = -1;
	float best_score = -1.0f, cur_score = -1.0f;
	for (const sim::bots::PerceivedEnemy &e : p_awareness.perception().enemies()) {
		if (!visible_recent(&e, p_tick) || e.slot >= p_states.size()) {
			continue;
		}
		const float d = std::max(distance(eye, sh.chest_point(p_states[e.slot])), 1.0f);
		float score = c.w_distance / d;
		if (aims_at(sh, p_states[e.slot], p_self)) {
			score += c.w_aim;
		}
		if (p_awareness.shot_by() == int(e.slot) && p_tick - p_awareness.shot_at_tick() <= memory) {
			score += c.w_damage;
		}
		if (int(e.slot) == target_) {
			score += c.w_current;
			cur_score = score;
		}
		if (score > best_score) {
			best_score = score;
			best = int(e.slot);
		}
	}
	if (best < 0 || best == target_) {
		return;
	}
	if (target_ >= 0 && cur_score >= 0.0f && best_score <= cur_score * (1.0f + std::max(c.switch_margin, 0.0f))) {
		return;
	}
	const bool had = target_ >= 0 && cur_score >= 0.0f;
	target_ = best;
	target_since_ = p_tick;
	err_next_ = p_tick;
	const float lo = std::min(c.reaction_min_s, c.reaction_max_s), hi = std::max(c.reaction_min_s, c.reaction_max_s);

	const float base = hi - (hi - lo) * p_skill01 + (unit(r_rng) - 0.5f) * 0.3f * (hi - lo);
	const float reaction = std::clamp(base, lo, hi) * (had ? 0.5f : 1.0f);
	react_at_ = p_tick + sim::seconds_to_ticks(reaction);
	loss_handled_ = false;
	lost_target_ = -1;
	sh.count(&TdmBotStats::retargets, uint64_t(1));
}

Sighting BotTargeting::track(TdmShared &r_shared, const sim::bots::Perception &p_perception, sim::Tick p_tick, std::span<const sim::SoldierState> p_states,
		uint64_t &r_rng) {
	TdmShared &sh = r_shared;
	const ServerView &view = *sh.host.view;
	const sim::bots::PerceivedEnemy *te = target_ >= 0 ? p_perception.find(uint8_t(target_)) : nullptr;
	if (target_ >= 0 && (te == nullptr || size_t(target_) >= p_states.size() || !view.alive(uint8_t(target_)) || p_tick - te->last_visible > kLoseTicks)) {
		target_ = -1;
		te = nullptr;
	}
	const bool visible = visible_recent(te, p_tick);
	if (visible) {
		last_engaged_ = p_tick;
	} else if (target_ >= 0 && !loss_handled_) {

		loss_handled_ = true;
		lost_target_ = target_;
		blind_at_ = sh.chest_point(p_states[size_t(target_)]);
		blind_at_ = add(te->last_position, 0.0f, blind_at_[1] - p_states[size_t(target_)].position[1], 0.0f);
		if (unit(r_rng) < sh.config->blind_fire_share) {
			blind_until_ = p_tick + kBlindTicks;
			sh.count(&TdmBotStats::blind_fire, uint64_t(1));
		}
	}
	return { te, visible };
}

AimError BotTargeting::aim_error(sim::Tick p_tick, float p_sigma, uint64_t &r_rng) {
	if (p_tick >= err_next_) {
		err_yaw_ = gauss(r_rng) * p_sigma;
		err_pitch_ = gauss(r_rng) * p_sigma * kErrorPitchShare;
		err_next_ = p_tick + kErrorTicks;
	}
	return { err_yaw_, err_pitch_ };
}

}

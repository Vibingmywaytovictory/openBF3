#include "tdm_bot_objectives.h"

#include "bf3/core/math.h"
#include "bf3/host/kits.h"

#include <algorithm>
#include <cmath>

namespace bf3::host::tdm_bot {

namespace {

namespace sw = sim::weapons;

constexpr float kRunSpeed = 4.5f;
constexpr float kPathFactor = 1.3f;
constexpr uint32_t kReviveTimeoutTicks = 900;
constexpr uint32_t kDefibRetryTicks = 66;
constexpr uint32_t kGrenadeLostMinTicks = 30, kGrenadeLostMaxTicks = 240;
constexpr uint32_t kGrenadeSwitchTicks = 120, kGrenadeAfterTicks = 45, kGrenadeHoldExtraTicks = 4;
constexpr float kRetreatMetres = 14.0f;
constexpr uint32_t kRetreatMaxTicks = 360;
constexpr uint32_t kRetreatClearTicks = 60;
constexpr uint32_t kHuntGiveUpTicks = 1200;
constexpr uint32_t kHuntBlockTicks = 600;

}

const DownedSoldier *BotRevive::find(const ServerView &p_view) const {
	const DownedSoldier *d = nullptr;
	for (const DownedSoldier &x : p_view.downed()) {
		if (x.slot == slot_) {
			d = &x;
		}
	}
	return d;
}

const DownedSoldier *BotRevive::update(TdmShared &r_shared, BotNavigation &r_navigation, const ReviveCheck &p_check) {
	TdmShared &sh = r_shared;
	const TdmBotConfig &c = *sh.config;
	const ServerView &view = *sh.host.view;
	const ReviveCheck &q = p_check;
	const sim::Tick tick = q.tick;
	if (slot_ >= 0) {
		const DownedSoldier *d = find(view);
		const bool over = d == nullptr || d->until_tick <= tick || tick - since_ > kReviveTimeoutTicks || q.shot_recent ||
				(q.seen != nullptr && distance(q.eye, sh.chest_point(*q.seen)) < 40.0f);
		if (over) {
			if (d != nullptr) {
				sh.count(&TdmBotStats::revive_aborts, uint64_t(1));
			}
			slot_ = -1;
			r_navigation.clear_goal();
		}
	}
	const uint32_t idle_ticks = sim::seconds_to_ticks(std::max(c.revive_idle_s, 0.0f));
	if (slot_ < 0 && q.kit == uint8_t(KitRole::Assault) && q.defibrillator && tick - q.last_engaged >= idle_ticks &&
			(q.shot_at_tick == 0 || tick - q.shot_at_tick >= idle_ticks) && q.self->health >= c.retreat_below_hp) {
		float best = 1e30f;
		const uint8_t team = q.slot < sh.teams.size() ? sh.teams[q.slot] : 0;
		for (const DownedSoldier &d : view.downed()) {
			if (d.team != team || d.slot == q.slot || d.until_tick <= tick || !finite(d.corpse)) {
				continue;
			}
			const float dist = distance(q.self->position, d.corpse);
			const float travel = dist * kPathFactor / kRunSpeed + std::max(c.revive_margin_s, 0.0f);
			if (dist > c.revive_radius_m || float(d.until_tick - tick) * sim::kTickSeconds <= travel) {
				continue;
			}
			if (dist < best) {
				best = dist;
				slot_ = d.slot;
			}
		}
		if (slot_ >= 0) {
			since_ = tick;
			defib_next_ = tick;
			sh.count(&TdmBotStats::revive_attempts, uint64_t(1));
		}
	}
	return slot_ >= 0 ? find(view) : nullptr;
}

void BotRevive::fired_defib(sim::Tick p_tick) {
	defib_next_ = p_tick + kDefibRetryTicks;
}

bool BotGrenade::can_start(const TdmShared &p_shared, const BotTargeting &p_targeting,
		const sim::bots::Perception &p_perception, sim::Tick p_tick, const sim::SoldierState &p_self,
		std::span<const sim::SoldierState> p_states, bool p_have, sim::Vec3 *r_at) const {
	const TdmBotConfig &c = *p_shared.config;
	if (!p_have || p_targeting.lost_target() < 0 || p_tick < next_ ||
			size_t(p_targeting.lost_target()) >= p_states.size()) {
		return false;
	}
	const sim::bots::PerceivedEnemy *e = p_perception.find(uint8_t(p_targeting.lost_target()));
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
	if (r_at != nullptr) {
		*r_at = e->last_position;
	}
	return true;
}

bool BotGrenade::start(TdmShared &r_shared, BotTargeting &r_targeting, const sim::bots::Perception &p_perception,
		sim::Tick p_tick, const sim::SoldierState &p_self, std::span<const sim::SoldierState> p_states, bool p_have) {
	const TdmBotConfig &c = *r_shared.config;
	sim::Vec3 at{};
	if (!can_start(r_shared, r_targeting, p_perception, p_tick, p_self, p_states, p_have, &at)) {
		return false;
	}
	next_ = p_tick + sim::seconds_to_ticks(std::max(c.grenade_every_s, 1.0f));
	phase_ = 1;
	phase_tick_ = p_tick;
	at_ = at;
	r_targeting.forget_lost();
	r_shared.count(&TdmBotStats::grenades, uint64_t(1));
	return true;
}

void BotGrenade::step(const BotArms &p_arms, sim::Tick p_tick, const sim::Vec3 &p_eye, uint8_t &r_slot, Trigger &r_trigger, float &r_yaw, float &r_pitch,
		bool &r_aim) {
	const sw::SoldierWeaponsState *ws = p_arms.weapons();
	const sw::WeaponSimDef *d = p_arms.def(kSlotGrenade);
	if (d == nullptr) {
		phase_ = 0;
		return;
	}

	const float dx = at_[0] - p_eye[0], dz = at_[2] - p_eye[2];
	const float h = std::sqrt(dx * dx + dz * dz);
	const float v = std::max(d->projectile.muzzle_speed * std::max(d->hold_min_power, 0.1f), 1.0f);
	const float gr = std::max(-d->projectile.gravity, 0.1f);
	const float s = std::clamp(gr * h / (v * v), 0.0f, 1.0f);
	r_yaw = yaw_of(dx, dz);
	r_pitch = std::clamp(0.5f * std::asin(s), 10.0f * core::kDegToRadF, 45.0f * core::kDegToRadF);
	r_aim = true;
	r_slot = kSlotGrenade;
	r_trigger = Trigger::Grenade;
	const sw::WeaponSlotState &ss = ws->slots[kSlotGrenade];
	const bool in_hand = ws->active == kSlotGrenade && ws->pending == 0xFF;
	if (phase_ == 1) {
		if (in_hand && ss.phase == sw::WeaponPhase::Ready) {
			phase_ = 2;
			phase_tick_ = p_tick;
			hold_ = uint32_t(std::max<sw::SimTime>(d->hold_delay, 0) * sim::kSimHz / sw::kTimeUnitsPerSecond) + kGrenadeHoldExtraTicks;
		} else if (p_tick - phase_tick_ > kGrenadeSwitchTicks) {
			phase_ = 0;
		}
	} else if (phase_ == 2) {
		if (p_tick - phase_tick_ >= hold_) {
			phase_ = 3;
			phase_tick_ = p_tick;
		}
	} else if (phase_ == 3 && p_tick - phase_tick_ >= kGrenadeAfterTicks) {
		phase_ = 0;
	}
}

bool BotRetreat::recovered(const TdmBotConfig &p_config, bool p_waiting, sim::Tick p_tick, float p_health) const {
	return p_health >= p_config.regen_until_hp || (p_waiting && p_tick - regen_since_ > sim::seconds_to_ticks(p_config.regen_wait_s));
}

bool BotRetreat::cleared(sim::Tick p_tick) const {
	return p_tick - clear_since_ >= kRetreatClearTicks;
}

bool BotRetreat::running(sim::Tick p_tick) const {
	return p_tick - since_ < kRetreatMaxTicks;
}

bool BotRetreat::start(TdmShared &r_shared, BotNavigation &r_navigation, sim::Tick p_tick, const sim::SoldierState &p_self, const sim::Vec3 &p_threat,
		uint64_t &r_rng) {
	if (!finite(p_threat)) {
		return false;
	}
	float dx = p_self.position[0] - p_threat[0], dz = p_self.position[2] - p_threat[2];
	const float l = std::sqrt(dx * dx + dz * dz);
	if (!(l > 0.1f)) {
		const float a = unit(r_rng) * sim::kTwoPi;
		dx = std::sin(a);
		dz = std::cos(a);
	} else {
		dx /= l;
		dz /= l;
	}
	const sim::Vec3 away = add(p_self.position, dx * kRetreatMetres, 0.0f, dz * kRetreatMetres);
	if (!r_navigation.set_goal(r_shared, p_tick, away, 2.0f, true)) {
		return false;
	}
	threat_ = p_threat;
	since_ = p_tick;
	clear_since_ = p_tick;
	r_shared.count(&TdmBotStats::retreats, uint64_t(1));
	return true;
}

int BotHunt::known_enemy(const sim::bots::Perception &p_perception, const sim::SoldierState &p_self) const {
	float best = 1e30f;
	int pick = -1;
	for (const sim::bots::PerceivedEnemy &e : p_perception.enemies()) {
		if (!(e.detected || e.heard) || e.last_seen < searched_ || !finite(e.last_position)) {
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

bool BotHunt::wanted(sim::Tick p_tick, const sim::bots::Perception &p_perception, const sim::SoldierState &p_self) const {
	return p_tick >= block_until_ && known_enemy(p_perception, p_self) >= 0;
}

bool BotHunt::enemy_within(
		const sim::bots::Perception &p_perception, const sim::SoldierState &p_self, float p_metres) const {
	const int k = known_enemy(p_perception, p_self);
	const sim::bots::PerceivedEnemy *const e = k >= 0 ? p_perception.find(uint8_t(k)) : nullptr;
	return e != nullptr && horizontal(p_self.position, e->last_position) <= p_metres;
}

void BotHunt::step(TdmShared &r_shared, BotNavigation &r_navigation, const sim::bots::Perception &p_perception, sim::Tick p_tick, const sim::SoldierState &p_self) {
	if (p_tick - since_ > kHuntGiveUpTicks) {
		searched_ = p_tick;
		block_until_ = p_tick + kHuntBlockTicks;
		r_navigation.clear_goal();
		r_shared.count(&TdmBotStats::hunts_given_up, uint64_t(1));
	}
	const int k = known_enemy(p_perception, p_self);
	if (k >= 0) {
		const sim::bots::PerceivedEnemy *e = p_perception.find(uint8_t(k));
		if (e != nullptr) {
			if (horizontal(p_self.position, e->last_position) <= kHuntArriveMetres) {
				searched_ = p_tick;
			} else {
				r_navigation.set_goal(r_shared, p_tick, e->last_position, kHuntArriveMetres, false);
			}
		}
	}
}

}

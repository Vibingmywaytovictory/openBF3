#include "tdm_bot_objectives.h"

#include "bf3/host/kits.h"

#include <algorithm>

namespace bf3::host::tdm_bot {

namespace {

constexpr float kRunSpeed = 4.5f;
constexpr float kPathFactor = 1.3f;
constexpr uint32_t kReviveTimeoutTicks = 900;

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

}

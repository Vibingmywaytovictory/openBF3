#include "tdm_bot_weapons.h"

#include "bf3/core/math.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/server_view.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace bf3::host::tdm_bot {

namespace {

namespace sw = sim::weapons;

constexpr uint32_t kModeDelayTicks = 3;
constexpr uint32_t kModeWaitTicks = 10;
constexpr float kSingleShotBeyondMetres = 60.0f;
constexpr float kTacticalReload = 0.3f;
constexpr float kFriendlyLineMetres = 0.8f;
constexpr float kBurstPauseMinS = 0.15f, kBurstPauseMaxS = 0.35f;

}

BotArms::BotArms(const Gunplay *p_gunplay, uint8_t p_slot) {
	if (p_gunplay != nullptr && p_gunplay->armed(p_slot)) {
		ws_ = &p_gunplay->weapons(p_slot);
		table_ = &p_gunplay->table();
	}
}

const sw::WeaponSimDef *BotArms::def(uint8_t p_slot) const {
	if (ws_ == nullptr || p_slot >= sw::kMaxWeaponSlots) {
		return nullptr;
	}
	const uint16_t d = ws_->slots[p_slot].def;
	return d != sw::kNoWeapon && d < table_->defs.size() ? &table_->defs[d] : nullptr;
}

int32_t BotArms::rounds(uint8_t p_slot) const {
	const sw::WeaponSimDef *d = def(p_slot);
	if (d == nullptr) {
		return 0;
	}
	return d->unlimited_ammo ? 1000 : ws_->slots[p_slot].magazine + ws_->slots[p_slot].reserve;
}

void BotWeapons::new_life(sim::Tick p_tick) {
	bursting_ = false;
	pause_until_ = next_tap_ = p_tick;
	tapped_ = false;
	mode_wait_ = 0;
	reload_held_ = false;
	sidearm_ = false;
}

WeaponCommand BotWeapons::input(TdmShared &r_shared, uint8_t p_slot, const BotArms &p_arms, const FireRequest &p_request, uint64_t &r_rng) {
	TdmShared &sh = r_shared;
	const TdmBotConfig &c = *sh.config;
	const FireRequest &q = p_request;
	const sim::Tick tick = q.tick;
	const sw::SoldierWeaponsState *ws = p_arms.weapons();
	uint8_t want_slot = q.want_slot;
	Trigger trigger = q.trigger;
	WeaponCommand out;
	sw::WeaponInput &in = out.input;
	in.fire_mode_counter = mode_counter_;
	if (ws != nullptr) {

		if (!sidearm_ && p_arms.def(kSlotPrimary) != nullptr && p_arms.rounds(kSlotPrimary) <= 0 && p_arms.rounds(kSlotSidearm) > 0) {
			sidearm_ = true;
			sh.count(&TdmBotStats::sidearm_switches, uint64_t(1));
			if (want_slot == kSlotPrimary) {
				want_slot = kSlotSidearm;
			}
		}
		if (p_arms.def(want_slot) == nullptr) {
			want_slot = p_arms.def(kSlotPrimary) != nullptr ? kSlotPrimary : ws->active;
			if (trigger == Trigger::Defib || trigger == Trigger::Knife || trigger == Trigger::Grenade) {
				trigger = Trigger::None;
			}
		}
		in.select_slot = want_slot;
		const sw::WeaponSlotState &ss = ws->slots[ws->active < sw::kMaxWeaponSlots ? ws->active : 0];
		const sw::WeaponSimDef *d = p_arms.def(ws->active);
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
					tick > q.life_tick + kModeDelayTicks && tick >= mode_wait_ && modes > 1) {
				const uint8_t steps = uint8_t((uint32_t(auto_mode) + modes - ss.mode) % modes);
				if (steps > 0 && steps <= 3) {
					mode_counter_ = uint8_t((mode_counter_ + steps) & 3u);
					in.fire_mode_counter = mode_counter_;
					mode_wait_ = tick + kModeWaitTicks;
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
			const bool tactical = gun && q.quiet && !busy && reserve && float(ss.magazine) < kTacticalReload * capacity;
			const bool reload = need || tactical;
			if (reload && !reload_held_) {
				sh.count(&TdmBotStats::reloads, uint64_t(1));
			}
			reload_held_ = reload;
			in.reload = reload;
			bool fire = false;
			if (trigger == Trigger::Grenade) {
				fire = q.throw_ready && in_hand;
			} else if (trigger == Trigger::Defib) {
				if (in_hand && q.aligned && phase == sw::WeaponPhase::Ready && q.defib_ready && !tapped_) {
					fire = true;
					tapped_ = true;
					out.defib_fired = true;
					sh.count(&TdmBotStats::revive_shots, uint64_t(1));
				} else {
					tapped_ = false;
				}
			} else if (trigger == Trigger::Knife) {
				if (in_hand && q.aligned && phase == sw::WeaponPhase::Ready && !tapped_) {
					fire = true;
					tapped_ = true;
				} else {
					tapped_ = false;
				}
			} else if (trigger == Trigger::Aimed || trigger == Trigger::Blind) {
				const bool ready = trigger == Trigger::Blind || tick >= q.react_at;
				const bool want_fire = in_hand && q.aligned && ready && !busy && !reload && ss.magazine > 0 && !q.sprint &&
						!friend_in_line(sh, p_slot, q.position, q.eye, q.view_yaw, q.view_pitch, q.target_distance);
				const bool single = q.target_distance > kSingleShotBeyondMetres || trigger == Trigger::Blind;
				if (automatic && !single) {
					if (bursting_) {
						const uint32_t fired = ws->next_shot_index - burst_start_;
						if (fired >= burst_rounds_ || !want_fire) {
							bursting_ = false;
							pause_until_ = tick + sim::seconds_to_ticks(uniform(r_rng, kBurstPauseMinS, kBurstPauseMaxS));
						} else {
							fire = true;
						}
					} else if (want_fire && tick >= pause_until_) {

						const float share = std::clamp(q.target_distance / kSingleShotBeyondMetres, 0.0f, 1.0f);
						const uint32_t lo = std::max(c.burst_min_rounds, 1u), hi = std::max(c.burst_max_rounds, lo);
						const uint32_t top = std::max(lo, uint32_t(std::lround(float(hi) - share * float(hi - lo))));
						burst_rounds_ = uniform_u(r_rng, lo, top);
						burst_start_ = ws->next_shot_index;
						bursting_ = true;
						fire = true;
						sh.count(&TdmBotStats::bursts, uint64_t(1));
					}
				} else if (tapped_) {
					tapped_ = false;
				} else if (want_fire && tick >= next_tap_) {
					fire = true;
					tapped_ = true;
					const float rate = q.target_distance > kSingleShotBeyondMetres ? 1.5f : 3.0f;
					next_tap_ = tick + std::max<uint32_t>(2u, uint32_t(std::lround(float(sim::kSimHz) / rate)));
					sh.count(&TdmBotStats::bursts, uint64_t(1));
				}
				if (fire && sh.observer != nullptr) {
					sh.observer->fired(*sh.host.view, tick, p_slot, trigger == Trigger::Aimed ? q.target : -1, q.last_visible, trigger == Trigger::Blind);
				}
			}
			in.fire = fire;
			in.zoom = q.zoom && gun && in_hand && !busy;

			if (gun) {
				out.comp_yaw = q.skill * ss.recoil.yaw * core::kDegToRadF;
				out.comp_pitch = q.skill * ss.recoil.pitch * core::kDegToRadF;
			}
		}
	}
	return out;
}

bool friend_in_line(const TdmShared &p_shared, uint8_t p_slot, const sim::Vec3 &p_position, const sim::Vec3 &p_eye, float p_yaw, float p_pitch, float p_length) {
	const TdmShared &sh = p_shared;
	const uint8_t team = p_slot < sh.teams.size() ? sh.teams[p_slot] : 0;
	const std::span<const sim::SoldierState> states = sh.host.view->states();
	const sim::Vec3 dir = view_dir(p_yaw, p_pitch);
	const float length = std::max(p_length - 0.5f, 0.0f);
	for (size_t s = 0; s < states.size() && s < sh.teams.size() && s < sh.alive.size(); ++s) {
		if (s == p_slot || team == 0 || sh.teams[s] != team || sh.alive[s] == 0 || !finite(states[s].position)) {
			continue;
		}
		if (horizontal(states[s].position, p_position) > length + 1.0f) {
			continue;
		}
		if (ray_point_distance(p_eye, dir, length, sh.chest_point(states[s])) <= kFriendlyLineMetres) {
			return true;
		}
	}
	return false;
}

}

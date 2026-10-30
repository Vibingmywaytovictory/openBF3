#include "tdm_bot_body.h"

#include "bf3/core/math.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/sim/bots/aim_model.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <algorithm>
#include <cmath>

namespace bf3::host::tdm_bot {

namespace {

namespace sw = sim::weapons;

constexpr float kTurnRateDegS = 540.0f;
constexpr float kPitchTurnRateDegS = 360.0f;
constexpr float kMaxPitchDeg = 80.0f;
constexpr float kFireConeDeg = 4.0f;

constexpr float kSprintConeDeg = 20.0f;
constexpr float kReviveDirectMetres = 2.5f;
constexpr float kReviveStandMetres = 0.8f;
constexpr float kReviveCrouchMetres = 2.0f;
constexpr float kReviveCentreUp = 0.3f;
constexpr float kDefibReach = 1.7f;
constexpr float kDefibConeDeg = 6.0f;
constexpr float kKnifeBehindDeg = 100.0f;
constexpr uint32_t kKnifeTicks = 45;

constexpr float kHeadAimDrop = 0.1f;
constexpr float kZoomBeyondMetres = 6.0f;
constexpr uint32_t kHoldTicks = 240;

}

void BotBody::attach(std::shared_ptr<TdmShared> p_shared, uint32_t p_index) {
	shared = std::move(p_shared);
	index = p_index;
	if (shared != nullptr) {
		awareness.configure(shared->config->perception);
	}
}

void BotBody::set_slot(uint8_t p_slot) {
	slot = p_slot;
	seed = (shared != nullptr ? shared->host.match_seed : 0) ^ uint64_t(p_slot);
	navigation.set_slot(p_slot, seed);
	life = UINT32_MAX;
}

void BotBody::new_life(sim::Tick p_tick, const sim::SoldierState &p_state) {
	const TdmBotConfig &c = cfg();
	life = p_state.life;
	life_tick = p_tick;
	rng = mix(seed ^ (uint64_t(life) << 8));
	const float lo = std::min(c.skill_min, c.skill_max), hi = std::max(c.skill_min, c.skill_max);
	skill01 = unit(rng);
	skill = std::clamp(lo + (hi - lo) * skill01, 0.0f, 1.0f);
	const ServerView &view = *shared->host.view;
	kit = shared->host.rules ? view.kit(slot)
							 : (shared->host.gunplay != nullptr ? shared->host.gunplay->kit(slot) : uint8_t(0));
	view_yaw = sim::yaw_radians(p_state.yaw);
	view_pitch = 0.0f;
	awareness.new_life(p_tick);
	targeting.new_life();
	hunt.new_life();
	weapons.new_life(p_tick);
	strafe.stop();
	look.new_life();
	retreat.new_life();
	medic.new_life();
	grenade.new_life();
	knife_until = 0;
	navigation.new_life();
	activity = TdmBotActivity::Roam;
}

void BotBody::set_activity(sim::Tick p_tick, TdmBotActivity p_activity) {
	activity = p_activity;
	TdmShared &sh = *shared;
	if (sh.stats != nullptr) {
		sh.stats->activity_ticks[size_t(p_activity)].fetch_add(1, std::memory_order_relaxed);
	}
	if (sh.observer != nullptr) {
		sh.observer->activity(p_tick, slot, p_activity);
	}
}

void BotBody::restricted(sim::Tick p_tick, sim::Command &r_cmd) {
	set_activity(p_tick, TdmBotActivity::Restricted);
	r_cmd.yaw = sim::quantize_yaw(view_yaw);
	r_cmd.pitch = sim::quantize_pitch(view_pitch);
	weapons.hold_fire();
}

Sighting BotBody::see(sim::Tick p_tick, const sim::SoldierState &p_state, std::span<const sim::SoldierState> p_states) {
	TdmShared &sh = *shared;
	const bool perceived =
			awareness.sense(sh, slot, seed ^ uint64_t(life), p_tick, p_state, p_states, view_yaw, view_pitch);
	return targeting.sense(sh, awareness, perceived, p_tick, p_state, p_states, skill01, rng);
}

BotFrame BotBody::sense_revive(
		sim::Tick p_tick, const sim::SoldierState &p_state, const ServerView &p_view, const Sighting &p_sighting) {
	TdmShared &sh = *shared;
	const std::span<const sim::SoldierState> states = p_view.states();
	const sim::Vec3 eye = sh.eye_point(p_state);
	const int target = targeting.target();
	const bool shot_recent = awareness.shot_recent(p_tick);
	const BotArms arms(sh.host.gunplay, slot);

	ReviveCheck check;
	check.tick = p_tick;
	check.slot = slot;
	check.kit = kit;
	check.defibrillator = arms.def(kKitGadgetSlot) != nullptr;
	check.self = &p_state;
	check.eye = eye;
	check.shot_recent = shot_recent;
	check.shot_at_tick = awareness.shot_at_tick();
	check.last_engaged = targeting.last_engaged();
	check.seen = p_sighting.visible && p_sighting.enemy != nullptr ? &states[size_t(target)] : nullptr;
	const DownedSoldier *revive = medic.update(sh, navigation, check);

	return BotFrame{ p_tick, p_state, states, p_view.world(), eye, p_sighting.enemy, p_sighting.visible, target,
		shot_recent, arms, revive };
}

BotIntent BotBody::begin_plan(TdmBotActivity p_activity) {
	BotIntent i;
	i.want_yaw = view_yaw;
	i.want_slot = weapons.preferred_slot();
	i.walk_yaw = view_yaw;
	if (p_activity != TdmBotActivity::Fight) {
		strafe.stop();
	}
	return i;
}

void BotBody::face_shots(const BotFrame &p_frame, TdmBotActivity p_activity, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	if (!i.have_aim && f.shot_recent && p_activity != TdmBotActivity::Retreat && p_activity != TdmBotActivity::Revive) {
		i.want_yaw =
				yaw_of(awareness.shot_from()[0] - f.self.position[0], awareness.shot_from()[2] - f.self.position[2]);
		i.sprint = false;
	}
}

bool BotBody::turn(const BotFrame &p_frame, BotIntent &r_intent) {
	BotIntent &i = r_intent;
	const float yaw_step = kTurnRateDegS * core::kDegToRadF * sim::kTickSeconds;
	const float pitch_step = kPitchTurnRateDegS * core::kDegToRadF * sim::kTickSeconds;
	view_yaw = wrap(view_yaw + std::clamp(wrap(i.want_yaw - view_yaw), -yaw_step, yaw_step));
	i.want_pitch = std::clamp(i.want_pitch, -kMaxPitchDeg * core::kDegToRadF, kMaxPitchDeg * core::kDegToRadF);
	view_pitch += std::clamp(i.want_pitch - view_pitch, -pitch_step, pitch_step);
	if (!std::isfinite(view_yaw) || !std::isfinite(view_pitch)) {
		view_yaw = sim::yaw_radians(p_frame.self.yaw);
		view_pitch = 0.0f;
	}
	const float cone = (i.trigger == Trigger::Defib ? kDefibConeDeg : kFireConeDeg) * core::kDegToRadF;
	const bool aligned = i.have_aim && std::fabs(wrap(i.want_yaw - view_yaw)) <= cone &&
			std::fabs(i.want_pitch - view_pitch) <= cone;
	if (i.sprint) {
		const float len = std::sqrt(i.move_dx * i.move_dx + i.move_dz * i.move_dz);
		i.sprint = !i.have_aim && len > 0.5f &&
				std::fabs(wrap(yaw_of(i.move_dx, i.move_dz) - view_yaw)) <= kSprintConeDeg * core::kDegToRadF;
	}
	return aligned;
}

void BotBody::compose(const BotFrame &p_frame, TdmBotActivity p_activity, const BotIntent &p_intent, bool p_aligned,
		sim::Command &r_cmd) {
	const BotFrame &f = p_frame;
	const BotIntent &i = p_intent;
	FireRequest request;
	request.tick = f.tick;
	request.life_tick = life_tick;
	request.want_slot = i.want_slot;
	request.trigger = i.trigger;
	request.aligned = p_aligned;
	request.sprint = i.sprint;
	request.zoom = i.zoom;
	request.quiet = !f.visible && f.tick - targeting.last_engaged() > 120u && p_activity != TdmBotActivity::Revive;
	request.throw_ready = grenade.throw_ready();
	request.defib_ready = medic.defib_ready(f.tick);
	request.react_at = targeting.react_at();
	request.target_distance = i.target_distance;
	request.target = f.target;
	request.last_visible = f.te != nullptr ? f.te->last_visible : 0;
	request.skill = skill;
	request.position = f.self.position;
	request.eye = f.eye;
	request.view_yaw = view_yaw;
	request.view_pitch = view_pitch;
	const WeaponCommand weapon = weapons.input(*shared, slot, f.arms, request, rng);
	if (weapon.defib_fired) {
		medic.fired_defib(f.tick);
	}
	const sw::WeaponInput &in = weapon.input;
	const float comp_yaw = weapon.comp_yaw, comp_pitch = weapon.comp_pitch;

	sim::Command &cmd = r_cmd;
	cmd.yaw = sim::quantize_yaw(wrap(view_yaw - comp_yaw));
	cmd.pitch = sim::quantize_pitch(
			std::clamp(view_pitch - comp_pitch, -kMaxPitchDeg * core::kDegToRadF, kMaxPitchDeg * core::kDegToRadF));
	cmd.pose = i.pose;
	const float sy = std::sin(view_yaw), cy = std::cos(view_yaw);
	const float fwd = i.move_dx * sy + i.move_dz * cy;
	const float rgt = -i.move_dx * cy + i.move_dz * sy;
	cmd.move_forward = sim::quantize_axis(std::clamp(std::isfinite(fwd) ? fwd : 0.0f, -1.0f, 1.0f));
	cmd.move_right = sim::quantize_axis(std::clamp(std::isfinite(rgt) ? rgt : 0.0f, -1.0f, 1.0f));
	uint32_t b = i.sprint ? sim::buttons::kSprint : 0u;
	if (i.walk_jump) {
		b |= sim::buttons::kJump;
	}
	b = sw::with_weapon_input(b, in);
	cmd.buttons = b;
}

bool BotBody::walk(const BotFrame &p_frame, BotIntent &r_intent) {
	BotIntent &i = r_intent;
	if (navigation.walk(*shared, p_frame.tick, p_frame.self, i.walk_dx, i.walk_dz, i.walk_yaw, i.walk_sprint,
				i.walk_pose, i.walk_jump)) {
		i.move_dx = i.walk_dx;
		i.move_dz = i.walk_dz;
		i.pose = i.walk_pose;
		return true;
	}
	i.walk_yaw = look.look(p_frame.tick, awareness.perception(), view_yaw, rng);
	return false;
}

void BotBody::walk_facing(const BotFrame &p_frame, BotIntent &r_intent) {
	walk(p_frame, r_intent);
	r_intent.want_yaw = r_intent.walk_yaw;
	r_intent.sprint = r_intent.walk_sprint;
}

void BotBody::roam(const BotFrame &p_frame, BotIntent &r_intent) {
	navigation.pick_roam_goal(*shared, p_frame.tick, p_frame.self.position, rng);
	walk_facing(p_frame, r_intent);
}

void BotBody::hunt_step(const BotFrame &p_frame, BotIntent &r_intent) {
	hunt.step(*shared, navigation, awareness.perception(), p_frame.tick, p_frame.self);
	walk_facing(p_frame, r_intent);
}

void BotBody::crouch(BotIntent &r_intent) {
	r_intent.pose = sim::Pose::Crouch;
}

void BotBody::face_threat(const BotFrame &p_frame, BotIntent &r_intent) {
	r_intent.want_yaw =
			yaw_of(retreat.threat()[0] - p_frame.self.position[0], retreat.threat()[2] - p_frame.self.position[2]);
}

void BotBody::look_around(const BotFrame &p_frame, BotIntent &r_intent) {
	r_intent.walk_yaw = look.look(p_frame.tick, awareness.perception(), view_yaw, rng);
	r_intent.want_yaw = r_intent.walk_yaw;
	r_intent.sprint = r_intent.walk_sprint;
}

void BotBody::aim_blind(const BotFrame &p_frame, BotIntent &r_intent) {
	r_intent.target_distance = r_intent.aim_at(p_frame.eye, targeting.blind_at());
}

void BotBody::trigger_blind(BotIntent &r_intent) {
	r_intent.trigger = Trigger::Blind;
}

void BotBody::aim_target(const BotFrame &p_frame, BotIntent &r_intent) {
	TdmShared &sh = *shared;
	const TdmBotConfig &c = cfg();
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	const sim::SoldierState &ts = f.states[size_t(f.target)];
	const sim::Vec3 chest = sh.chest_point(ts);
	const sw::SoldierWeaponsState *ws = f.arms.weapons();
	const sw::WeaponSimDef *d = f.arms.def(ws != nullptr ? ws->active : 0);
	sim::Vec3 point = chest;
	if (f.te != nullptr && !f.te->chest_visible) {
		point = add(sim::bots::perception_head(c.perception, ts, sh.host.eye_heights), 0.0f, -kHeadAimDrop, 0.0f);
	}
	const sim::Vec3 mark = point;
	if (d != nullptr && !d->projectile.instant_hit) {
		point = sim::bots::lead_point(f.eye, mark, ts.velocity, d->projectile.muzzle_speed, d->projectile.gravity);
	}
	i.target_distance = i.aim_at(f.eye, point);

	const sim::Vec3 los = sub(chest, f.eye);
	const float l = std::max(distance(f.eye, chest), 1e-3f);
	const float along = (ts.velocity[0] * los[0] + ts.velocity[1] * los[1] + ts.velocity[2] * los[2]) / l;
	const float vx = ts.velocity[0] - along * los[0] / l, vy = ts.velocity[1] - along * los[1] / l,
				vz = ts.velocity[2] - along * los[2] / l;
	const float crossing = std::sqrt(vx * vx + vy * vy + vz * vz);
	const float own = std::sqrt(f.self.velocity[0] * f.self.velocity[0] + f.self.velocity[2] * f.self.velocity[2]);
	const float on = float(f.tick - targeting.target_since()) * sim::kTickSeconds;
	const float sigma = sim::bots::aim_error(c.aim, skill, on, own > 0.5f, std::isfinite(crossing) ? crossing : 0.0f) *
			core::kDegToRadF;
	const AimError err = targeting.aim_error(f.tick, sigma, rng);
	i.want_yaw = wrap(i.want_yaw + err.yaw);
	i.want_pitch += err.pitch;
}

void BotBody::trigger_target(const BotFrame &p_frame, BotIntent &r_intent) {
	TdmShared &sh = *shared;
	const TdmBotConfig &c = cfg();
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	const sim::SoldierState &ts = f.states[size_t(f.target)];
	i.trigger = Trigger::Aimed;
	const float hd = horizontal(f.self.position, ts.position);
	if (hd <= c.knife_within_m && f.arms.def(kSlotKnife) != nullptr) {
		const float behind =
				std::fabs(wrap(yaw_of(f.self.position[0] - ts.position[0], f.self.position[2] - ts.position[2]) -
						sim::yaw_radians(ts.yaw)));
		if (behind >= kKnifeBehindDeg * core::kDegToRadF && knife_until <= f.tick) {
			knife_until = f.tick + kKnifeTicks;
			sh.count(&TdmBotStats::knife_attacks, uint64_t(1));
		}
	}
	if (knife_until > f.tick) {
		i.want_slot = kSlotKnife;
		i.trigger = Trigger::Knife;
		i.aim_at(f.eye, sh.chest_point(ts));
	}
	i.zoom = i.target_distance > kZoomBeyondMetres && i.trigger == Trigger::Aimed;
}

bool BotBody::hold_window(sim::Tick p_tick) {
	if (p_tick - hold_from >= 2u * kHoldTicks) {
		hold_from = p_tick;
	}
	return p_tick - hold_from < kHoldTicks;
}

void BotBody::move_fight(const BotFrame &p_frame, BotIntent &r_intent) {
	TdmShared &sh = *shared;
	const TdmBotConfig &c = cfg();
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	const sim::SoldierState &ts = f.states[size_t(f.target)];
	if (i.target_distance < c.engage_strafe_m) {
		strafe.step(c, slot, f.tick, f.world, f.self, view_yaw, rng, i.move_dx, i.move_dz);
	} else if (i.target_distance > c.crouch_beyond_m && skill > 0.5f &&
			f.tick - targeting.target_since() < kHoldTicks && hold_window(f.tick)) {
		strafe.stop();
		i.pose = sim::Pose::Crouch;
	} else if (navigation.set_goal(sh, f.tick, ts.position, kHuntArriveMetres, false) && walk(f, i)) {
		strafe.stop();
		i.pose = sim::Pose::Stand;
	} else {
		strafe.step(c, slot, f.tick, f.world, f.self, view_yaw, rng, i.move_dx, i.move_dz);
	}
}

bool BotBody::revive_approach(const BotFrame &p_frame, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	const sim::Vec3 &corpse = f.revive->corpse;
	const float hd = horizontal(f.self.position, corpse);
	if (hd > kReviveDirectMetres) {
		navigation.set_goal(*shared, f.tick, corpse, 1.0f, true);
		walk(f, i);
		i.want_yaw = i.walk_yaw;
		return false;
	}
	if (hd > kReviveStandMetres) {
		const float inv = 0.6f / std::max(hd, 1e-3f);
		i.move_dx = (corpse[0] - f.self.position[0]) * inv;
		i.move_dz = (corpse[2] - f.self.position[2]) * inv;
	}
	return true;
}

void BotBody::revive_pose(const BotFrame &p_frame, BotIntent &r_intent) {
	const float hd = horizontal(p_frame.self.position, p_frame.revive->corpse);
	if (hd <= kReviveDirectMetres) {
		r_intent.pose = hd < kReviveCrouchMetres ? sim::Pose::Crouch : sim::Pose::Stand;
	}
}

void BotBody::revive_trigger(const BotFrame &p_frame, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	const sim::Vec3 &corpse = f.revive->corpse;
	if (horizontal(f.self.position, corpse) > kReviveDirectMetres) {
		return;
	}
	i.want_slot = kKitGadgetSlot;
	const float reach = i.aim_at(f.eye, add(corpse, 0.0f, kReviveCentreUp, 0.0f));
	if (reach <= kDefibReach) {
		i.trigger = Trigger::Defib;
	}
}

void BotBody::throw_step(const BotFrame &p_frame, BotIntent &r_intent) {
	BotIntent &i = r_intent;
	grenade.step(p_frame.arms, p_frame.tick, p_frame.eye, i.want_slot, i.trigger, i.want_yaw, i.want_pitch, i.have_aim);
}

}

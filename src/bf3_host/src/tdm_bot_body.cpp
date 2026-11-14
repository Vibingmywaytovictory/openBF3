#include "tdm_bot_body.h"

#include "bf3/ai/main/bot_skill.h"
#include "bf3/core/math.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/static_world.h"
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

constexpr float kThrowReachSlack = 0.5f;
constexpr float kBagArriveMetres = 1.0f;

constexpr float kHeadAimDrop = 0.1f;
constexpr float kZoomBeyondMetres = 6.0f;
constexpr uint32_t kHoldTicks = 240;

}

float aim_sigma_deg(
		const TdmBotConfig &p_config, const sw::WeaponSimDef *p_weapon, float p_skill, double p_seconds_on) {
	ai::TriggerWeapon weapon;
	weapon.explosion_radius = p_weapon != nullptr && p_weapon->projectile.explosion.present
			? p_weapon->projectile.explosion.radius
			: 0.0f;
	return ai::weapon_deviation_deg(p_config.weapon_templates,
			p_weapon != nullptr ? std::string_view(p_weapon->variant) : std::string_view(), weapon, p_skill,
			p_seconds_on);
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
	skill = std::clamp(c.ai.bot_skill, 0.0f, 1.0f);
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
	support.new_life();
	vehicles.new_life();
	vehicle_aimer_id = 0;
	walked_phase = ExplosivePhase::Approach;
	knife_until = 0;
	navigation.new_life();
	activity = TdmBotActivity::Roam;
}

void BotBody::set_activity(sim::Tick p_tick, TdmBotActivity p_activity) {
	activity = p_activity;
	report_activity(p_tick, p_activity);
}

void BotBody::report_activity(sim::Tick p_tick, TdmBotActivity p_activity) {
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

Sighting BotBody::keep(
		sim::Tick p_tick, const sim::SoldierState &p_state, std::span<const sim::SoldierState> p_states) {
	awareness.check_shots(*shared, slot, p_tick, p_state);
	return targeting.track(*shared, awareness.perception(), p_tick, p_states, rng);
}

Sighting BotBody::see(sim::Tick p_tick, const sim::SoldierState &p_state, std::span<const sim::SoldierState> p_states) {
	TdmShared &sh = *shared;
	awareness.perceive(sh, slot, seed ^ uint64_t(life), p_tick, p_state, p_states, view_yaw, view_pitch);
	return targeting.sense(sh, awareness, p_tick, p_state, p_states, skill, rng);
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
	check.defibrillator = defib_slot(sh, arms).has_value();
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

BotFrame BotBody::hold_frame(
		sim::Tick p_tick, const sim::SoldierState &p_state, const ServerView &p_view, const Sighting &p_sighting) {
	TdmShared &sh = *shared;
	return BotFrame{ p_tick, p_state, p_view.states(), p_view.world(), sh.eye_point(p_state), p_sighting.enemy,
		p_sighting.visible, targeting.target(), awareness.shot_recent(p_tick), BotArms(sh.host.gunplay, slot),
		medic.find(p_view) };
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
	request.quiet = !f.visible && f.tick - targeting.last_engaged() > 120u && p_activity != TdmBotActivity::Revive &&
			p_activity != TdmBotActivity::Support;
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
	if (weapon.supply_fired) {
		support.thrown(*shared, f.tick, i.want_slot);
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
	if (navigation.walk(*shared, p_frame.tick, p_frame.self, p_frame.world, i.walk_dx, i.walk_dz, i.walk_yaw,
				i.walk_sprint, i.walk_pose, i.walk_jump)) {
		i.move_dx = i.walk_dx;
		i.move_dz = i.walk_dz;
		i.pose = i.walk_pose;
		i.walked = true;
		return true;
	}
	i.walk_yaw = look.look(p_frame.tick, awareness.perception(), view_yaw, rng);
	return false;
}

void BotBody::rewalk(const BotFrame &p_frame, BotIntent &r_intent) {
	BotIntent &i = r_intent;
	if (!i.walked ||
			!navigation.walk(*shared, p_frame.tick, p_frame.self, p_frame.world, i.walk_dx, i.walk_dz, i.walk_yaw,
					i.walk_sprint, i.walk_pose, i.walk_jump)) {
		return;
	}
	i.move_dx = i.walk_dx;
	i.move_dz = i.walk_dz;
	i.pose = i.stands ? sim::Pose::Stand : i.walk_pose;
	if (i.walk_faces) {
		i.want_yaw = i.walk_yaw;
	}
	if (i.walk_sprints || i.sprints) {
		i.sprint = i.sprints || i.walk_sprint;
	}
}

void BotBody::walk_facing(const BotFrame &p_frame, BotIntent &r_intent) {
	walk(p_frame, r_intent);
	r_intent.want_yaw = r_intent.walk_yaw;
	r_intent.sprint = r_intent.walk_sprint;
	r_intent.walk_faces = r_intent.walked;
	r_intent.walk_sprints = r_intent.walked;
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
		if (aimer_target != f.target) {
			aimer = ai::Aimer{};
			aimer_target = f.target;
		}
		ai::Vec3 dir{};
		if (ai::aim_vector(aimer, f.eye, mark, ts.velocity, {}, ai::kSoldierAimAccuracy, d->projectile.drag,
					d->projectile.muzzle_speed, d->projectile.gravity, double(f.tick) * sim::kTickSeconds, dir)) {
			const float range = distance(f.eye, mark);
			point = sim::Vec3{ f.eye[0] + dir.x * range, f.eye[1] + dir.y * range, f.eye[2] + dir.z * range };
		}
	}
	i.target_distance = i.aim_at(f.eye, point);

	const double on = double(f.tick - targeting.target_since()) * sim::kTickSeconds;
	const float sigma = aim_sigma_deg(c, d, skill, on) * core::kDegToRadF;
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
	if (const std::optional<SoldierWeapon> w = soldier_weapon(sh, f.arms, i.target_distance)) {
		i.want_slot = w->slot;
		i.trigger = w->trigger;
	}
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
		i.hold_stand();
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
		i.walk_faces = i.walked;
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
	const std::optional<uint8_t> defib = defib_slot(*shared, f.arms);
	if (!defib.has_value()) {
		return;
	}
	i.want_slot = *defib;
	const float reach = i.aim_at(f.eye, add(corpse, 0.0f, kReviveCentreUp, 0.0f));
	if (reach <= kDefibReach) {
		i.trigger = Trigger::Defib;
	}
}

void BotBody::throw_step(const BotFrame &p_frame, BotIntent &r_intent) {
	BotIntent &i = r_intent;
	grenade.step(p_frame.arms, p_frame.tick, p_frame.eye, i.want_slot, i.trigger, i.want_yaw, i.want_pitch, i.have_aim);
}

SupportSense BotBody::sense_support(const BotFrame &p_frame, bool p_engaged, const SupportWay &p_way) {
	SupportCheck check;
	check.tick = p_frame.tick;
	check.slot = slot;
	check.self = &p_frame.self;
	check.states = p_frame.states;
	check.engaged = p_engaged;
	check.way = p_way;
	return support.update(*shared, p_frame.arms, check);
}

bool BotBody::support_approach(const BotFrame &p_frame, SupplyNeed p_need, BotIntent &r_intent) {
	const sim::Vec3 *at = support.target(p_need);
	if (at == nullptr) {
		return false;
	}
	support.pursue(p_need, p_frame.tick);
	if (horizontal(p_frame.self.position, *at) <= cfg().behaviours.throw_m) {
		return true;
	}
	navigation.set_goal(*shared, p_frame.tick, *at, kBagArriveMetres, true, kSquadGoalMoveMetres);
	walk(p_frame, r_intent);
	r_intent.want_yaw = r_intent.walk_yaw;
	r_intent.walk_faces = r_intent.walked;
	return false;
}

void BotBody::support_trigger(const BotFrame &p_frame, SupplyNeed p_need, BotIntent &r_intent) {
	const sim::Vec3 *at = support.target(p_need);
	if (at == nullptr || horizontal(p_frame.self.position, *at) > cfg().behaviours.throw_m + kThrowReachSlack) {
		return;
	}
	support.pursue(p_need, p_frame.tick);
	r_intent.want_slot = support.gadget_slot(p_need);
	r_intent.aim_at(p_frame.eye, add(*at, 0.0f, kReviveCentreUp, 0.0f));
	r_intent.trigger = Trigger::Supply;
}

void BotBody::call_medic(const BotFrame &p_frame) {
	support.post_call(*shared, p_frame.tick, slot);
}

void BotBody::medkit_or_hold(const BotFrame &p_frame, BotIntent &r_intent) {
	const std::optional<sim::Vec3> &bag = support.medkit();
	if (bag.has_value() &&
			navigation.set_goal(*shared, p_frame.tick, *bag, kBagArriveMetres, true, kSquadGoalMoveMetres) &&
			walk(p_frame, r_intent)) {
		r_intent.want_yaw = r_intent.walk_yaw;
		r_intent.walk_faces = true;
		return;
	}
	r_intent.pose = sim::Pose::Crouch;
	look_around(p_frame, r_intent);
}

VehicleCheck BotBody::vehicle_check(const BotFrame &p_frame) const {
	VehicleCheck c;
	c.tick = p_frame.tick;
	c.slot = slot;
	c.self = &p_frame.self;
	c.eye = p_frame.eye;
	c.view_yaw = view_yaw;
	return c;
}

bool BotBody::target_seated(const BotFrame &p_frame) const {
	if (p_frame.target < 0) {
		return false;
	}
	for (const sim::vehicles::VehicleTarget &v : shared->host.view->vehicle_targets()) {
		for (uint8_t k = 0; k < v.crew_count; ++k) {
			if (v.crew[k] == uint32_t(p_frame.target)) {
				return true;
			}
		}
	}
	return false;
}

bool BotBody::sense_vehicle(const BotFrame &p_frame) {
	return vehicles.sense(*shared, p_frame.arms, vehicle_check(p_frame));
}

bool BotBody::sense_repair(const BotFrame &p_frame) {
	return vehicles.sense_repair(*shared, p_frame.arms, vehicle_check(p_frame));
}

void BotBody::vehicle_aim(const BotFrame &p_frame, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	if (!vehicles.track(*shared, f.eye, false)) {
		return;
	}
	const VehicleAim &a = vehicles.enemy();
	if (a.type == ai::WeaponType::C4) {
		const sim::Vec3 at = vehicles.phase() == ExplosivePhase::Place
				? sim::Vec3{ a.near_point[0], f.self.position[1], a.near_point[2] }
				: a.centre;
		i.target_distance = i.aim_at(f.eye, at);
		return;
	}
	const sw::WeaponSimDef *d = f.arms.def(a.weapon_slot);
	sim::Vec3 point = a.centre;
	if (d != nullptr && !d->projectile.instant_hit) {
		if (vehicle_aimer_id != a.id) {
			vehicle_aimer = ai::Aimer{};
			vehicle_aimer_id = a.id;
		}
		ai::Vec3 dir{};
		if (ai::aim_vector(vehicle_aimer, f.eye, a.centre, a.velocity, {}, ai::kSoldierAimAccuracy, d->projectile.drag,
					d->projectile.muzzle_speed, d->projectile.gravity, double(f.tick) * sim::kTickSeconds, dir)) {
			const float range = distance(f.eye, a.centre);
			point = sim::Vec3{ f.eye[0] + dir.x * range, f.eye[1] + dir.y * range, f.eye[2] + dir.z * range };
		}
	}
	i.target_distance = i.aim_at(f.eye, point);
	ai::TriggerWeapon weapon;
	weapon.explosion_radius = d != nullptr && d->projectile.explosion.present ? d->projectile.explosion.radius : 0.0f;
	const double on = double(f.tick - a.since) * sim::kTickSeconds;
	const float sigma = ai::ai_deviation_deg(slot_template(*shared, f.arms, a.weapon_slot),
								ai::trigger_skill(skill, weapon, true, false), on, 1.0f) *
			core::kDegToRadF;
	const AimError err = targeting.aim_error(f.tick, sigma, rng);
	i.want_yaw = wrap(i.want_yaw + err.yaw);
	i.want_pitch += err.pitch;
}

void BotBody::vehicle_trigger(const BotFrame &p_frame, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	BotIntent &i = r_intent;
	const VehicleAim &a = vehicles.enemy();
	if (!a.held()) {
		return;
	}
	i.want_slot = a.weapon_slot;
	if (a.type != ai::WeaponType::C4) {
		i.trigger = a.visible ? Trigger::Launcher : Trigger::None;
		i.zoom = true;
		return;
	}
	vehicles.explosive_step(*shared, f.arms, slot, f.tick);
	if (vehicles.phase() == ExplosivePhase::Place) {
		i.trigger = Trigger::Place;
	} else if (vehicles.phase() == ExplosivePhase::Detonate) {
		i.pose = sim::Pose::Crouch;
		if (vehicles.detonate_due(f.tick)) {
			i.trigger = Trigger::Detonate;
		}
	}
}

void BotBody::vehicle_move(const BotFrame &p_frame, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	const VehicleAim &a = vehicles.enemy();
	if (!a.held() || a.type != ai::WeaponType::C4) {
		return;
	}
	if (vehicles.phase() != walked_phase) {
		navigation.clear_goal();
		walked_phase = vehicles.phase();
	}
	if (vehicles.phase() == ExplosivePhase::Approach) {
		const sw::WeaponSimDef *d = f.arms.def(a.weapon_slot);
		const float gap = d != nullptr ? 0.5f * d->projectile.explosion.inner_radius : 0.0f;
		const sim::Vec3 goal = beside_hull_point(a, f.eye, f.self.position, gap);
		const float hd = horizontal(f.self.position, goal);
		if (hd <= ai::kPlacedBackOffMetres &&
				!f.world.ray_any(f.eye, sub(add(goal, 0.0f, f.eye[1] - f.self.position[1], 0.0f), f.eye),
						sim::query_mask::kMovement)) {
			navigation.clear_goal();
			r_intent.move_dx = (goal[0] - f.self.position[0]) / std::max(hd, 1e-3f);
			r_intent.move_dz = (goal[2] - f.self.position[2]) / std::max(hd, 1e-3f);
		} else if (navigation.set_goal(*shared, f.tick, goal, 0.5f, true, kSquadGoalMoveMetres)) {
			walk(f, r_intent);
		} else {
			navigation.clear_goal();
		}
	} else if (vehicles.phase() == ExplosivePhase::BackOff) {
		for (const float turn : { 0.0f, 0.8f, -0.8f, 1.6f, -1.6f }) {
			if (navigation.set_goal(*shared, f.tick, vehicles.back_off_goal(f.self.position, turn), 2.0f, true,
						kSquadGoalMoveMetres)) {
				walk_facing(f, r_intent);
				return;
			}
		}
		navigation.clear_goal();
	}
}

void BotBody::repair_move(const BotFrame &p_frame, BotIntent &r_intent) {
	const BotFrame &f = p_frame;
	if (!vehicles.track(*shared, f.eye, true) || shared->host.gunplay == nullptr) {
		return;
	}
	const VehicleAim &a = vehicles.repair();
	const float reach = weapon_range(*shared, f.arms, a.weapon_slot);
	if (a.surface <= ai::assist_reach(reach, 0.0f)) {
		return;
	}
	navigation.set_goal(*shared, f.tick, beside_hull_point(a, f.eye, f.self.position, 0.25f * reach), 0.5f, true,
			kSquadGoalMoveMetres);
	walk_facing(f, r_intent);
}

void BotBody::repair_aim(const BotFrame &p_frame, BotIntent &r_intent) {
	const VehicleAim &a = vehicles.repair();
	if (a.held() && shared->host.gunplay != nullptr &&
			a.surface <= weapon_range(*shared, p_frame.arms, a.weapon_slot)) {
		r_intent.target_distance = r_intent.aim_at(p_frame.eye, a.near_point);
		r_intent.sprint = false;
	}
}

void BotBody::repair_trigger(const BotFrame &p_frame, BotIntent &r_intent) {
	const VehicleAim &a = vehicles.repair();
	if (!a.held() || shared->host.gunplay == nullptr) {
		return;
	}
	r_intent.want_slot = a.weapon_slot;
	if (a.surface <= weapon_range(*shared, p_frame.arms, a.weapon_slot)) {
		r_intent.trigger = Trigger::Repair;
	}
}
}

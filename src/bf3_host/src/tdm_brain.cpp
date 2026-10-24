#include "bf3/host/tdm_brain.h"

#include "bf3/core/util/finite.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim_def.h"
#include "tdm_bot_awareness.h"
#include "tdm_bot_common.h"
#include "tdm_bot_movement.h"
#include "tdm_bot_objectives.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_targeting.h"
#include "tdm_bot_weapons.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace bf3::host {

using namespace tdm_bot;

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

struct BotFrame {
	sim::Tick tick;
	const sim::SoldierState &self;
	std::span<const sim::SoldierState> states;
	const sim::CollisionWorld &world;
	sim::Vec3 eye;
	const sim::bots::PerceivedEnemy *te;
	bool visible;
	int target;
	bool shot_recent;
	const BotArms &arms;
	const DownedSoldier *revive;
};

struct BotIntent {
	float move_dx = 0.0f, move_dz = 0.0f;
	bool sprint = false;
	sim::Pose pose = sim::Pose::Stand;
	float want_yaw = 0.0f, want_pitch = 0.0f;
	bool have_aim = false;
	bool zoom = false;
	float target_distance = 0.0f;
	uint8_t want_slot = kSlotPrimary;
	Trigger trigger = Trigger::None;
	float walk_dx = 0.0f, walk_dz = 0.0f, walk_yaw = 0.0f;
	bool walk_sprint = false;
	bool walk_jump = false;
	sim::Pose walk_pose = sim::Pose::Stand;

	float aim_at(const sim::Vec3 &p_eye, const sim::Vec3 &p_point) {
		const float dx = p_point[0] - p_eye[0], dy = p_point[1] - p_eye[1], dz = p_point[2] - p_eye[2];
		const float h = std::sqrt(dx * dx + dz * dz);
		want_yaw = yaw_of(dx, dz);
		want_pitch = std::atan2(dy, std::max(h, 1e-3f));
		have_aim = true;
		return std::sqrt(h * h + dy * dy);
	}
};

}

const char *tdm_bot_activity_name(TdmBotActivity p_activity) {
	switch (p_activity) {
		case TdmBotActivity::Dead:
			return "dead";
		case TdmBotActivity::Restricted:
			return "restricted";
		case TdmBotActivity::Roam:
			return "roam";
		case TdmBotActivity::Hunt:
			return "hunt";
		case TdmBotActivity::Fight:
			return "fight";
		case TdmBotActivity::Retreat:
			return "retreat";
		case TdmBotActivity::RegenWait:
			return "regen_wait";
		case TdmBotActivity::Revive:
			return "revive";
		case TdmBotActivity::Grenade:
			return "grenade";
	}
	return "?";
}

struct TdmBrain::Impl {
	std::shared_ptr<TdmShared> shared;
	uint32_t index = 0;
	uint8_t slot = 0xFF;
	uint64_t seed = 0;
	uint64_t rng = 0;
	BotAwareness awareness;
	BotNavigation navigation;

	uint32_t life = UINT32_MAX;
	sim::Tick life_tick = 0;
	uint8_t kit = 0;
	float skill = 0.5f, skill01 = 0.5f;
	float view_yaw = 0.0f, view_pitch = 0.0f;
	TdmBotActivity activity = TdmBotActivity::Dead;

	BotTargeting targeting;
	BotHunt hunt;

	BotWeapons weapons;

	BotStrafe strafe;

	BotLook look;

	BotRetreat retreat;

	BotRevive medic;

	BotGrenade grenade;

	sim::Tick knife_until = 0;

	const TdmBotConfig &cfg() const { return *shared->config; }

	void new_life(sim::Tick p_tick, const sim::SoldierState &p_state) {
		const TdmBotConfig &c = cfg();
		life = p_state.life;
		life_tick = p_tick;
		rng = mix(seed ^ (uint64_t(life) << 8));
		const float lo = std::min(c.skill_min, c.skill_max), hi = std::max(c.skill_min, c.skill_max);
		skill01 = unit(rng);
		skill = std::clamp(lo + (hi - lo) * skill01, 0.0f, 1.0f);
		const ServerView &view = *shared->host.view;
		kit = shared->host.rules ? view.kit(slot) : (shared->host.gunplay != nullptr ? shared->host.gunplay->kit(slot) : uint8_t(0));
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

	void set_activity(sim::Tick p_tick, TdmBotActivity p_activity) {
		activity = p_activity;
		TdmShared &sh = *shared;
		if (sh.stats != nullptr) {
			sh.stats->activity_ticks[size_t(p_activity)].fetch_add(1, std::memory_order_relaxed);
		}
		if (sh.observer != nullptr) {
			sh.observer->activity(p_tick, slot, p_activity);
		}
	}

	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) {
		TdmShared &sh = *shared;
		const uint64_t t0 = now_ns();
		sh.begin_tick(p_tick);
		sim::Command c = run(p_tick, p_state);
		sh.brains_ns += now_ns() - t0;
		if (sh.stats != nullptr) {
			sh.stats->last_perception_ns.store(sh.perception_ns, std::memory_order_relaxed);
			sh.stats->last_brains_ns.store(sh.brains_ns, std::memory_order_relaxed);
		}
		if (sh.observer != nullptr) {
			sh.observer->commanded(p_tick, slot, activity, c);
		}
		return c;
	}

	sim::Command run(sim::Tick p_tick, const sim::SoldierState &p_state) {
		TdmShared &sh = *shared;
		const TdmBotConfig &c = cfg();
		sim::Command cmd = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		cmd.flags = 0;
		if (slot == 0xFF || !is_alive(p_state)) {
			targeting.drop();
			set_activity(p_tick, TdmBotActivity::Dead);
			return cmd;
		}
		if (p_state.life != life) {
			new_life(p_tick, p_state);
		}
		const ServerView &view = *sh.host.view;
		if (view.input_restricted() || view.phase() != net::MatchPhase::InRound) {

			set_activity(p_tick, TdmBotActivity::Restricted);
			cmd.yaw = sim::quantize_yaw(view_yaw);
			cmd.pitch = sim::quantize_pitch(view_pitch);
			weapons.hold_fire();
			return cmd;
		}
		const std::span<const sim::SoldierState> states = view.states();
		const sim::Vec3 eye = sh.eye_point(p_state);
		awareness.check_shots(sh, slot, p_tick, p_state);
		if (sim::bots::perception_tick(slot, p_tick, c.perception.update_ticks)) {
			awareness.perceive(sh, slot, seed ^ uint64_t(life), p_tick, p_state, states, view_yaw, view_pitch);
			targeting.choose(sh, awareness, p_tick, p_state, states, skill01, rng);
		}

		const Sighting sighting = targeting.track(sh, awareness.perception(), p_tick, states, rng);
		const sim::bots::PerceivedEnemy *te = sighting.enemy;
		const bool visible = sighting.visible;
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
		check.seen = visible && te != nullptr ? &states[size_t(target)] : nullptr;
		const DownedSoldier *revive = medic.update(sh, navigation, check);

		const BotFrame frame{ p_tick, p_state, states, view.world(), eye, te, visible, target, shot_recent, arms, revive };
		const TdmBotActivity act = choose_activity(frame);
		set_activity(p_tick, act);

		BotIntent intent = plan(frame, act);
		const bool aligned = turn(frame, intent);
		compose(frame, act, intent, aligned, cmd);
		return cmd;
	}

	bool turn(const BotFrame &p_frame, BotIntent &r_intent) {
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
		const bool aligned = i.have_aim && std::fabs(wrap(i.want_yaw - view_yaw)) <= cone && std::fabs(i.want_pitch - view_pitch) <= cone;
		if (i.sprint) {
			const float len = std::sqrt(i.move_dx * i.move_dx + i.move_dz * i.move_dz);
			i.sprint = !i.have_aim && len > 0.5f && std::fabs(wrap(yaw_of(i.move_dx, i.move_dz) - view_yaw)) <= kSprintConeDeg * core::kDegToRadF;
		}
		return aligned;
	}

	void compose(const BotFrame &p_frame, TdmBotActivity p_activity, const BotIntent &p_intent, bool p_aligned, sim::Command &r_cmd) {
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
		request.react_at = targeting.react_at();
		request.target_distance = i.target_distance;
		request.target = f.target;
		request.last_visible = f.te != nullptr ? f.te->last_visible : 0;
		request.skill = skill;
		request.position = f.self.position;
		request.eye = f.eye;
		request.view_yaw = view_yaw;
		request.view_pitch = view_pitch;
		const WeaponCommand weapon = weapons.input(*shared, slot, f.arms, request, medic.defib_next(), rng);
		const sw::WeaponInput &in = weapon.input;
		const float comp_yaw = weapon.comp_yaw, comp_pitch = weapon.comp_pitch;

		sim::Command &cmd = r_cmd;
		cmd.yaw = sim::quantize_yaw(wrap(view_yaw - comp_yaw));
		cmd.pitch = sim::quantize_pitch(std::clamp(view_pitch - comp_pitch, -kMaxPitchDeg * core::kDegToRadF, kMaxPitchDeg * core::kDegToRadF));
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

	TdmBotActivity choose_activity(const BotFrame &p_frame) {
		TdmShared &sh = *shared;
		const TdmBotConfig &c = cfg();
		const BotFrame &f = p_frame;
		if (activity == TdmBotActivity::Retreat || activity == TdmBotActivity::RegenWait) {
			if (retreat.recovered(c, activity == TdmBotActivity::RegenWait, f.tick, f.self.health)) {
				activity = TdmBotActivity::Roam;
			}
		}
		const bool low = f.self.health < c.retreat_below_hp;
		TdmBotActivity act = TdmBotActivity::Roam;
		if (f.revive != nullptr) {
			act = TdmBotActivity::Revive;
		} else if (grenade.active()) {
			act = TdmBotActivity::Grenade;
		} else if (activity == TdmBotActivity::Retreat && !f.visible && retreat.cleared(f.tick)) {
			act = TdmBotActivity::RegenWait;
			retreat.begin_wait(f.tick);
		} else if (activity == TdmBotActivity::Retreat && retreat.running(f.tick) && low) {
			act = TdmBotActivity::Retreat;
		} else if (activity == TdmBotActivity::RegenWait && !f.visible && !f.shot_recent) {
			act = TdmBotActivity::RegenWait;
		} else if (low && (f.visible || f.shot_recent) && sh.nav != nullptr && activity != TdmBotActivity::Retreat && activity != TdmBotActivity::RegenWait &&
				retreat.start(sh, navigation, f.tick, f.self, f.visible ? f.states[size_t(f.target)].position : awareness.shot_from(), rng)) {
			act = TdmBotActivity::Retreat;
		} else if (f.visible) {
			act = TdmBotActivity::Fight;
		} else if (grenade.start(sh, targeting, awareness.perception(), f.tick, f.self, f.states, f.arms.rounds(kSlotGrenade) > 0)) {
			act = TdmBotActivity::Grenade;
		} else if (targeting.blind_until() > f.tick) {
			act = TdmBotActivity::Fight;
		} else {
			act = hunt.wanted(f.tick, awareness.perception(), f.self) ? TdmBotActivity::Hunt : TdmBotActivity::Roam;
		}
		if (act == TdmBotActivity::Retreat && f.visible) {
			retreat.seen(f.tick);
		}
		if (act == TdmBotActivity::Hunt && activity != TdmBotActivity::Hunt) {
			hunt.begin(f.tick);
		}
		return act;
	}

	BotIntent plan(const BotFrame &p_frame, TdmBotActivity p_activity) {
		TdmShared &sh = *shared;
		const BotFrame &f = p_frame;
		BotIntent i;
		i.want_yaw = view_yaw;
		i.want_slot = weapons.preferred_slot();
		i.walk_yaw = view_yaw;
		if (p_activity != TdmBotActivity::Fight) {
			strafe.stop();
		}
		switch (p_activity) {
			case TdmBotActivity::Fight:
				fight(f, i);
				break;
			case TdmBotActivity::Revive:
				approach_revive(f, i);
				break;
			case TdmBotActivity::Retreat:
				walk(f, i);
				i.want_yaw = i.walk_yaw;
				i.sprint = i.walk_sprint;
				break;
			case TdmBotActivity::RegenWait:
				i.pose = sim::Pose::Crouch;
				i.want_yaw = yaw_of(retreat.threat()[0] - f.self.position[0], retreat.threat()[2] - f.self.position[2]);
				break;
			case TdmBotActivity::Grenade:
				grenade.step(f.arms, f.tick, f.eye, i.want_slot, i.trigger, i.want_yaw, i.want_pitch, i.have_aim);
				break;
			case TdmBotActivity::Hunt: {
				hunt.step(sh, navigation, awareness.perception(), f.tick, f.self);
				walk(f, i);
				i.want_yaw = i.walk_yaw;
				i.sprint = i.walk_sprint;
				break;
			}
			case TdmBotActivity::Roam:
			default: {
				navigation.pick_roam_goal(sh, f.tick, f.self.position, rng);
				walk(f, i);
				i.want_yaw = i.walk_yaw;
				i.sprint = i.walk_sprint;
				break;
			}
		}

		if (!i.have_aim && f.shot_recent && p_activity != TdmBotActivity::Retreat && p_activity != TdmBotActivity::Revive) {
			i.want_yaw = yaw_of(awareness.shot_from()[0] - f.self.position[0], awareness.shot_from()[2] - f.self.position[2]);
			i.sprint = false;
		}
		return i;
	}

	bool walk(const BotFrame &p_frame, BotIntent &r_intent) {
		BotIntent &i = r_intent;
		if (navigation.walk(*shared, p_frame.tick, p_frame.self, i.walk_dx, i.walk_dz, i.walk_yaw, i.walk_sprint, i.walk_pose, i.walk_jump)) {
			i.move_dx = i.walk_dx;
			i.move_dz = i.walk_dz;
			i.pose = i.walk_pose;
			return true;
		}
		i.walk_yaw = look.look(p_frame.tick, awareness.perception(), view_yaw, rng);
		return false;
	}

	void fight(const BotFrame &p_frame, BotIntent &r_intent) {
		TdmShared &sh = *shared;
		const TdmBotConfig &c = cfg();
		const BotFrame &f = p_frame;
		BotIntent &i = r_intent;
		if (!f.visible) {
			i.target_distance = i.aim_at(f.eye, targeting.blind_at());
			i.trigger = Trigger::Blind;
			return;
		}
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
		const float vx = ts.velocity[0] - along * los[0] / l, vy = ts.velocity[1] - along * los[1] / l, vz = ts.velocity[2] - along * los[2] / l;
		const float crossing = std::sqrt(vx * vx + vy * vy + vz * vz);
		const float own = std::sqrt(f.self.velocity[0] * f.self.velocity[0] + f.self.velocity[2] * f.self.velocity[2]);
		const float on = float(f.tick - targeting.target_since()) * sim::kTickSeconds;
		const float sigma = sim::bots::aim_error(c.aim, skill, on, own > 0.5f, std::isfinite(crossing) ? crossing : 0.0f) * core::kDegToRadF;
		const AimError err = targeting.aim_error(f.tick, sigma, rng);
		i.want_yaw = wrap(i.want_yaw + err.yaw);
		i.want_pitch += err.pitch;
		i.trigger = Trigger::Aimed;

		const float hd = horizontal(f.self.position, ts.position);
		if (hd <= c.knife_within_m && f.arms.def(kSlotKnife) != nullptr) {
			const float behind = std::fabs(wrap(yaw_of(f.self.position[0] - ts.position[0], f.self.position[2] - ts.position[2]) - sim::yaw_radians(ts.yaw)));
			if (behind >= kKnifeBehindDeg * core::kDegToRadF && knife_until <= f.tick) {
				knife_until = f.tick + kKnifeTicks;
				sh.count(&TdmBotStats::knife_attacks, uint64_t(1));
			}
		}
		if (knife_until > f.tick) {
			i.want_slot = kSlotKnife;
			i.trigger = Trigger::Knife;
			i.aim_at(f.eye, chest);
		}
		i.zoom = i.target_distance > kZoomBeyondMetres && i.trigger == Trigger::Aimed;

		if (i.target_distance < c.engage_strafe_m) {
			strafe.step(c, slot, f.tick, f.world, f.self, view_yaw, rng, i.move_dx, i.move_dz);
		} else if (i.target_distance > c.crouch_beyond_m && skill > 0.5f && f.tick - targeting.target_since() < kHoldTicks) {
			strafe.stop();
			i.pose = sim::Pose::Crouch;
		} else if (navigation.set_goal(sh, f.tick, ts.position, kHuntArriveMetres, false) && walk(f, i)) {
			strafe.stop();
			i.pose = sim::Pose::Stand;
		} else {
			strafe.step(c, slot, f.tick, f.world, f.self, view_yaw, rng, i.move_dx, i.move_dz);
		}
	}

	void approach_revive(const BotFrame &p_frame, BotIntent &r_intent) {
		const BotFrame &f = p_frame;
		BotIntent &i = r_intent;
		const sim::Vec3 &corpse = f.revive->corpse;
		const sim::Vec3 centre = add(corpse, 0.0f, kReviveCentreUp, 0.0f);
		const float hd = horizontal(f.self.position, corpse);
		if (hd > kReviveDirectMetres) {
			navigation.set_goal(*shared, f.tick, corpse, 1.0f, true);
			walk(f, i);
			i.want_yaw = i.walk_yaw;
			return;
		}
		if (hd > kReviveStandMetres) {
			const float inv = 0.6f / std::max(hd, 1e-3f);
			i.move_dx = (corpse[0] - f.self.position[0]) * inv;
			i.move_dz = (corpse[2] - f.self.position[2]) * inv;
		}
		i.pose = hd < kReviveCrouchMetres ? sim::Pose::Crouch : sim::Pose::Stand;
		i.want_slot = kKitGadgetSlot;
		const float reach = i.aim_at(f.eye, centre);
		if (reach <= kDefibReach) {
			i.trigger = Trigger::Defib;
		}
	}
};

TdmBrain::TdmBrain(std::shared_ptr<TdmShared> p_shared, uint32_t p_index) :
		impl_(std::make_unique<Impl>()) {
	impl_->shared = std::move(p_shared);
	impl_->index = p_index;
	if (impl_->shared != nullptr) {
		impl_->awareness.configure(impl_->shared->config->perception);
	}
}

TdmBrain::~TdmBrain() = default;

sim::Command TdmBrain::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	if (impl_->shared == nullptr) {
		return sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
	}
	return impl_->command(p_tick, p_state);
}

void TdmBrain::set_slot(uint8_t p_slot) {
	Impl &m = *impl_;
	m.slot = p_slot;
	m.seed = (m.shared != nullptr ? m.shared->host.match_seed : 0) ^ uint64_t(p_slot);
	m.navigation.set_slot(p_slot, m.seed);
	m.life = UINT32_MAX;
}

namespace {

using Key = core::KeyDef<TdmBotConfig>;
using core::KeyFault;
using core::KeyKind;
using core::KeyOutcome;

bool parse_number(std::string_view p_text, double &r_value) {
	if (p_text.empty() || p_text.size() > 32) {
		return false;
	}
	char buf[40];
	std::memcpy(buf, p_text.data(), p_text.size());
	buf[p_text.size()] = '\0';
	char *end = nullptr;
	const double v = std::strtod(buf, &end);
	if (end == buf || *end != '\0' || !std::isfinite(v)) {
		return false;
	}
	r_value = v;
	return true;
}

KeyOutcome bot_number(const Key &p_key, std::string_view p_value, double &r_value) {
	if (!parse_number(p_value, r_value)) {
		return { KeyFault::NotANumber };
	}
	if (r_value < p_key.min || r_value > p_key.max) {
		return { KeyFault::OutOfRange, r_value };
	}
	return {};
}

KeyOutcome bot_float(const Key &p_key, std::string_view p_value, TdmBotConfig &r_config) {
	double v = 0.0;
	const KeyOutcome outcome = bot_number(p_key, p_value, v);
	if (outcome.fault == KeyFault::None) {
		*static_cast<float *>(p_key.field(r_config)) = float(v);
	}
	return outcome;
}

KeyOutcome bot_count(const Key &p_key, std::string_view p_value, TdmBotConfig &r_config) {
	double v = 0.0;
	const KeyOutcome outcome = bot_number(p_key, p_value, v);
	if (outcome.fault == KeyFault::None) {
		*static_cast<uint32_t *>(p_key.field(r_config)) = uint32_t(v);
	}
	return outcome;
}

#define BF3_BOT_FIELD(f) [](TdmBotConfig &c) -> void * { return &c.f; }

const Key kKeys[] = {
	{ "skill_min", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(skill_min), {}, bot_float },
	{ "skill_max", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(skill_max), {}, bot_float },
	{ "reaction_min_s", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(reaction_min_s), {}, bot_float },
	{ "reaction_max_s", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(reaction_max_s), {}, bot_float },
	{ "switch_margin", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(switch_margin), {}, bot_float },
	{ "strafe_min_m", KeyKind::Custom, 0.0f, 50.0f, BF3_BOT_FIELD(strafe_min_m), {}, bot_float },
	{ "strafe_max_m", KeyKind::Custom, 0.0f, 50.0f, BF3_BOT_FIELD(strafe_max_m), {}, bot_float },
	{ "crouch_beyond_m", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(crouch_beyond_m), {}, bot_float },
	{ "retreat_below_hp", KeyKind::Custom, 0.0f, 100.0f, BF3_BOT_FIELD(retreat_below_hp), {}, bot_float },
	{ "grenade_min_m", KeyKind::Custom, 0.0f, 200.0f, BF3_BOT_FIELD(grenade_min_m), {}, bot_float },
	{ "grenade_max_m", KeyKind::Custom, 0.0f, 200.0f, BF3_BOT_FIELD(grenade_max_m), {}, bot_float },
	{ "grenade_every_s", KeyKind::Custom, 1.0f, 3600.0f, BF3_BOT_FIELD(grenade_every_s), {}, bot_float },
	{ "knife_within_m", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(knife_within_m), {}, bot_float },
	{ "heat_cell_m", KeyKind::Custom, 1.0f, 1000.0f, BF3_BOT_FIELD(heat_cell_m), {}, bot_float },
	{ "heat_decay_s", KeyKind::Custom, 0.1f, 3600.0f, BF3_BOT_FIELD(heat_decay_s), {}, bot_float },
	{ "revive_radius_m", KeyKind::Custom, 0.0f, 500.0f, BF3_BOT_FIELD(revive_radius_m), {}, bot_float },
	{ "revive_idle_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(revive_idle_s), {}, bot_float },
	{ "revive_margin_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(revive_margin_s), {}, bot_float },
	{ "w_distance", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_distance), {}, bot_float },
	{ "w_aim", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_aim), {}, bot_float },
	{ "w_damage", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_damage), {}, bot_float },
	{ "w_current", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_current), {}, bot_float },
	{ "damage_memory_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(damage_memory_s), {}, bot_float },
	{ "aims_at_me_deg", KeyKind::Custom, 0.0f, 90.0f, BF3_BOT_FIELD(aims_at_me_deg), {}, bot_float },
	{ "engage_strafe_m", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(engage_strafe_m), {}, bot_float },
	{ "hunt_share", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(hunt_share), {}, bot_float },
	{ "blind_fire_share", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(blind_fire_share), {}, bot_float },
	{ "regen_wait_s", KeyKind::Custom, 0.0f, 600.0f, BF3_BOT_FIELD(regen_wait_s), {}, bot_float },
	{ "regen_until_hp", KeyKind::Custom, 0.0f, 100.0f, BF3_BOT_FIELD(regen_until_hp), {}, bot_float },
	{ "perception.fov_h_deg", KeyKind::Custom, 1.0f, 360.0f, BF3_BOT_FIELD(perception.fov_h_deg), {}, bot_float },
	{ "perception.fov_v_deg", KeyKind::Custom, 1.0f, 180.0f, BF3_BOT_FIELD(perception.fov_v_deg), {}, bot_float },
	{ "perception.max_range_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.max_range_m), {}, bot_float },
	{ "perception.near_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.near_m), {}, bot_float },
	{ "perception.detect_near_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(perception.detect_near_s), {},
			bot_float },
	{ "perception.far_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.far_m), {}, bot_float },
	{ "perception.detect_far_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(perception.detect_far_s), {}, bot_float },
	{ "perception.forget_s", KeyKind::Custom, 0.0f, 600.0f, BF3_BOT_FIELD(perception.forget_s), {}, bot_float },
	{ "perception.hearing_radius_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.hearing_radius_m), {},
			bot_float },
	{ "perception.hearing_error_m", KeyKind::Custom, 0.0f, 200.0f, BF3_BOT_FIELD(perception.hearing_error_m), {},
			bot_float },
	{ "perception.head_height_m", KeyKind::Custom, 0.0f, 3.0f, BF3_BOT_FIELD(perception.head_height_m), {}, bot_float },
	{ "perception.chest_height_m", KeyKind::Custom, 0.0f, 3.0f, BF3_BOT_FIELD(perception.chest_height_m), {},
			bot_float },
	{ "aim.e0_deg", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.e0_deg), {}, bot_float },
	{ "aim.e_inf_deg", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.e_inf_deg), {}, bot_float },
	{ "aim.tau_s", KeyKind::Custom, 0.001f, 60.0f, BF3_BOT_FIELD(aim.tau_s), {}, bot_float },
	{ "aim.moving_penalty_deg", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.moving_penalty_deg), {}, bot_float },
	{ "aim.crossing_deg_per_mps", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.crossing_deg_per_mps), {},
			bot_float },
	{ "aim.skill_scale", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(aim.skill_scale), {}, bot_float },
	{ "count", KeyKind::Custom, 0, 64, BF3_BOT_FIELD(count), {}, bot_count },
	{ "burst_min_rounds", KeyKind::Custom, 1, 100, BF3_BOT_FIELD(burst_min_rounds), {}, bot_count },
	{ "burst_max_rounds", KeyKind::Custom, 1, 100, BF3_BOT_FIELD(burst_max_rounds), {}, bot_count },
	{ "perception.update_ticks", KeyKind::Custom, 1, 600, BF3_BOT_FIELD(perception.update_ticks), {}, bot_count },
	{ "perception.max_candidates", KeyKind::Custom, 1, 64, BF3_BOT_FIELD(perception.max_candidates), {}, bot_count },
};

#undef BF3_BOT_FIELD

std::string bot_fault(const core::KeyFaultInfo &p_info) {
	const std::string where = core::key_fault_where(p_info);
	const std::string key(p_info.key);
	switch (p_info.fault) {
		case KeyFault::TooLarge:
			return std::string(p_info.what) + ": " + std::to_string(p_info.count) + " bytes, more than " +
					std::to_string(p_info.limit);
		case KeyFault::NoEquals:
			return where + ": no '='";
		case KeyFault::NotANumber:
			return where + ": '" + key + "' needs a finite number";
		case KeyFault::OutOfRange:
			return where + ": '" + key + "' = " + std::to_string(p_info.number) + " is outside [" +
					std::to_string(p_info.min) + ", " + std::to_string(p_info.max) + "]";
		case KeyFault::UnknownKey:
			return where + ": unknown key '" + key + "'";
		default:
			return core::key_fault_message(p_info);
	}
}

const core::KeyValueGrammar<TdmBotConfig> kGrammar{ "bot_tuning", kKeys, kMaxBotTuningBytes, kMaxBotTuningLines,
	SIZE_MAX, false, '\n', true, true, bot_fault };

}

core::Status parse_bot_tuning(std::string_view p_text, TdmBotConfig &r_config) {
	core::Result<TdmBotConfig> parsed = core::parse_key_values(p_text, kGrammar, r_config);
	if (!parsed) {
		return parsed.error();
	}
	r_config = std::move(*parsed);
	return core::Status::success();
}

core::Status load_bot_tuning(const std::string &p_path, TdmBotConfig &r_config) {
	std::ifstream f(p_path, std::ios::binary);
	if (!f) {
		return core::make_error("bot_tuning: cannot open " + p_path);
	}
	std::string text;
	text.resize(kMaxBotTuningBytes + 1);
	f.read(text.data(), std::streamsize(text.size()));
	text.resize(size_t(f.gcount()));
	const core::Status st = parse_bot_tuning(text, r_config);
	if (!st.ok()) {
		return core::error_at(p_path, st.error());
	}
	return core::Status::success();
}

core::Status add_tdm_bots(GameServer &r_server, const GameServerConfig &p_config) {
	if (p_config.tdm_bots == nullptr || p_config.tdm_bots->count == 0) {
		return core::Status::success();
	}
	if (p_config.level == nullptr) {
		return core::make_error("tdm bots: no level");
	}
	std::shared_ptr<const TdmBotConfig> config = p_config.tdm_bots;
	if (!config->tuning_cfg.empty()) {
		auto tuned = std::make_shared<TdmBotConfig>(*config);
		BF3_TRY(load_bot_tuning(config->tuning_cfg, *tuned));
		tuned->count = config->count;
		config = tuned;
	}
	TdmBrainHost host;
	host.view = &r_server.view();
	host.gunplay = r_server.gunplay();
	host.match_seed = r_server.gunplay() != nullptr ? r_server.gunplay()->match_seed() : p_config.seed;
	host.rules = p_config.rules.mode == MatchRulesConfig::Mode::Tdm;
	for (size_t i = 0; i < 3; ++i) {
		if (p_config.level->motor.poses[i].eye_height > 0.0f) {
			host.eye_heights[i] = p_config.level->motor.poses[i].eye_height;
		}
	}
	if (p_config.bots != nullptr && p_config.bots->level != nullptr) {
		host.nav = p_config.bots->level;
		host.walker = p_config.bots->walker;
	}

	if (!p_config.level->spawns.empty()) {
		double sx = 0.0, sz = 0.0;
		for (const sim::SpawnPoint &p : p_config.level->spawns) {
			sx += p.position[0];
			sz += p.position[2];
		}
		const float half = 0.5f * float(kHeatCells) * std::max(config->heat_cell_m, 1.0f);
		const float cx = float(sx / double(p_config.level->spawns.size())), cz = float(sz / double(p_config.level->spawns.size()));
		if (std::isfinite(cx) && std::isfinite(cz)) {
			host.heat_origin = { cx - half, 0.0f, cz - half };
		}
	}
	core::Result<std::shared_ptr<TdmShared>> made = make_tdm_shared(host, config);
	if (!made) {
		return made.error();
	}
	std::shared_ptr<TdmShared> shared = std::move(*made);

	const GameServerStats st = r_server.stats();
	const size_t limit = std::min<size_t>({ st.slots.size(), size_t(p_config.max_slots), size_t(net::kMaxPlayerRows) });
	uint32_t free_below = 0;
	for (size_t s = 0; s < limit; ++s) {
		free_below += st.slots[s].used ? 0u : 1u;
	}
	if (st.slots.empty()) {
		free_below = std::min<uint32_t>(p_config.max_slots, uint32_t(net::kMaxPlayerRows));
	}
	const uint32_t count = std::min(config->count, free_below);
	for (uint32_t i = 0; i < count; ++i) {
		auto brain = std::make_unique<TdmBrain>(shared, i);
		TdmBrain *raw = brain.get();

		const int team = host.rules ? 0 : 1 + int((i + i / 4u) % 2u);
		const int slot = r_server.add_local_bot(std::move(brain), team);
		if (slot < 0) {
			return core::make_error("tdm bots: no slot for bot " + std::to_string(i) + " of " + std::to_string(count));
		}
		raw->set_slot(uint8_t(slot));
		if (shared->stats != nullptr) {
			shared->stats->bots.fetch_add(1, std::memory_order_relaxed);
		}
	}
	return core::Status::success();
}

}

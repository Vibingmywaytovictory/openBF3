#include "bf3/host/ai_bot_source.h"

#include "bf3/host/server_view.h"
#include "bf3/sim/command.h"
#include "tdm_bot_body.h"
#include "tdm_bot_common.h"
#include "tdm_bot_shared.h"
#include "tdm_strategy.h"

#include <optional>

namespace bf3::host {

using namespace tdm_bot;

TdmBotActivity tdm_activity(ai::InfantryActivity p_activity) {
	switch (p_activity) {
		case ai::InfantryActivity::Idle:
		case ai::InfantryActivity::Roam:
			return TdmBotActivity::Roam;
		case ai::InfantryActivity::Hunt:
			return TdmBotActivity::Hunt;
		case ai::InfantryActivity::Fight:
			return TdmBotActivity::Fight;
		case ai::InfantryActivity::Retreat:
			return TdmBotActivity::Retreat;
		case ai::InfantryActivity::RegenWait:
			return TdmBotActivity::RegenWait;
		case ai::InfantryActivity::Revive:
			return TdmBotActivity::Revive;
		case ai::InfantryActivity::Grenade:
			return TdmBotActivity::Grenade;
	}
	return TdmBotActivity::Roam;
}

struct AiBotSource::Impl final : ai::InfantryBody {
	BotBody body;
	std::unique_ptr<ai::BotMain> bot;
	sim::Tick now = 0;
	const sim::SoldierState *state = nullptr;
	Sighting sighting;
	std::optional<BotFrame> frame;
	ai::InfantrySenses sensed;
	BotIntent intent;
	TdmBotActivity planned = TdmBotActivity::Roam;
	sim::Command cmd{};

	const ServerView &view() const { return *body.shared->host.view; }
	const BotFrame &f() const { return *frame; }

	sim::Tick tick() const override { return now; }
	ai::Vec3 position() const override { return state->position; }

	void see() override { sighting = body.see(now, *state, view().states()); }

	void update_sensing() override {
		const TdmShared &sh = *body.shared;
		frame.emplace(body.sense_revive(now, *state, view(), sighting));
		const BotFrame &fr = *frame;
		sensed.enemy_visible = fr.visible;
		sensed.blind_fire = body.targeting.blind_until() > now;
		sensed.shot_at = fr.shot_recent;
		sensed.low_health = fr.self.health < body.cfg().retreat_below_hp;
		sensed.revive_target = fr.revive != nullptr;
		sensed.grenade_running = body.grenade.active();
		sensed.hunt_target = body.hunt.wanted(now, body.awareness.perception(), fr.self);
		sensed.hunt_near = sensed.hunt_target &&
				body.hunt.enemy_within(body.awareness.perception(), fr.self, body.cfg().behaviours.hunt_near_m);
		sensed.navigable = sh.nav != nullptr;
	}

	const ai::InfantrySenses &senses() const override { return sensed; }

	bool can_throw_grenade() const override {
		const BotFrame &fr = f();
		return body.grenade.can_start(*body.shared, body.targeting, body.awareness.perception(), now, fr.self,
				fr.states, fr.arms.rounds(kSlotGrenade) > 0);
	}

	bool throw_grenade() override {
		const BotFrame &fr = f();
		return body.grenade.start(*body.shared, body.targeting, body.awareness.perception(), now, fr.self, fr.states,
				fr.arms.rounds(kSlotGrenade) > 0);
	}

	bool take_cover() override {
		const BotFrame &fr = f();
		const sim::Vec3 threat = fr.visible ? fr.states[size_t(fr.target)].position : body.awareness.shot_from();
		return body.retreat.start(*body.shared, body.navigation, now, fr.self, threat, body.rng);
	}

	void wait_in_cover() override { body.retreat.begin_wait(now); }
	bool cover_recovered(bool p_waiting) const override {
		return body.retreat.recovered(body.cfg(), p_waiting, now, state->health);
	}
	bool cover_cleared() const override { return body.retreat.cleared(now); }
	bool cover_running() const override { return body.retreat.running(now); }

	void begin_plan(ai::InfantryActivity p_activity) override {
		planned = tdm_activity(p_activity);
		if (planned == TdmBotActivity::Retreat && f().visible) {
			body.retreat.seen(now);
		}
		if (planned == TdmBotActivity::Hunt && body.activity != TdmBotActivity::Hunt) {
			body.hunt.begin(now);
		}
		body.set_activity(now, planned);
		intent = body.begin_plan(planned);
	}

	bool move_to(ai::MoveGoal p_goal, const ai::Vec3 &p_point) override {
		switch (p_goal) {
			case ai::MoveGoal::Roam:
				body.roam(f(), intent);
				break;
			case ai::MoveGoal::Hunt:
				body.hunt_step(f(), intent);
				break;
			case ai::MoveGoal::Cover:
				body.walk_facing(f(), intent);
				break;
			case ai::MoveGoal::Squad:
				body.navigation.set_goal(
						*body.shared, now, p_point, body.cfg().behaviours.squad_arrive_m, false, kSquadGoalMoveMetres);
				body.walk_facing(f(), intent);
				if (bot->squad_goal().kind != ai::SquadGoalKind::SquadTarget &&
						std::hypot(p_point.x - f().self.position[0], p_point.z - f().self.position[2]) >
								body.cfg().behaviours.squad_far_m) {
					intent.sprint = true;
					intent.pose = sim::Pose::Stand;
				}
				break;
		}
		return false;
	}

	bool move_to_object(ai::ObjectGoal p_object) override {
		if (p_object == ai::ObjectGoal::Target) {
			const ai::SquadGoal &goal = bot->squad_goal();
			const bool advance = bot->ordered_behaviour() == int(ai::InfantryBehaviour::MoveTo) &&
					goal.kind != ai::SquadGoalKind::None && !goal.wait_for_members &&
					std::hypot(goal.position.x - f().self.position[0], goal.position.z - f().self.position[2]) >
							body.cfg().behaviours.squad_arrive_m;
			if (advance &&
					body.navigation.set_goal(*body.shared, now, goal.position, body.cfg().behaviours.squad_arrive_m,
							false, kSquadGoalMoveMetres) &&
					body.walk(f(), intent)) {
				intent.pose = sim::Pose::Stand;
				return true;
			}
			body.move_fight(f(), intent);
			return true;
		}
		return body.revive_approach(f(), intent);
	}

	bool soldier_pose(ai::PoseRequest p_pose) override {
		if (p_pose == ai::PoseRequest::Crouch) {
			body.crouch(intent);
		} else {
			body.revive_pose(f(), intent);
		}
		return true;
	}

	bool trigger(ai::TriggerKind p_trigger) override {
		switch (p_trigger) {
			case ai::TriggerKind::Aimed:
				body.trigger_target(f(), intent);
				break;
			case ai::TriggerKind::Blind:
				body.trigger_blind(intent);
				break;
			case ai::TriggerKind::Grenade:
				body.throw_step(f(), intent);
				break;
			case ai::TriggerKind::Defib:
				body.revive_trigger(f(), intent);
				break;
		}
		return intent.trigger != Trigger::None;
	}

	bool sense(ai::SenseFocus p_focus) override {
		switch (p_focus) {
			case ai::SenseFocus::Target:
				body.aim_target(f(), intent);
				break;
			case ai::SenseFocus::BlindPoint:
				body.aim_blind(f(), intent);
				break;
			case ai::SenseFocus::Threat:
				body.face_threat(f(), intent);
				break;
			case ai::SenseFocus::ShotOrigin:
				body.face_shots(f(), planned, intent);
				break;
			case ai::SenseFocus::Around:
				body.look_around(f(), intent);
				break;
		}
		return true;
	}

	void end_plan() override {
		const bool aligned = body.turn(f(), intent);
		body.compose(f(), planned, intent, aligned, cmd);
	}

	sim::Command run(sim::Tick p_tick, const sim::SoldierState &p_state) {
		cmd = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		cmd.flags = 0;
		if (body.slot == 0xFF || !is_alive(p_state)) {
			body.targeting.drop();
			body.set_activity(p_tick, TdmBotActivity::Dead);
			bot->update_bot(false, false);
			return cmd;
		}
		const bool new_life = p_state.life != body.life;
		if (new_life) {
			body.new_life(p_tick, p_state);
		}
		bot->update_bot(true, new_life);
		bot->set_side(view().team(body.slot));
		if (view().input_restricted() || view().phase() != net::MatchPhase::InRound) {
			body.restricted(p_tick, cmd);
			return cmd;
		}
		now = p_tick;
		state = &p_state;
		const double dt = sim::kTickSeconds;
		bot->see(dt);
		bot->update_sensing(dt);
		if (bot->wants_decision()) {
			const uint64_t unplanned = bot->stats().unplanned;
			bot->decision_making(dt);
			TdmShared &sh = *body.shared;
			sh.count(&TdmBotStats::decisions, uint64_t(1));
			if (bot->stats().unplanned != unplanned) {
				sh.count(&TdmBotStats::unplanned_decisions, uint64_t(1));
			}
			if (sh.observer != nullptr) {
				sh.observer->decided(p_tick, body.slot, bot->behaviour(), bot->activity());
				sh.observer->urgencies(p_tick, body.slot, *bot);
			}
		}
		bot->plan_execution(dt);
		frame.reset();
		state = nullptr;
		return cmd;
	}
};

AiBotSource::AiBotSource(std::shared_ptr<TdmShared> p_shared, uint32_t p_index) : impl_(std::make_unique<Impl>()) {
	Impl &m = *impl_;
	m.body.attach(std::move(p_shared), p_index);
	static const ai::BehaviourTuning s_defaults;
	const ai::BehaviourTuning &tuning = m.body.shared != nullptr ? m.body.cfg().behaviours : s_defaults;
	m.bot = std::make_unique<ai::BotMain>(p_index, -1, 0, m, tuning);
	if (m.body.shared != nullptr) {
		m.body.shared->count(&TdmBotStats::bf2_bots, uint32_t(1));
	}
}

AiBotSource::~AiBotSource() {
	if (impl_->body.shared != nullptr && impl_->body.shared->strategy != nullptr) {
		impl_->body.shared->strategy->remove_bot(impl_->body.slot);
	}
}

sim::Command AiBotSource::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	Impl &m = *impl_;
	if (m.body.shared == nullptr) {
		return sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
	}
	TdmShared &sh = *m.body.shared;
	const uint64_t t0 = now_ns();
	sh.begin_tick(p_tick);
	if (sh.strategy != nullptr) {
		sh.strategy->step(p_tick);
	}
	const sim::Command c = m.run(p_tick, p_state);
	sh.brains_ns += now_ns() - t0;
	if (sh.stats != nullptr) {
		sh.stats->last_perception_ns.store(sh.perception_ns, std::memory_order_relaxed);
		sh.stats->last_brains_ns.store(sh.brains_ns, std::memory_order_relaxed);
	}
	if (sh.observer != nullptr) {
		sh.observer->commanded(p_tick, m.body.slot, m.body.activity, c);
	}
	return c;
}

void AiBotSource::admitted(uint8_t p_slot) {
	impl_->body.set_slot(p_slot);
	impl_->bot->set_player(p_slot);
	if (impl_->body.shared != nullptr && impl_->body.shared->strategy != nullptr) {
		impl_->body.shared->strategy->add_bot(p_slot, *impl_->bot);
	}
}

const ai::BotMain &AiBotSource::bot() const {
	return *impl_->bot;
}

ai::BotMain &AiBotSource::bot() {
	return *impl_->bot;
}

ai::SquadBot &AiBotSource::squad_bot() {
	return *impl_->bot;
}

}

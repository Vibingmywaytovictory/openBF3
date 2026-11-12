#include "bf3/host/ai_bot_source.h"

#include "bf3/host/ai_world.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/command.h"
#include "tdm_bot_body.h"
#include "tdm_bot_common.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_support.h"
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
		case ai::InfantryActivity::Support:
			return TdmBotActivity::Support;
		case ai::InfantryActivity::CallMedic:
			return TdmBotActivity::CallMedic;
	}
	return TdmBotActivity::Roam;
}

struct AiBotSource::Impl final : ai::InfantryBody, BotSeat {
	BotBody body;
	ai::BotMain *bot = nullptr;
	sim::Tick now = 0;
	const sim::SoldierState *state = nullptr;
	bool playing = false;
	Sighting sighting;
	std::optional<BotFrame> frame;
	ai::InfantrySenses sensed;
	BotIntent intent;
	BotIntent held;
	TdmBotActivity planned = TdmBotActivity::Roam;
	bool lost = false;
	sim::Tick plan_tick = UINT32_MAX;
	uint64_t decisions = 0;
	uint64_t unplanned = 0;

	const ServerView &view() const { return *body.shared->host.view; }
	const BotFrame &f() const { return *frame; }

	sim::Tick tick() const override { return now; }
	ai::Vec3 position() const override { return state->position; }

	void see() override { sighting = body.see(now, *state, view().states()); }

	void release() {
		held = body.begin_plan(planned);
		intent = held;
	}

	bool target_held() {
		lost = lost || f().target < 0;
		return f().target >= 0;
	}

	bool corpse_held() {
		lost = lost || f().revive == nullptr;
		return f().revive != nullptr;
	}

	SupportWay support_way() const {
		SupportWay way;
		const TdmStrategy *strategy = body.shared->strategy.get();
		if (strategy == nullptr) {
			return way;
		}
		way.point = strategy->mission(body.slot);
		if (way.point.has_value()) {
			way.free_within_m = body.cfg().behaviours.mission_arrive_m;
			return way;
		}
		way.following = bot->ordered_behaviour() == int(ai::InfantryBehaviour::MoveTo) &&
				bot->squad_goal().kind == ai::SquadGoalKind::Slot;
		if (way.following) {
			way.squad_mates = strategy->squad_mates(body.slot);
		}
		return way;
	}

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
		const SupportSense support = body.sense_support(fr,
				fr.visible || fr.shot_recent || sensed.grenade_running || sensed.revive_target || sensed.hunt_near,
				support_way());
		sensed.heal_target = support.heal;
		sensed.supply_target = support.supply;
		sensed.call_medic = support.call;
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
		body.activity = planned;
		intent = body.begin_plan(planned);
		lost = false;
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
			case ai::MoveGoal::Medkit:
				body.medkit_or_hold(f(), intent);
				break;
			case ai::MoveGoal::Squad:
				body.navigation.set_goal(
						*body.shared, now, p_point, body.cfg().behaviours.squad_arrive_m, false, kSquadGoalMoveMetres);
				body.walk_facing(f(), intent);
				if (bot->squad_goal().kind != ai::SquadGoalKind::SquadTarget &&
						std::hypot(p_point.x - f().self.position[0], p_point.z - f().self.position[2]) >
								body.cfg().behaviours.squad_far_m) {
					intent.hold_sprint();
					intent.hold_stand();
				}
				break;
		}
		return false;
	}

	bool move_to_object(ai::ObjectGoal p_object) override {
		if (p_object == ai::ObjectGoal::Target) {
			if (!target_held()) {
				return false;
			}
			const ai::SquadGoal &goal = bot->squad_goal();
			const bool advance = bot->ordered_behaviour() == int(ai::InfantryBehaviour::MoveTo) &&
					goal.kind != ai::SquadGoalKind::None && !goal.wait_for_members &&
					std::hypot(goal.position.x - f().self.position[0], goal.position.z - f().self.position[2]) >
							body.cfg().behaviours.squad_arrive_m;
			if (advance &&
					body.navigation.set_goal(*body.shared, now, goal.position, body.cfg().behaviours.squad_arrive_m,
							false, kSquadGoalMoveMetres) &&
					body.walk(f(), intent)) {
				intent.hold_stand();
				return true;
			}
			body.move_fight(f(), intent);
			return true;
		}
		if (p_object == ai::ObjectGoal::Patient || p_object == ai::ObjectGoal::SupplyMate) {
			return body.support_approach(
					f(), p_object == ai::ObjectGoal::Patient ? SupplyNeed::Health : SupplyNeed::Ammo, intent);
		}
		return corpse_held() && body.revive_approach(f(), intent);
	}

	bool soldier_pose(ai::PoseRequest p_pose) override {
		if (p_pose == ai::PoseRequest::Crouch) {
			body.crouch(intent);
		} else if (corpse_held()) {
			body.revive_pose(f(), intent);
		}
		return true;
	}

	bool trigger(ai::TriggerKind p_trigger) override {
		switch (p_trigger) {
			case ai::TriggerKind::Aimed:
				if (target_held()) {
					body.trigger_target(f(), intent);
				}
				break;
			case ai::TriggerKind::Blind:
				body.trigger_blind(intent);
				break;
			case ai::TriggerKind::Grenade:
				body.throw_step(f(), intent);
				break;
			case ai::TriggerKind::Defib:
				if (corpse_held()) {
					body.revive_trigger(f(), intent);
				}
				break;
			case ai::TriggerKind::Medkit:
				body.support_trigger(f(), SupplyNeed::Health, intent);
				break;
			case ai::TriggerKind::AmmoBag:
				body.support_trigger(f(), SupplyNeed::Ammo, intent);
				break;
			case ai::TriggerKind::CallMedic:
				body.call_medic(f());
				bot->call_for_medic();
				return true;
		}
		return intent.trigger != Trigger::None;
	}

	bool sense(ai::SenseFocus p_focus) override {
		switch (p_focus) {
			case ai::SenseFocus::Target:
				if (target_held()) {
					body.aim_target(f(), intent);
				}
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

	bool end_plan() override {
		held = intent;
		plan_tick = now;
		return !lost;
	}

	void reset_controls() override { release(); }

	BotSeatState prepare(sim::Tick p_tick, bool p_playing) override {
		frame.reset();
		state = nullptr;
		playing = p_playing;
		const std::span<const sim::SoldierState> states = view().states();
		if (body.slot >= states.size()) {
			return {};
		}
		const sim::SoldierState &s = states[body.slot];
		if (!is_alive(s)) {
			return { &s, false };
		}
		if (s.life != body.life) {
			body.new_life(p_tick, s);
			sensed = ai::InfantrySenses{};
			planned = TdmBotActivity::Roam;
			release();
		}
		bot->set_side(view().team(body.slot));
		if (!p_playing) {
			release();
			return { &s, true };
		}
		now = p_tick;
		state = &s;
		sighting = body.keep(p_tick, s, states);
		frame.emplace(body.hold_frame(p_tick, s, view(), sighting));
		return { &s, true };
	}

	void acted(sim::Tick p_tick) override {
		const ai::BotMainStats &st = bot->stats();
		if (st.decisions == decisions) {
			return;
		}
		TdmShared &sh = *body.shared;
		sh.count(&TdmBotStats::decisions, st.decisions - decisions);
		sh.count(&TdmBotStats::unplanned_decisions, st.unplanned - unplanned);
		decisions = st.decisions;
		unplanned = st.unplanned;
		if (sh.observer != nullptr) {
			sh.observer->decided(p_tick, body.slot, bot->behaviour(), bot->activity());
			sh.observer->urgencies(p_tick, body.slot, *bot);
		}
	}

	sim::Command run(sim::Tick p_tick, const sim::SoldierState &p_state) {
		sim::Command cmd = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		cmd.flags = 0;
		if (body.slot == 0xFF || bot == nullptr || !is_alive(p_state)) {
			body.targeting.drop();
			body.set_activity(p_tick, TdmBotActivity::Dead);
			return cmd;
		}
		if (!playing || !frame.has_value() || frame->tick != p_tick) {
			body.restricted(p_tick, cmd);
			return cmd;
		}
		if (bot->has_plan()) {
			body.report_activity(p_tick, tdm_activity(bot->activity()));
		}
		if (plan_tick != p_tick) {
			body.rewalk(*frame, held);
		}
		BotIntent i = held;
		if (i.trigger == Trigger::Aimed && !frame->visible) {
			i.trigger = Trigger::None;
		}
		const bool aligned = body.turn(*frame, i);
		body.compose(*frame, planned, i, aligned, cmd);
		return cmd;
	}
};

AiBotSource::AiBotSource(std::shared_ptr<TdmShared> p_shared, uint32_t p_index) : impl_(std::make_unique<Impl>()) {
	Impl &m = *impl_;
	m.body.attach(std::move(p_shared), p_index);
	if (m.body.shared != nullptr) {
		m.body.shared->count(&TdmBotStats::bf2_bots, uint32_t(1));
	}
}

AiBotSource::~AiBotSource() {
	TdmShared *const sh = impl_->body.shared.get();
	if (sh != nullptr && sh->strategy != nullptr) {
		sh->strategy->remove_bot(impl_->body.slot);
	}
	if (sh != nullptr && sh->ai != nullptr) {
		sh->ai->remove(impl_->body.slot);
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
	if (sh.ai != nullptr) {
		sh.ai->step(p_tick);
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
	Impl &m = *impl_;
	m.body.set_slot(p_slot);
	TdmShared *const sh = m.body.shared.get();
	if (sh == nullptr) {
		return;
	}
	if (sh->ai == nullptr) {
		sh->ai = std::make_shared<HostAiBots>(
				*sh->host.view, sh->config->perception.max_range_m, sh->config->behaviours.call_s,
				[sh](uint8_t p_caller) { return ask_medic(*sh, p_caller); }, sh->config->ai);
	}
	m.bot = sh->ai->admit(p_slot, m, m.body.cfg().behaviours, &m);
	if (m.bot == nullptr) {
		return;
	}
	m.bot->set_takes_squads(sh->strategy != nullptr && sh->config->squads);
	if (sh->strategy != nullptr) {
		sh->strategy->add_bot(p_slot, *m.bot);
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

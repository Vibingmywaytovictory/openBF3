#include "client_prediction.h"

#include "client_ticks.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace bf3::host {

namespace {

float length3(const sim::Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
}

sim::Vec3 sub3(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

bool states_match(const sim::SoldierState &p_a, const sim::SoldierState &p_b, float p_pos_eps, float p_vel_eps) {
	const uint16_t events_a = uint16_t(p_a.events & ~sim::soldier_events::kDamaged);
	const uint16_t events_b = uint16_t(p_b.events & ~sim::soldier_events::kDamaged);
	if (p_a.tick != p_b.tick || p_a.life != p_b.life || p_a.yaw != p_b.yaw || p_a.pitch != p_b.pitch || p_a.pose != p_b.pose ||
			p_a.target_pose != p_b.target_pose || p_a.transition_ticks != p_b.transition_ticks || p_a.transition_total != p_b.transition_total ||
			p_a.mode != p_b.mode || p_a.flags != p_b.flags || events_a != events_b || p_a.jump_penalty_ticks != p_b.jump_penalty_ticks ||
			p_a.air_ticks != p_b.air_ticks || p_a.protect_ticks != p_b.protect_ticks) {
		return false;
	}
	if (!(length3(sub3(p_a.position, p_b.position)) <= p_pos_eps) || !(length3(sub3(p_a.velocity, p_b.velocity)) <= p_vel_eps)) {
		return false;
	}
	return std::fabs(p_a.impact_speed - p_b.impact_speed) <= 1e-3f;
}

}

core::Status OwnPrediction::init(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor) {
	world_ = &p_world;
	BF3_TRY(motor_.init(p_world, p_motor));
	motor_hash_ = sim::motor_params_hash(p_motor);
	ring_ = std::make_unique<sim::PredictionRing>(config_.prediction_ring);
	return core::Status::success();
}

bool OwnPrediction::motor_matches(uint64_t p_server_motor_hash) {
	if (p_server_motor_hash != motor_hash_) {
		++motor_hash_mismatch_;
		return false;
	}
	return true;
}

void OwnPrediction::match_started(uint64_t p_now) {
	last_tick_ns_ = p_now;
}

void OwnPrediction::catch_up(const sim::SoldierState &p_base) {
	const sim::CatchUpStats c = sim::catch_up(motor_, p_base, *ring_, clock_.next_tick());
	replayed_ticks_ += c.steps;
}

void OwnPrediction::decay(uint64_t p_now) {
	const double dt_ms = last_tick_ns_ != 0 && p_now > last_tick_ns_ ? double(p_now - last_tick_ns_) / 1e6 : kMsPerTick;
	last_tick_ns_ = p_now;
	const float decay = float(std::exp(-std::min(dt_ms, 1000.0) / std::max(1e-3, double(config_.error_decay_ms))));
	for (float &v : visual_offset_) {
		v *= decay;
	}
	if (length3(visual_offset_) < config_.snap_below_m) {
		visual_offset_ = {};
	}
}

void OwnPrediction::authoritative(const sim::SoldierState &p_s, uint64_t p_now) {
	if (have_own_ && p_s.life < own_life_) {
		return;
	}
	const bool spawn = !have_own_ || p_s.life > own_life_;
	if (!spawn && have_auth_ && p_s.tick <= last_auth_.tick) {
		return;
	}

	if (p_s.tick >= clock_.next_tick()) {
		clock_.force_past(p_s.tick, p_now, input_.next_sequence());
	}
	last_auth_ = p_s;
	have_auth_ = true;
	last_server_tick_ = std::max(last_server_tick_, p_s.tick);
	if (spawn) {
		own_life_ = p_s.life;
		have_own_ = true;
		spawn_state_ = p_s;
		input_.keep_pose(p_s.target_pose);

		ring_->put({ sim::hold_command(p_s), p_s, false });
		gunplay_.weapons_spawn(p_s);
		catch_up(p_s);
		++spawns_;
		spawned_ = true;
		visual_offset_ = {};
		return;
	}
	const sim::PredictionEntry *e = ring_->find(p_s.tick);
	if (e == nullptr) {
		++ring_misses_;
		gunplay_.weapons_ring_miss(p_s);
		catch_up(p_s);
		return;
	}
	if (states_match(e->state, p_s, config_.position_epsilon_m, config_.velocity_epsilon)) {
		if (config_.debug_hash_compare && sim::soldier_state_hash(e->state) != sim::soldier_state_hash(p_s)) {
			++hash_mismatches_;
		}
		after_match(p_s);
		return;
	}
	++reconciliations_;
	last_reconcile_tick_ = p_s.tick;
	last_reconcile_ns_ = p_now;
	const sim::Tick next = clock_.next_tick();
	const sim::PredictionEntry *before = next > 0 ? ring_->find(next - 1u) : nullptr;
	const sim::Vec3 old_pos = before != nullptr ? before->state.position : p_s.position;
	gunplay_.weapons_reconcile(p_s);
	catch_up(p_s);
	const sim::PredictionEntry *after = next > 0 ? ring_->find(next - 1u) : nullptr;
	const sim::Vec3 new_pos = after != nullptr ? after->state.position : p_s.position;
	const sim::Vec3 corr = sub3(old_pos, new_pos);
	const float m = length3(corr);
	if (m > 1e-3f) {
		++corrections_over_1mm_;
	}
	max_correction_m_ = std::max(max_correction_m_, m);
	if (m > config_.teleport_above_m) {
		++teleports_;
		visual_offset_ = {};
	} else {
		for (size_t k = 0; k < 3; ++k) {
			visual_offset_[k] += corr[k];
		}
	}
}

void OwnPrediction::after_match(const sim::SoldierState &p_s) {
	if (gunplay_.reconcile_after_match(p_s)) {
		catch_up(p_s);
		return;
	}
	const sim::PredictionEntry *at = ring_->find(p_s.tick);
	if (at == nullptr || std::fabs(at->state.health - p_s.health) <= 1e-3f) {
		return;
	}
	const float delta = p_s.health - at->state.health;
	const sim::Tick next = clock_.next_tick();
	for (sim::Tick t = p_s.tick; t < next; ++t) {
		const sim::PredictionEntry *e = ring_->find(t);
		if (e == nullptr || e->state.life != p_s.life) {
			continue;
		}
		sim::PredictionEntry moved = *e;
		moved.state.health = t == p_s.tick ? p_s.health : moved.state.health + delta;
		if (t == p_s.tick) {
			moved.state.damage = p_s.damage;
			moved.state.events = uint16_t((moved.state.events & ~sim::soldier_events::kDamaged) | (p_s.events & sim::soldier_events::kDamaged));
		}
		ring_->put(moved);
	}
	++health_rebases_;
}

void OwnPrediction::fill_base(sim::Tick p_tick) {
	if (ring_->find(p_tick - 1u) != nullptr) {
		return;
	}
	const sim::PredictionEntry *b = ring_->newest_at_or_below(p_tick - 1u);
	const sim::SoldierState *base = (b != nullptr && b->state.life == own_life_) ? &b->state : nullptr;
	if (have_auth_ && last_auth_.life == own_life_ && last_auth_.tick < p_tick && (base == nullptr || last_auth_.tick > base->tick)) {
		base = &last_auth_;
	}
	if (base != nullptr) {
		const sim::SoldierState copy = *base;
		catch_up(copy);
		++base_fills_;
	}
}

bool OwnPrediction::step(const sim::PredictionEntry *p_base, const sim::Command &p_command) {
	if (p_base == nullptr || p_command.tick != p_base->state.tick + 1u) {
		++step_precondition_failures_;
		assert(false && "ClientSession: prediction base missing");
		return false;
	}
	sim::SoldierState state = p_base->state;
	motor_.step(state, p_command);
	gunplay_.predict(p_command, p_base->state, state, *world_);
	events_acc_ |= state.events;
	ring_->put({ p_command, state, true });
	return true;
}

void OwnPrediction::publish(PresentationFrame &r_frame, const sim::PredictionEntry *p_made, const sim::Command &p_command, uint64_t p_now) {
	PresentationFrame &f = r_frame;
	const sim::PredictionEntry *prev = p_made != nullptr && p_made->state.tick > 0 ? ring_->find(p_made->state.tick - 1u) : nullptr;
	f.valid = p_made != nullptr && prev != nullptr;
	if (f.valid) {
		f.tick = p_made->state.tick;
		f.tick_fraction = std::clamp(clock_.ticks(p_now) - double(f.tick), 0.0, 1.0);
		f.own_prev = prev->state;
		f.own_curr = p_made->state;
		f.own_command = p_command;
		f.eye_height_prev = motor_.eye_height(prev->state);
		f.eye_height_curr = motor_.eye_height(p_made->state);
	}
	f.visual_offset = visual_offset_;
	f.server_own_valid = have_auth_;
	f.server_own = last_auth_;
	f.server_events = server_events_acc_;
	server_events_acc_ = 0;
	f.events = events_acc_;
	events_acc_ = 0;
}

void OwnPrediction::stats(ClientStats &r_stats) const {
	r_stats.spawned = spawned_;
	r_stats.motor_hash_mismatch = motor_hash_mismatch_;
	r_stats.last_server_tick = last_server_tick_;
	r_stats.reconciliations = reconciliations_;
	r_stats.last_reconcile_tick = last_reconcile_tick_;
	r_stats.last_reconcile_ns = last_reconcile_ns_;
	r_stats.ring_misses = ring_misses_;
	r_stats.spawns = spawns_;
	r_stats.replayed_ticks = replayed_ticks_;
	r_stats.base_fills = base_fills_;
	r_stats.step_precondition_failures = step_precondition_failures_;
	r_stats.corrections_over_1mm = corrections_over_1mm_;
	r_stats.max_correction_m = max_correction_m_;
	r_stats.teleports = teleports_;
	r_stats.hash_mismatches = hash_mismatches_;
	r_stats.health_rebases = health_rebases_;
}

}

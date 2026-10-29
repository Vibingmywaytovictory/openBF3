#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "client_clock.h"
#include "client_gunplay.h"
#include "client_input.h"

#include <cstdint>
#include <memory>

namespace bf3::host {

class OwnPrediction {
public:
	OwnPrediction(const ClientSessionConfig &p_config, ClientClock &r_clock, ClientInput &r_input, ClientGunplay &r_gunplay) :
			config_(p_config), clock_(r_clock), input_(r_input), gunplay_(r_gunplay) {}

	core::Status init(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor);
	uint64_t motor_hash() const { return motor_hash_; }
	bool motor_matches(uint64_t p_server_motor_hash);
	void match_started(uint64_t p_now);
	void attach_weapons(sim::WeaponLane *p_lane) { ring_->attach_weapons(p_lane); }
	void discard_after(sim::Tick p_tick) { ring_->discard_after(p_tick); }

	bool have_own() const { return have_own_; }
	uint32_t own_life() const { return own_life_; }
	const sim::SoldierState &spawn_state() const { return spawn_state_; }
	void note_server_events(uint16_t p_events) { server_events_acc_ = uint16_t(server_events_acc_ | p_events); }

	void decay(uint64_t p_now);
	void authoritative(const sim::SoldierState &p_s, uint64_t p_now);
	void fill_base(sim::Tick p_tick);
	const sim::PredictionEntry *entry(sim::Tick p_tick) const { return ring_->find(p_tick); }
	bool step(const sim::PredictionEntry *p_base, const sim::Command &p_command);
	void publish(PresentationFrame &r_frame, const sim::PredictionEntry *p_made, const sim::Command &p_command, uint64_t p_now);

	void stats(ClientStats &r_stats) const;

private:
	void catch_up(const sim::SoldierState &p_base);
	void after_match(const sim::SoldierState &p_s);

	const ClientSessionConfig &config_;
	ClientClock &clock_;
	ClientInput &input_;
	ClientGunplay &gunplay_;
	const sim::CollisionWorld *world_ = nullptr;
	sim::SoldierMotor motor_;
	std::unique_ptr<sim::PredictionRing> ring_;
	uint64_t motor_hash_ = 0;

	bool have_own_ = false;
	uint32_t own_life_ = 0;
	sim::SoldierState spawn_state_;
	bool have_auth_ = false;
	sim::SoldierState last_auth_;
	sim::Vec3 visual_offset_{};
	uint32_t events_acc_ = 0;
	uint16_t server_events_acc_ = 0;
	uint64_t last_tick_ns_ = 0;

	bool spawned_ = false;
	uint64_t motor_hash_mismatch_ = 0;
	sim::Tick last_server_tick_ = 0;
	uint64_t reconciliations_ = 0;
	sim::Tick last_reconcile_tick_ = 0;
	uint64_t last_reconcile_ns_ = 0;
	uint64_t ring_misses_ = 0;
	uint64_t spawns_ = 0;
	uint64_t replayed_ticks_ = 0;
	uint64_t base_fills_ = 0;
	uint64_t step_precondition_failures_ = 0;
	uint64_t corrections_over_1mm_ = 0;
	float max_correction_m_ = 0.0f;
	uint64_t teleports_ = 0;
	uint64_t hash_mismatches_ = 0;
	uint64_t health_rebases_ = 0;
};

}

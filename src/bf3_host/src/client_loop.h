#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "client_clock.h"
#include "client_events.h"
#include "client_gunplay.h"
#include "client_input.h"
#include "client_link.h"
#include "client_match.h"
#include "client_tracks.h"
#include "client_transport.h"

#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace bf3::host {

class ClientLoop {
public:
	core::Status init(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level);
	void begin(std::unique_ptr<ClientTransport> p_transport, MonotonicClock p_clock);
	void iterate();
	void shutdown();
	void close();

	void set_input(const InputSample &p_input);
	bool read_presentation(PresentationFrame &r_frame) const;
	ClientStats stats() const;
	void send_debug(const net::DebugRequest &p_request);
	bool take_debug_report(net::DebugReport &r_report);
	void shift_clock(int32_t p_ticks);
	PresentationEvents &events();
	void take_predicted(std::vector<PredictedShot> &r_shots, std::vector<PredictedTimed> &r_timed);
	void take_own_test_samples(std::vector<OwnTestSample> &r_samples);
	void send_kit_select(const net::KitSelect &p_select);
	void send_spawn_request(uint8_t p_kit);

private:
	ClientSessionConfig config;
	std::shared_ptr<const SharedLevel> level;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	std::unique_ptr<sim::PredictionRing> ring;
	uint64_t motor_hash = 0;
	MonotonicClock now_ns;
	ClientLink link{ now_ns };

	mutable std::mutex frame_mutex;
	std::unique_ptr<PresentationFrame> shared_frame = std::make_unique<PresentationFrame>();
	std::atomic<int32_t> shift_request{ 0 };

	std::unique_ptr<PresentationFrame> build = std::make_unique<PresentationFrame>();
	ClientStats stats_;
	uint64_t serial = 0;
	bool have_match = false;
	uint8_t slot = 0;
	ClientClock clock{ config };
	ClientInput input;

	bool have_own = false;
	uint32_t own_life = 0;
	sim::SoldierState spawn_state;
	bool have_auth = false;
	sim::SoldierState last_auth;
	sim::Vec3 visual_offset{};
	uint32_t events_acc = 0;
	uint16_t server_events_acc = 0;

	bool pending_snapshot_valid = false;
	net::Snapshot pending_snapshot;
	std::vector<sim::SoldierState> pending_spawned;

	RemoteTracks tracks{ config };
	uint64_t last_tick_ns = 0;

	ClientEvents feed{ slot };
	ClientMatch match{ feed, slot };
	ClientGunplay gunplay{ config, feed, slot, now_ns };

	void refuse(const char *p_reason);
	bool accept_match(const net::MatchInfo &p_match, uint64_t p_now);
	void gunplay_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d, uint64_t p_now);
	void after_match(const sim::SoldierState &p_s);

	void handle_datagram(const net::NetSim::Datagram &p_d, uint64_t p_now);
	void run_catch_up(const sim::SoldierState &p_base);
	void authoritative(const sim::SoldierState &p_s, uint64_t p_now);
	void client_tick(uint64_t p_now);
	void publish(uint64_t p_now, const sim::PredictionEntry *p_made, const sim::Command &p_cmd);
	void collect_stats();
	static void copy_frame(PresentationFrame &r_to, const PresentationFrame &p_from);
};

}

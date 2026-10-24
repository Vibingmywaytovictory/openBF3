#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "client_clock.h"
#include "client_link.h"
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
	struct Track {
		bool used = false;
		uint8_t generation = 0;
		uint64_t last_seen_ns = 0;
		std::deque<OtherSample> samples;
	};
	ClientSessionConfig config;
	std::shared_ptr<const SharedLevel> level;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	std::unique_ptr<sim::PredictionRing> ring;
	uint64_t motor_hash = 0;
	MonotonicClock now_ns;
	ClientLink link{ now_ns };

	mutable std::mutex input_mutex;
	InputSample input;
	mutable std::mutex frame_mutex;
	std::unique_ptr<PresentationFrame> shared_frame = std::make_unique<PresentationFrame>();
	std::atomic<int32_t> shift_request{ 0 };

	std::unique_ptr<PresentationFrame> build = std::make_unique<PresentationFrame>();
	ClientStats stats_;
	uint64_t serial = 0;
	bool have_match = false;
	uint8_t slot = 0;
	uint8_t divisor = 2;
	ClientClock clock{ config };

	uint32_t next_sequence = 1;
	sim::Command last_made;
	bool jump_seen = false;
	uint32_t last_jump_presses = 0;
	sim::Pose keep_pose = sim::Pose::Stand;

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

	std::array<Track, net::kMaxSlots> tracks;
	bool recv_valid = false;
	double recv_offset = 0;
	double recv_rate = 0;
	std::deque<std::pair<uint64_t, double>> offsets;
	uint64_t last_arrival_ns = 0;
	double interval_mean_ms = 0;
	double interval_mad_ms = 0;
	uint64_t last_tick_ns = 0;

	struct ClSeq {
		bool valid = false;
		uint16_t seq = 0;
		uint32_t tick = 0;
		bool main = false;
		bool others = false;
		std::vector<net::OtherSoldier> main_records, others_records;
		std::array<uint8_t, net::kMaxSlots> main_at{}, others_at{};
		std::array<uint64_t, 2> main_slots{}, others_slots{};
	};
	struct ClRecords final : ReceivedRecordSource {
		static constexpr size_t kSeqs = 64;
		std::array<ClSeq, kSeqs> ring;
		const ClSeq *find(uint16_t p_seq) const {
			const ClSeq &e = ring[p_seq % kSeqs];
			return e.valid && e.seq == p_seq ? &e : nullptr;
		}
		void add(uint16_t p_seq, uint32_t p_tick, bool p_main, std::span<const net::OtherSoldier> p_records);
		bool arrived(uint16_t p_seq, bool p_main) const {
			const ClSeq *e = find(p_seq);
			return e != nullptr && (p_main ? e->main : e->others);
		}

		uint32_t mask(uint16_t p_newest, bool p_main) const;
		size_t candidates(const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const override;
		bool view_tick(uint16_t p_seq, uint32_t &r_tick) const override;
		void slots_in(const ShotView &p_view, std::array<uint64_t, 2> &r_slots) const override;
	};
	struct ClEmit final : sim::WeaponEmitSink {
		ClientLoop *m = nullptr;
		void emitted(const sim::Command &p_command, const sim::weapons::WeaponTickOutput &p_output, const sim::WeaponPredictionEntry &p_after) override;
	};

	struct ClCommandView {
		bool valid = false;
		sim::Tick tick = 0;
		uint32_t sequence = 0;
		ShotView view;
	};
	struct ClLife {
		bool valid = false;
		uint32_t life = 0;
		uint8_t kit = 0;
		uint8_t team = 0;
	};
	static constexpr size_t kClCommandViews = 256;
	static constexpr size_t kClLogLimit = size_t(1) << 20;

	bool cl_gunplay = false;
	std::shared_ptr<const sim::weapons::WeaponTable> cl_table;
	std::unique_ptr<sim::WeaponLane> cl_lane;
	std::unique_ptr<ClientProjectiles> cl_projectiles;
	ClRecords cl_records;
	ClEmit cl_emit;
	uint64_t cl_match_seed = 0;
	net::ShotQuantization cl_frame;
	std::array<ClLife, 8> cl_lives{};
	uint8_t cl_kit = 0;
	uint8_t cl_team = 0;
	bool cl_wire_valid = false;
	net::WeaponsWire cl_wire;
	sim::Tick cl_wire_tick = 0;
	sim::Tick cl_newest_tick = 0;
	std::array<ClCommandView, kClCommandViews> cl_views{};
	bool cl_fire_seen = false;
	uint32_t cl_last_fire_presses = 0;
	bool cl_fired = false;
	uint16_t cl_fired_view_seq = 0;
	uint64_t cl_fired_ns = 0;
	PresentationEvents cl_events;
	std::mutex cl_log_mutex;
	std::vector<PredictedShot> cl_shot_log;
	std::vector<PredictedTimed> cl_timed_log;
	std::vector<OwnTestSample> cl_sample_log;
	std::vector<OwnTestSample> cl_sample_scratch;
	std::vector<net::TimedOther> cl_track_scratch;
	std::vector<net::OtherPose> cl_poses;

	void cl_push(const PresentEvent &p_event);
	bool cl_match_info(const net::MatchInfo &p_match, uint64_t p_now);
	void cl_refuse(const char *p_reason);
	static bool cl_slot_ok(uint32_t p_slot);
	bool cl_def_ok(uint32_t p_def) const;
	void cl_spawned(const net::Spawned &p_spawned);
	void cl_snapshot(const net::Snapshot &p_snapshot, uint64_t p_now);
	void cl_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d, uint64_t p_now);
	void cl_note_arrival(uint16_t p_seq, uint64_t p_now);
	void cl_track_reorder(sim::Tick p_tick, std::span<const net::OtherSoldier> p_records);
	bool cl_wire_at(sim::Tick p_tick) const { return cl_gunplay && cl_wire_valid && cl_wire_tick == p_tick; }
	bool cl_put_wire(const sim::SoldierState &p_s, sim::Tick p_spawn_tick);
	void cl_weapons_spawn(const sim::SoldierState &p_s);
	void cl_weapons_ring_miss(const sim::SoldierState &p_s);
	void cl_weapons_reconcile(const sim::SoldierState &p_s);
	void cl_after_match(const sim::SoldierState &p_s);
	void cl_command_made(sim::Command &r_command, uint64_t p_now);
	void cl_predict_weapons(const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after);
	uint32_t cl_others_ack_bits() const;
	void cl_publish_begin(uint64_t p_now);
	void cl_publish_end(PresentationFrame &r_frame, uint64_t p_now);

	static double local_ticks(uint64_t p_ns);
	void received_time_sample(sim::Tick p_tick, uint64_t p_now);
	void add_others(const net::Snapshot &p_snap, uint64_t p_now);
	void handle_datagram(const net::NetSim::Datagram &p_d, uint64_t p_now);
	void run_catch_up(const sim::SoldierState &p_base);
	void authoritative(const sim::SoldierState &p_s, uint64_t p_now);
	sim::Command sample_command(sim::Tick p_tick);
	void client_tick(uint64_t p_now);
	void publish(uint64_t p_now, const sim::PredictionEntry *p_made, const sim::Command &p_cmd);
	void collect_stats();
	static void copy_frame(PresentationFrame &r_to, const PresentationFrame &p_from);

	bool tdm_match_valid = false;
	net::MatchState tdm_match;
	PlayerTable tdm_players;
	bool tdm_death_valid = false;
	uint32_t tdm_death_life = 0;
	sim::Tick tdm_respawn_tick = 0;
	sim::Tick tdm_revive_until = 0;
	bool tdm_unspawned_now = false;
	std::array<uint32_t, 8> tdm_revived_lives{};
	OwnShotIds tdm_shot_ids;
	PresentationEvents tdm_own_events;
	bool tdm_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d);
	void tdm_spawned(const net::Spawned &p_spawned);
	void tdm_combat(const net::CombatEvents &p_events);
	bool tdm_rules(const net::MatchInfo &p_match);
	void tdm_command(sim::Command &r_command);
	bool tdm_unspawned(sim::Tick p_tick);
	void tdm_push(PresentEvent &r_event);
	void tdm_drain_own();
	void tdm_publish(PresentationFrame &r_frame);
};

}

#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/netsim.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/prediction.h"
#include "client_events.h"
#include "client_transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::host {

class ClientGunplay {
public:
	ClientGunplay(const ClientSessionConfig &p_config, ClientEvents &r_events, const uint8_t &p_own_slot, const MonotonicClock &p_clock) :
			config_(p_config), events_(r_events), own_slot_(p_own_slot), clock_(p_clock) {}

	bool on() const { return on_; }
	sim::WeaponLane *lane() { return lane_.get(); }
	const net::ShotQuantization &frame() const { return frame_; }
	bool wants_poses() const { return projectiles_ != nullptr; }
	sim::Tick newest_tick() const { return newest_tick_; }

	std::optional<std::string> match_info(const net::MatchInfo &p_match, const SharedLevel &p_level);
	void spawned(const net::Spawned &p_spawned, bool p_have_own, uint32_t p_own_life);
	void note_arrival(uint16_t p_seq, uint64_t p_now);
	void snapshot(const net::Snapshot &p_snapshot, bool p_newer);
	void others(const net::SnapshotOthers &p_others, uint64_t p_now);
	void bad_datagram() { ++bad_datagrams_; }
	void shot_events(const net::NetSim::Datagram &p_d);
	bool combat(const net::NetSim::Datagram &p_d, net::CombatEvents &r_events);

	void weapons_spawn(const sim::SoldierState &p_s);
	void weapons_ring_miss(const sim::SoldierState &p_s);
	void weapons_reconcile(const sim::SoldierState &p_s);
	bool reconcile_after_match(const sim::SoldierState &p_s);

	void command_made(sim::Command &r_command, const InputSample &p_input, uint32_t p_own_life, uint64_t p_now, bool p_have_snapshot, uint16_t p_newest_seq,
			const PresentationFrame &p_built);
	void predict(const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after, const sim::CollisionWorld &p_world);
	uint32_t others_ack_bits(bool p_have_snapshot, uint16_t p_newest_seq) const;
	void publish(PresentationFrame &r_frame, std::span<const net::OtherPose> p_poses, const sim::CollisionWorld &p_world);

	void take_predicted(std::vector<PredictedShot> &r_shots, std::vector<PredictedTimed> &r_timed);
	void take_own_test_samples(std::vector<OwnTestSample> &r_samples);

	void publish_stats(ClientStats &r_stats) const;
	uint64_t bad_events() const { return bad_events_; }
	void stats(ClientStats &r_stats) const;

private:
	struct Seq {
		bool valid = false;
		uint16_t seq = 0;
		uint32_t tick = 0;
		bool main = false;
		bool others = false;
		std::vector<net::OtherSoldier> main_records, others_records;
		std::array<uint8_t, net::kMaxSlots> main_at{}, others_at{};
		std::array<uint64_t, 2> main_slots{}, others_slots{};
	};
	struct Records final : ReceivedRecordSource {
		static constexpr size_t kSeqs = 64;
		std::array<Seq, kSeqs> ring;
		const Seq *find(uint16_t p_seq) const {
			const Seq &e = ring[p_seq % kSeqs];
			return e.valid && e.seq == p_seq ? &e : nullptr;
		}
		void add(uint16_t p_seq, uint32_t p_tick, bool p_main, std::span<const net::OtherSoldier> p_records);
		bool arrived(uint16_t p_seq, bool p_main) const {
			const Seq *e = find(p_seq);
			return e != nullptr && (p_main ? e->main : e->others);
		}

		uint32_t mask(uint16_t p_newest, bool p_main) const;
		size_t candidates(const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const override;
		bool view_tick(uint16_t p_seq, uint32_t &r_tick) const override;
		void slots_in(const ShotView &p_view, std::array<uint64_t, 2> &r_slots) const override;
	};
	struct Emit final : sim::WeaponEmitSink {
		ClientGunplay *m = nullptr;
		void emitted(const sim::Command &p_command, const sim::weapons::WeaponTickOutput &p_output, const sim::WeaponPredictionEntry &p_after) override;
	};
	struct CommandView {
		bool valid = false;
		sim::Tick tick = 0;
		uint32_t sequence = 0;
		ShotView view;
	};
	struct Life {
		bool valid = false;
		uint32_t life = 0;
		uint8_t kit = 0;
		uint8_t team = 0;
	};
	static constexpr size_t kCommandViews = 256;
	static constexpr size_t kLogLimit = size_t(1) << 20;

	bool def_ok(uint32_t p_def) const;
	bool wire_at(sim::Tick p_tick) const { return on_ && wire_valid_ && wire_tick_ == p_tick; }
	bool put_wire(const sim::SoldierState &p_s, sim::Tick p_spawn_tick);

	const ClientSessionConfig &config_;
	ClientEvents &events_;
	const uint8_t &own_slot_;
	const MonotonicClock &clock_;

	bool on_ = false;
	std::shared_ptr<const ClientGunplayInputs> inputs_;
	std::shared_ptr<const sim::weapons::WeaponTable> table_;
	std::unique_ptr<sim::WeaponLane> lane_;
	std::unique_ptr<ClientProjectiles> projectiles_;
	Records records_;
	Emit emit_;
	uint64_t match_seed_ = 0;
	net::ShotQuantization frame_;
	std::array<Life, 8> lives_{};
	uint8_t kit_ = 0;
	uint8_t team_ = 0;
	bool wire_valid_ = false;
	net::WeaponsWire wire_;
	sim::Tick wire_tick_ = 0;
	sim::Tick newest_tick_ = 0;
	std::array<CommandView, kCommandViews> views_{};
	bool fire_seen_ = false;
	uint32_t last_fire_presses_ = 0;
	bool fired_ = false;
	uint16_t fired_view_seq_ = 0;
	uint64_t fired_ns_ = 0;
	std::mutex log_mutex_;
	std::vector<PredictedShot> shot_log_;
	std::vector<PredictedTimed> timed_log_;
	std::vector<OwnTestSample> sample_log_;
	std::vector<OwnTestSample> sample_scratch_;

	uint64_t weapon_hash_mismatch_ = 0;
	uint64_t kit_hash_mismatch_ = 0;
	uint64_t weapon_reconciliations_ = 0;
	uint64_t weapon_ring_misses_ = 0;
	uint64_t predicted_shots_ = 0;
	uint64_t predicted_timed_ = 0;
	uint64_t no_view_shots_ = 0;
	uint64_t snapshot_others_received_ = 0;
	uint64_t remote_shots_ = 0;
	uint64_t combat_hits_ = 0;
	uint64_t combat_deaths_ = 0;
	uint64_t combat_detonations_ = 0;
	uint64_t bad_datagrams_ = 0;
	uint64_t bad_weapon_wires_ = 0;
	uint64_t bad_events_ = 0;
	uint64_t late_sample_ = 0;
};

}

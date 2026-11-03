#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "client_clock.h"
#include "client_events.h"
#include "client_gunplay.h"
#include "client_input.h"
#include "client_link.h"
#include "client_match.h"
#include "client_prediction.h"
#include "client_tracks.h"
#include "client_transport.h"

#include "bf3/sim/collision_world.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace bf3::host {

class ClientLoop {
public:
	static constexpr size_t kMaxHeldDatagrams = 16384;
	static constexpr uint64_t kLoadReportNs = 500'000'000ull;

	core::Status init(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level);
	void configure(const ClientSessionConfig &p_config);
	core::Status attach_level(std::shared_ptr<const SharedLevel> p_level);
	void configure_level(const ClientLevelConfig &p_config);
	core::Status detach_level();
	bool lobby_match(net::MatchInfo &r_match) const;
	bool level_change(net::LevelChange &r_change) const;
	void set_load_progress(const net::Loading &p_progress);
	bool round_over(ClientRoundOver &r_round_over) const;
	void set_prepare_progress(const net::Prepare &p_progress);
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
	void take_logic(ClientLogicFeed &r_feed) { match.take_logic(r_feed); }
	void take_own_test_samples(std::vector<OwnTestSample> &r_samples);
	void send_kit_select(const net::KitSelect &p_select);
	void send_spawn_request(uint8_t p_kit, uint8_t p_spawn_choice = 0);
	void send_squad_request(const net::SquadRequest &p_request);
	void send_suicide_request();
	void send_loadout_request(const net::LoadoutRequest &p_request);

private:
	ClientSessionConfig config;
	std::shared_ptr<const SharedLevel> level;
	std::unique_ptr<sim::CollisionWorld> world;
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

	bool pending_snapshot_valid = false;
	net::Snapshot pending_snapshot;
	net::SnapshotBaselines baselines;
	std::vector<sim::SoldierState> pending_spawned;

	struct HeldDatagram {
		net::NetSim::Datagram datagram;
		uint64_t at = 0;
	};
	std::vector<HeldDatagram> held;
	mutable std::mutex lobby_mutex;
	std::optional<net::MatchInfo> lobby_info;
	std::optional<net::LevelChange> lobby_change;
	std::optional<net::Loading> load_report;
	uint64_t load_report_sent_ns = 0;
	std::optional<ClientRoundOver> round_over_info;
	std::optional<net::Prepare> prepare_report;
	std::optional<net::Prepare> prepare_last;
	uint64_t prepare_sent_ns = 0;
	net::MatchInfo match_identity;
	bool following = false;
	std::optional<uint64_t> mismatch_deadline;

	RemoteTracks tracks{ config };
	ClientEvents feed{ slot };
	ClientMatch match{ feed, slot };
	ClientGunplay gunplay{ config, feed, slot, now_ns };
	OwnPrediction prediction{ config, clock, input, gunplay };

	void refuse(const char *p_reason);
	void send_mismatch(uint8_t p_components, uint64_t p_now);
	void note_kick(const net::Kick &p_kick);
	bool accept_match(const net::MatchInfo &p_match, uint64_t p_now);
	void gunplay_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d, uint64_t p_now);
	void handle_datagram(const net::NetSim::Datagram &p_d, uint64_t p_now);
	void hold(net::NetSim::Datagram &&p_d, uint64_t p_now);
	void note_level_change(const net::LevelChange &p_change);
	void note_lobby_match(const net::MatchInfo &p_match);
	void send_load_report(uint64_t p_now);
	void note_round_over(const net::NetSim::Datagram &p_d);
	void send_prepare_report(uint64_t p_now);
	void reset_level_state();
	void publish_link_stats();
	void client_tick(uint64_t p_now);
	void publish(uint64_t p_now, const sim::PredictionEntry *p_made, const sim::Command &p_cmd);
	void collect_stats();
	static void copy_frame(PresentationFrame &r_to, const PresentationFrame &p_from);
};

}

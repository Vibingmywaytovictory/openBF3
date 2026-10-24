#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

namespace bf3::host {

class RemoteTracks {
public:
	explicit RemoteTracks(const ClientSessionConfig &p_config) :
			config_(p_config) {}

	void set_divisor(uint8_t p_divisor) { divisor_ = p_divisor; }
	void add(sim::Tick p_server_tick, std::span<const net::OtherSoldier> p_others, uint8_t p_own_slot, uint64_t p_now);
	void arrival(sim::Tick p_server_tick, uint64_t p_now);
	void reorder(sim::Tick p_tick, std::span<const net::OtherSoldier> p_records, uint8_t p_own_slot);

	void count_timeouts(uint64_t p_now);
	void publish(PresentationFrame &r_frame, uint64_t p_now);
	std::span<const net::OtherPose> finish(PresentationFrame &r_frame, bool p_have_snapshot, sim::Tick p_newest_tick, bool p_poses);

	void publish_stats(ClientStats &r_stats) const { r_stats.interp_delay_ms = delay_ms_; }
	void stats(ClientStats &r_stats) const;

private:
	struct Track {
		bool used = false;
		uint8_t generation = 0;
		uint64_t last_seen_ns = 0;
		std::deque<OtherSample> samples;
	};

	const ClientSessionConfig &config_;
	uint8_t divisor_ = 2;
	std::array<Track, net::kMaxSlots> tracks_;
	bool recv_valid_ = false;
	double recv_offset_ = 0;
	std::deque<std::pair<uint64_t, double>> offsets_;
	uint64_t last_arrival_ns_ = 0;
	double interval_mean_ms_ = 0;
	double interval_mad_ms_ = 0;
	double delay_ms_ = 0;
	std::vector<net::TimedOther> scratch_;
	std::vector<net::OtherPose> poses_;

	uint64_t generation_reorder_ = 0;
	uint64_t others_ahead_ = 0;
	uint64_t track_timeout_ = 0;
};

}

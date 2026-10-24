#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace bf3::host {

class ClientClock {
public:
	explicit ClientClock(const ClientSessionConfig &p_config) :
			config_(p_config) {}

	bool running() const { return running_; }
	sim::Tick next_tick() const { return next_tick_; }
	void set_next_tick(sim::Tick p_tick) { next_tick_ = p_tick; }
	double ticks(uint64_t p_now) const;
	double own_ticks_per_ns() const;
	uint64_t tick_deadline_ns(sim::Tick p_tick) const;

	void hello_sent(uint64_t p_now) { hello_ns_ = p_now; }
	void start(sim::Tick p_server_tick, uint64_t p_now, uint32_t p_next_sequence);
	void anchor_spawned(sim::Tick p_tick, uint64_t p_now);
	void anchor_snapshot(sim::Tick p_tick, uint64_t p_now);
	void command_sent(uint32_t p_sequence, uint64_t p_now);
	void rtt_sample(const net::Snapshot &p_snap, uint64_t p_now, uint32_t p_next_sequence);
	std::optional<sim::Tick> buffer_report(const net::Snapshot &p_snap, uint64_t p_now, uint32_t p_next_sequence);
	void force_past(sim::Tick p_tick, uint64_t p_now, uint32_t p_next_sequence);
	void shift(int32_t p_ticks, uint64_t p_now);
	bool due(uint64_t p_now);

	void publish_stats(ClientStats &r_stats) const;
	void stats(ClientStats &r_stats) const;

private:
	struct SentTime {
		uint32_t sequence = 0;
		uint64_t ns = 0;
	};
	struct RttSample {
		uint64_t ns = 0;
		double ms = 0;
	};
	static constexpr size_t kSentTimes = 1024;

	void set(double p_ticks, uint64_t p_now);
	void set_rate(double p_scale, uint64_t p_now);
	double rtt_for_estimate_ms() const;
	double estimate(uint64_t p_now) const;
	void begin_holdoff(uint32_t p_next_sequence);

	const ClientSessionConfig &config_;
	bool running_ = false;
	double base_ticks_ = 0;
	uint64_t base_ns_ = 0;
	double rate_scale_ = 1.0;
	sim::Tick next_tick_ = 0;
	bool holdoff_ = true;
	uint32_t holdoff_sequence_ = 1;
	std::deque<int32_t> reports64_;
	std::deque<int32_t> reports8_;
	uint32_t target_ = 2;
	uint32_t candidate_ = 2;
	uint64_t candidate_since_ns_ = 0;
	uint64_t hello_ns_ = 0;
	double hello_rtt_ms_ = 0;
	sim::Tick anchor_tick_ = 0;
	uint64_t anchor_ns_ = 0;
	std::array<SentTime, kSentTimes> sent_times_{};
	std::deque<RttSample> rtt_window_;
	bool rtt_valid_ = false;
	double rtt_ms_ = 0.0;
	double rtt_min_ms_ = 0.0;

	int32_t command_buffer_ticks_ = 0;
	uint64_t clock_resets_ = 0;
	uint64_t forced_forward_ = 0;
};

}

#pragma once

#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::net {

struct NetSimConfig {
	float latency_min_ms = 0;
	float latency_max_ms = 0;
	float loss = 0;
	float spike_chance = 0;
	float spike_min_ms = 300;
	float spike_max_ms = 1000;
	float reorder = 0;
	float duplicate = 0;
	uint64_t seed = 1;

	bool enabled() const { return latency_max_ms > 0 || loss > 0 || spike_chance > 0 || reorder > 0 || duplicate > 0; }
};

inline constexpr size_t kMaxNetSimProfileBytes = 4096;
inline constexpr size_t kMaxNetSimProfileItems = 64;

core::Result<NetSimConfig> parse_netsim(std::string_view p_profile);
std::string format_netsim(const NetSimConfig &p_config);

struct NetSimStats {
	uint64_t submitted = 0;
	uint64_t delivered = 0;
	uint64_t dropped = 0;
	uint64_t spike_dropped = 0;
	uint64_t duplicated = 0;
	uint64_t reordered = 0;
	double mean_delay_ms = 0;
};

class NetSim {
public:
	struct Datagram {
		uint32_t peer = 0;
		uint8_t channel = 0;
		bool reliable = false;
		std::vector<uint8_t> bytes;
	};

	explicit NetSim(const NetSimConfig &p_config = {});

	void submit(Datagram p_datagram, uint64_t p_now_ns);

	bool poll(uint64_t p_now_ns, Datagram &r_datagram);

	uint64_t next_release_ns() const;
	uint64_t last_reliable_release_ns(uint32_t p_peer) const;
	size_t pending() const;
	const NetSimConfig &config() const { return config_; }
	const NetSimStats &stats() const { return stats_; }

private:
	struct Pending {
		uint64_t release_ns = 0;
		uint64_t order = 0;
		Datagram datagram;
	};
	NetSimConfig config_;
	NetSimStats stats_;
	std::deque<Pending> queue_;
	uint64_t rng_ = 0;
	uint64_t next_order_ = 0;
	uint64_t spike_until_ns_ = 0;
	uint64_t last_spike_check_ns_ = 0;
};

}

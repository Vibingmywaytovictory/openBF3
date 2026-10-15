#pragma once
// Network-conditions simulator (plan §2.2 bf3_net "netsim (BF3's own knobs)", §3.5; topics/05 §1.1 Game.cfg
// IsInternetSimulationEnabled / MinLatency / MaxLatency / PacketDrops / DropSpikeChance / Min/MaxDropDuration /
// ReorderingChance / DuplicationChance; docs/design/m1.md §7.6). Godot-free, single-threaded (one per direction,
// owned by the thread that services the endpoint).
//
// It sits between a session and its ENet host: every datagram the session sends goes through submit() and is
// handed to ENet when poll() releases it; every datagram ENet delivers goes through another NetSim before the
// session sees it. Latency applies to every channel; loss, drop spikes, duplication and reordering apply to
// unreliable datagrams only (a reliable datagram is delayed, in order, never dropped: ENet would resend it).
// M1 runs one NetSim per direction at the client endpoint, so the server path is the remote path unchanged.
//
// Profiles: "off"; "<rtt_ms>/<loss_percent>" e.g. "150/2" = 75 ms one way in each direction (so 150 ms round trip),
// +-10 % jitter, 2 % loss in each direction (the M1 exit "150 ms / 2 %"); or explicit keys
// "lat=60-90,loss=2,spike=0,spike_ms=300-1000,reorder=1,dup=1,seed=7" (milliseconds and percent, one direction);
// "bf3" = the Game.cfg values (100-200 ms, 1 % drops, 3 % spikes of 0.3-1.0 s, 1 % reorder, 2 % duplicate).

#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::net {

struct NetSimConfig {
	float latency_min_ms = 0; // one way
	float latency_max_ms = 0; // uniform in [min, max]: the jitter
	float loss = 0; // probability per unreliable datagram
	float spike_chance = 0; // per second: start a drop spike (every unreliable datagram lost while it lasts)
	float spike_min_ms = 300;
	float spike_max_ms = 1000;
	float reorder = 0; // probability an unreliable datagram is held back behind the next one
	float duplicate = 0; // probability an unreliable datagram is delivered twice
	uint64_t seed = 1;

	bool enabled() const { return latency_max_ms > 0 || loss > 0 || spike_chance > 0 || reorder > 0 || duplicate > 0; }
};

// A profile string as described above (one direction's config; the same config is used both ways).
core::Result<NetSimConfig> parse_netsim(std::string_view p_profile);
std::string format_netsim(const NetSimConfig &p_config);

struct NetSimStats {
	uint64_t submitted = 0;
	uint64_t delivered = 0;
	uint64_t dropped = 0; // loss + spikes
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

	// p_now_ns: monotonic time (net::monotonic_ns). A disabled simulator releases at once (poll right after).
	void submit(Datagram p_datagram, uint64_t p_now_ns);
	// Next datagram due at p_now_ns, in release order (reliable ones in submit order per channel).
	bool poll(uint64_t p_now_ns, Datagram &r_datagram);
	// Earliest pending release time, or 0 when nothing is pending (lets the owner sleep precisely).
	uint64_t next_release_ns() const;
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
	std::deque<Pending> queue_; // kept sorted by (release_ns, order)
	uint64_t rng_ = 0;
	uint64_t next_order_ = 0;
	uint64_t spike_until_ns_ = 0;
	uint64_t last_spike_check_ns_ = 0;
};

} // namespace bf3::net

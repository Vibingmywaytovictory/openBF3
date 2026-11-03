#pragma once

#include "bf3/host/own_hits.h"
#include "bf3/net/enet_host.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bf3::host {

inline constexpr size_t kNetTypeBuckets = 48;
inline constexpr uint64_t kNetStatsWindowNs = 1'000'000'000ull;

size_t net_type_bucket(std::span<const uint8_t> p_datagram);
const char *net_type_name(size_t p_bucket);

struct NetTypeCounters {
	uint64_t datagrams = 0;
	uint64_t payload_bytes = 0;
	uint64_t enet_bytes = 0;
};

struct NetTraffic {
	std::array<NetTypeCounters, kNetTypeBuckets> types{};

	void count(std::span<const uint8_t> p_datagram, net::Host::SendMode p_mode);
	void count(uint8_t p_channel, std::span<const uint8_t> p_datagram);
	NetTypeCounters total() const;
};
static_assert(std::is_trivially_copyable_v<NetTraffic>);

struct SlotTraffic {
	uint8_t slot = 0;
	uint32_t peer = 0;
	NetTraffic sent;
	NetTraffic received;
};

struct NetRates {
	std::array<float, kNetTypeBuckets> bytes_per_s{};
	float total_bytes_per_s = 0.0f;
	float payload_bytes_per_s = 0.0f;
	float datagrams_per_s = 0.0f;
};

struct NetStats {
	uint64_t revision = 0;
	double window_s = 0.0;
	NetRates in;
	NetRates out;
	float rtt_ms = 0.0f;
	float snapshots_per_s = 0.0f;
	float snapshot_loss = 0.0f;
	uint32_t ghosts_in_scope = 0;
	sim::Tick server_tick = 0;
	OwnHitCounts own_hits;
};
static_assert(std::is_trivially_copyable_v<NetStats>);

struct NetSample {
	const NetTraffic *in = nullptr;
	const NetTraffic *out = nullptr;
	uint64_t snapshots_received = 0;
	uint64_t snapshots_lost = 0;
	double rtt_ms = 0.0;
	uint32_t ghosts_in_scope = 0;
	sim::Tick server_tick = 0;
	OwnHitCounts own_hits{};
};

NetRates net_rates(const NetTraffic &p_from, const NetTraffic &p_to, double p_seconds);

class NetStatsMeter {
public:
	explicit NetStatsMeter(uint64_t p_window_ns = kNetStatsWindowNs) : window_ns_(p_window_ns) {}

	bool sample(const NetSample &p_sample, uint64_t p_now_ns, NetStats &r_stats);
	void reset() { have_base_ = false; }

private:
	uint64_t window_ns_;
	bool have_base_ = false;
	uint64_t base_ns_ = 0;
	NetTraffic base_in_;
	NetTraffic base_out_;
	uint64_t base_received_ = 0;
	uint64_t base_lost_ = 0;
	uint64_t revision_ = 0;
};

struct NetOverlayRow {
	std::string label;
	std::string value;
};

std::vector<NetOverlayRow> net_overlay_rows(const NetStats &p_stats);

class ServerNetMeter {
public:
	std::string report(
			std::span<const SlotTraffic> p_slots, std::span<const net::PeerCounters> p_enet, uint64_t p_now_ns);

private:
	struct Seen {
		uint8_t slot = 0;
		uint32_t peer = 0;
		NetTraffic sent;
		NetTraffic received;
		uint64_t enet_sent = 0;
		uint64_t enet_received = 0;
	};
	std::vector<Seen> seen_;
	uint64_t at_ns_ = 0;
};

}

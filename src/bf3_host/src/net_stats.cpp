#include "bf3/host/net_stats.h"

#include "bf3/core/util/format.h"
#include "bf3/net/protocol.h"

#include <algorithm>
#include <utility>

namespace bf3::host {

namespace {

struct TypeName {
	net::MessageType type;
	const char *name;
};

constexpr std::array<TypeName, 30> kTypeNames{ {
		{ net::MessageType::Snapshot, "Snapshot" },
		{ net::MessageType::CommandPacket, "CommandPacket" },
		{ net::MessageType::SnapshotOthers, "SnapshotOthers" },
		{ net::MessageType::ShotEvents, "ShotEvents" },
		{ net::MessageType::Hello, "Hello" },
		{ net::MessageType::MatchInfo, "MatchInfo" },
		{ net::MessageType::Ready, "Ready" },
		{ net::MessageType::Spawned, "Spawned" },
		{ net::MessageType::Loading, "Loading" },
		{ net::MessageType::Kick, "Kick" },
		{ net::MessageType::Bye, "Bye" },
		{ net::MessageType::CombatEvents, "CombatEvents" },
		{ net::MessageType::KitSelect, "KitSelect" },
		{ net::MessageType::MatchState, "MatchState" },
		{ net::MessageType::PlayerList, "PlayerList" },
		{ net::MessageType::SpawnRequest, "SpawnRequest" },
		{ net::MessageType::SuicideRequest, "SuicideRequest" },
		{ net::MessageType::LoadoutRequest, "LoadoutRequest" },
		{ net::MessageType::Debug, "Debug" },
		{ net::MessageType::DebugReport, "DebugReport" },
		{ net::MessageType::SoldierGear, "SoldierGear" },
		{ net::MessageType::Mismatch, "Mismatch" },
		{ net::MessageType::RoundOverInfo, "RoundOverInfo" },
		{ net::MessageType::Prepare, "Prepare" },
		{ net::MessageType::LevelChange, "LevelChange" },
		{ net::MessageType::LogicEvents, "LogicEvents" },
		{ net::MessageType::FullSync, "FullSync" },
		{ net::MessageType::LogicDelta, "LogicDelta" },
		{ net::MessageType::SquadRequest, "SquadRequest" },
		{ net::MessageType::SquadOrder, "SquadOrder" },
} };

static_assert(std::all_of(
		kTypeNames.begin(), kTypeNames.end(), [](const TypeName &p_t) { return size_t(p_t.type) < kNetTypeBuckets; }));

constexpr std::array<size_t, 7> kOverlayTypes{ size_t(net::MessageType::Snapshot),
	size_t(net::MessageType::SnapshotOthers), size_t(net::MessageType::ShotEvents),
	size_t(net::MessageType::CombatEvents), size_t(net::MessageType::PlayerList), size_t(net::MessageType::MatchState),
	size_t(net::MessageType::LogicEvents) };

constexpr size_t kServerReportTypes = 4;

uint64_t enet_total(const std::array<net::ChannelCounters, net::kCountedChannels> &p_channels) {
	uint64_t n = 0;
	for (const net::ChannelCounters &c : p_channels) {
		n += c.bytes;
	}
	return n;
}

float per_second(uint64_t p_from, uint64_t p_to, double p_seconds) {
	return p_seconds > 0.0 && p_to >= p_from ? float(double(p_to - p_from) / p_seconds) : 0.0f;
}

std::string rate_text(float p_bytes_per_s) {
	return core::fmt("%.0f B/s", double(p_bytes_per_s));
}

std::string top_types(const NetRates &p_rates) {
	std::array<size_t, kNetTypeBuckets> order{};
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.end(),
			[&](size_t a, size_t b) { return p_rates.bytes_per_s[a] > p_rates.bytes_per_s[b]; });
	std::string out;
	for (size_t i = 0; i < kServerReportTypes && p_rates.bytes_per_s[order[i]] > 0.0f; ++i) {
		out += core::fmt(
				"%s%s %.0f", out.empty() ? "" : ", ", net_type_name(order[i]), double(p_rates.bytes_per_s[order[i]]));
	}
	return out.empty() ? std::string("none") : out;
}

}

size_t net_type_bucket(std::span<const uint8_t> p_datagram) {
	return !p_datagram.empty() && p_datagram[0] < kNetTypeBuckets ? size_t(p_datagram[0]) : 0;
}

const char *net_type_name(size_t p_bucket) {
	for (const TypeName &t : kTypeNames) {
		if (size_t(t.type) == p_bucket) {
			return t.name;
		}
	}
	return "other";
}

void NetTraffic::count(std::span<const uint8_t> p_datagram, net::Host::SendMode p_mode) {
	NetTypeCounters &c = types[net_type_bucket(p_datagram)];
	c.datagrams++;
	c.payload_bytes += p_datagram.size();
	c.enet_bytes += net::enet_command_bytes(p_datagram.size(), p_mode);
}

void NetTraffic::count(uint8_t p_channel, std::span<const uint8_t> p_datagram) {
	count(p_datagram,
			p_channel == net::kChannelUnreliable ? net::Host::SendMode::Unsequenced : net::Host::SendMode::Reliable);
}

NetTypeCounters NetTraffic::total() const {
	NetTypeCounters t;
	for (const NetTypeCounters &c : types) {
		t.datagrams += c.datagrams;
		t.payload_bytes += c.payload_bytes;
		t.enet_bytes += c.enet_bytes;
	}
	return t;
}

NetRates net_rates(const NetTraffic &p_from, const NetTraffic &p_to, double p_seconds) {
	NetRates r;
	for (size_t i = 0; i < kNetTypeBuckets; ++i) {
		r.bytes_per_s[i] = per_second(p_from.types[i].enet_bytes, p_to.types[i].enet_bytes, p_seconds);
	}
	const NetTypeCounters a = p_from.total();
	const NetTypeCounters b = p_to.total();
	r.total_bytes_per_s = per_second(a.enet_bytes, b.enet_bytes, p_seconds);
	r.payload_bytes_per_s = per_second(a.payload_bytes, b.payload_bytes, p_seconds);
	r.datagrams_per_s = per_second(a.datagrams, b.datagrams, p_seconds);
	return r;
}

bool NetStatsMeter::sample(const NetSample &p_sample, uint64_t p_now_ns, NetStats &r_stats) {
	if (p_sample.in == nullptr || p_sample.out == nullptr) {
		return false;
	}
	if (!have_base_ || p_now_ns < base_ns_) {
		have_base_ = true;
		base_ns_ = p_now_ns;
		base_in_ = *p_sample.in;
		base_out_ = *p_sample.out;
		base_received_ = p_sample.snapshots_received;
		base_lost_ = p_sample.snapshots_lost;
		return false;
	}
	if (p_now_ns - base_ns_ < window_ns_) {
		return false;
	}
	const double seconds = double(p_now_ns - base_ns_) * 1e-9;
	NetStats s;
	s.revision = ++revision_;
	s.window_s = seconds;
	s.in = net_rates(base_in_, *p_sample.in, seconds);
	s.out = net_rates(base_out_, *p_sample.out, seconds);
	s.rtt_ms = float(p_sample.rtt_ms);
	s.snapshots_per_s = per_second(base_received_, p_sample.snapshots_received, seconds);
	const uint64_t received =
			p_sample.snapshots_received >= base_received_ ? p_sample.snapshots_received - base_received_ : 0;
	const uint64_t lost = p_sample.snapshots_lost >= base_lost_ ? p_sample.snapshots_lost - base_lost_ : 0;
	s.snapshot_loss = received + lost > 0 ? float(double(lost) / double(received + lost)) : 0.0f;
	s.ghosts_in_scope = p_sample.ghosts_in_scope;
	s.server_tick = p_sample.server_tick;
	s.own_hits = p_sample.own_hits;
	r_stats = s;
	base_ns_ = p_now_ns;
	base_in_ = *p_sample.in;
	base_out_ = *p_sample.out;
	base_received_ = p_sample.snapshots_received;
	base_lost_ = p_sample.snapshots_lost;
	return true;
}

std::vector<NetOverlayRow> net_overlay_rows(const NetStats &p_stats) {
	std::vector<NetOverlayRow> rows;
	rows.push_back({ "NET IN", rate_text(p_stats.in.total_bytes_per_s) });
	rows.push_back({ "NET OUT", rate_text(p_stats.out.total_bytes_per_s) });
	rows.push_back({ "RTT", core::fmt("%.0f ms", double(p_stats.rtt_ms)) });
	rows.push_back({ "SNAPSHOTS", core::fmt("%.1f /s", double(p_stats.snapshots_per_s)) });
	rows.push_back({ "SNAPSHOT LOSS", core::fmt("%.1f %%", 100.0 * double(p_stats.snapshot_loss)) });
	rows.push_back({ "GHOSTS IN SCOPE", core::fmt("%u", unsigned(p_stats.ghosts_in_scope)) });
	rows.push_back({ "SERVER TICK", core::fmt("%u", unsigned(p_stats.server_tick)) });
	for (const size_t t : kOverlayTypes) {
		rows.push_back({ std::string("IN ") + net_type_name(t), rate_text(p_stats.in.bytes_per_s[t]) });
	}
	rows.push_back({ std::string("OUT ") + net_type_name(size_t(net::MessageType::CommandPacket)),
			rate_text(p_stats.out.bytes_per_s[size_t(net::MessageType::CommandPacket)]) });
	return rows;
}

std::string ServerNetMeter::report(
		std::span<const SlotTraffic> p_slots, std::span<const net::PeerCounters> p_enet, uint64_t p_now_ns) {
	const double seconds = at_ns_ != 0 && p_now_ns > at_ns_ ? double(p_now_ns - at_ns_) * 1e-9 : 0.0;
	std::vector<Seen> next;
	std::string out;
	for (const SlotTraffic &s : p_slots) {
		Seen n;
		n.slot = s.slot;
		n.peer = s.peer;
		n.sent = s.sent;
		n.received = s.received;
		if (s.peer < p_enet.size()) {
			n.enet_sent = enet_total(p_enet[s.peer].sent);
			n.enet_received = enet_total(p_enet[s.peer].received);
		}
		const auto prev = std::find_if(seen_.begin(), seen_.end(),
				[&](const Seen &p_seen) { return p_seen.slot == n.slot && p_seen.peer == n.peer; });
		if (prev != seen_.end() && seconds > 0.0) {
			const NetRates sent = net_rates(prev->sent, n.sent, seconds);
			const NetRates received = net_rates(prev->received, n.received, seconds);
			out += core::fmt("%sslot %u peer %u out %.0f B/s (%s) in %.0f B/s (%s), enet out %.0f B/s in %.0f B/s, "
							 "intervals %llu over %llu max %llu B",
					out.empty() ? "" : "; ", unsigned(n.slot), unsigned(n.peer), double(sent.total_bytes_per_s),
					top_types(sent).c_str(), double(received.total_bytes_per_s), top_types(received).c_str(),
					double(per_second(prev->enet_sent, n.enet_sent, seconds)),
					double(per_second(prev->enet_received, n.enet_received, seconds)),
					(unsigned long long)s.intervals.count, (unsigned long long)s.intervals.over_budget,
					(unsigned long long)s.intervals.max_bytes);
		}
		next.push_back(std::move(n));
	}
	seen_ = std::move(next);
	at_ns_ = p_now_ns;
	return out.empty() ? std::string("no remote peers measured yet") : out;
}

}

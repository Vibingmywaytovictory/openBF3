#include <doctest/doctest.h>

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"
#include "test_support.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>

TEST_CASE("net: ENet version is the pinned 1.3.18") {
	CHECK(bf3::net::enet_version() == "1.3.18");
}

TEST_CASE("net: ping/pong packet round trip") {
	std::vector<uint8_t> p = bf3::net::make_ping(42, 64);
	CHECK(p.size() == 64);
	uint32_t seq = 0;
	uint64_t sent = 0;
	CHECK(!bf3::net::read_pong(p, seq, sent));
	REQUIRE(bf3::net::ping_to_pong(p));
	REQUIRE(bf3::net::read_pong(p, seq, sent));
	CHECK(seq == 42);
	CHECK(!bf3::net::ping_to_pong(p));
}

TEST_CASE("net: loopback ping on 127.0.0.1") {
	const bf3::net::PingStats st = bf3::net::loopback_ping(200);
	MESSAGE("port ", st.port, ", connect ", st.connect_ms, " ms, ", st.received, "/", st.sent, " pongs, RTT mean ", st.mean_us,
			" us, p50 ", st.p50_us, " us, p99 ", st.p99_us, " us, max ", st.max_us, " us, ENet RTT ", st.enet_rtt_ms, " ms");
	CHECK_MESSAGE(st.ok, st.error);
	CHECK(st.received == 200);
	CHECK(st.port != 0);
	CHECK(st.p99_us < 50'000.0);
}

TEST_CASE("net: two servers get distinct free ports") {
	auto a = bf3::net::Host::create_server("127.0.0.1", 0, 4, 2);
	auto b = bf3::net::Host::create_server("127.0.0.1", 0, 4, 2);
	REQUIRE(a);
	REQUIRE(b);
	CHECK(a->bound_port() != 0);
	CHECK(a->bound_port() != b->bound_port());
}

namespace {

using bf3::net::Host;

struct HostPair {
	std::unique_ptr<Host> server;
	std::unique_ptr<Host> client;
	uint32_t server_peer = 0;
	uint32_t client_peer = 0;
	std::vector<Host::Event> server_events;
	std::vector<Host::Event> client_events;

	void service() {
		Host::Event ev;
		while (server->service(1, ev) > 0) {
			server_events.push_back(std::move(ev));
		}
		while (client->service(1, ev) > 0) {
			client_events.push_back(std::move(ev));
		}
	}

	bool until(const std::function<bool()> &p_done) {
		return bf3::test::wait_until(
				[&] {
					service();
					return p_done();
				},
				std::chrono::seconds(10));
	}

	static size_t count(const std::vector<Host::Event> &p_events, Host::EventType p_type) {
		size_t n = 0;
		for (const Host::Event &e : p_events) {
			n += e.type == p_type ? 1 : 0;
		}
		return n;
	}
};

void connect_pair(HostPair &r_pair) {
	std::string err;
	r_pair.server = Host::create_server("127.0.0.1", 0, 4, 3, &err);
	REQUIRE_MESSAGE(r_pair.server, err);
	r_pair.client = Host::create_client(3, &err);
	REQUIRE_MESSAGE(r_pair.client, err);
	const int peer = r_pair.client->connect("127.0.0.1", r_pair.server->bound_port());
	REQUIRE(peer >= 0);
	r_pair.client_peer = uint32_t(peer);
	REQUIRE(r_pair.until([&] {
		return HostPair::count(r_pair.server_events, Host::EventType::Connect) == 1 &&
				HostPair::count(r_pair.client_events, Host::EventType::Connect) == 1;
	}));
	for (const Host::Event &e : r_pair.server_events) {
		if (e.type == Host::EventType::Connect) {
			r_pair.server_peer = e.peer;
		}
	}
	r_pair.server_events.clear();
	r_pair.client_events.clear();
}

std::vector<uint8_t> payload(size_t p_size, uint8_t p_seed) {
	std::vector<uint8_t> v(p_size);
	std::iota(v.begin(), v.end(), p_seed);
	return v;
}

}

TEST_CASE("net: the server disconnects a kicked peer") {
	HostPair pair;
	connect_pair(pair);
	const std::vector<uint8_t> kick = payload(40, 7);
	REQUIRE(pair.server->send(pair.server_peer, 1, kick.data(), kick.size(), Host::SendMode::Reliable));
	REQUIRE(pair.server->disconnect_later(pair.server_peer));
	CHECK_FALSE(pair.server->peer_connected(pair.server_peer));
	CHECK_FALSE(pair.server->send(pair.server_peer, 1, kick.data(), kick.size(), Host::SendMode::Reliable));
	CHECK_FALSE(pair.server->disconnect_later(pair.server_peer));
	REQUIRE(pair.until([&] {
		return HostPair::count(pair.client_events, Host::EventType::Disconnect) == 1 &&
				HostPair::count(pair.server_events, Host::EventType::Disconnect) == 1;
	}));
	REQUIRE(pair.client_events.size() == 2);
	CHECK(pair.client_events[0].type == Host::EventType::Receive);
	CHECK(pair.client_events[0].data == kick);
	CHECK(pair.client_events[1].type == Host::EventType::Disconnect);
	CHECK(pair.server->connected_peers() == 0);
	CHECK(pair.client->connected_peers() == 0);
}

TEST_CASE("net: per-peer counters equal the bytes sent and received on each channel") {
	constexpr uint64_t kSendReliable = 6;
	constexpr uint64_t kSendUnreliable = 8;
	constexpr uint64_t kSendUnsequenced = 8;
	constexpr uint64_t kSendFragment = 24;
	HostPair pair;
	connect_pair(pair);
	const uint64_t fragment = pair.client->max_unfragmented_bytes(pair.client_peer);
	REQUIRE(fragment == 1392 - 4 - kSendFragment);
	REQUIRE(pair.server->max_unfragmented_bytes(pair.server_peer) == fragment);

	struct Sent {
		bool from_client = true;
		uint8_t channel = 0;
		size_t size = 0;
		Host::SendMode mode = Host::SendMode::Unsequenced;
	};
	const Sent sends[] = {
		{ true, 0, 33, Host::SendMode::Unsequenced },
		{ true, 0, 70, Host::SendMode::Unsequenced },
		{ true, 1, 200, Host::SendMode::Reliable },
		{ true, 1, 5000, Host::SendMode::Reliable },
		{ true, 2, 90, Host::SendMode::Sequenced },
		{ false, 0, size_t(fragment), Host::SendMode::Unsequenced },
		{ false, 1, 12, Host::SendMode::Reliable },
		{ false, 1, 3000, Host::SendMode::Reliable },
		{ false, 2, 2800, Host::SendMode::Sequenced },
	};
	std::array<uint64_t, 3> from_client_bytes{}, from_server_bytes{}, from_client_packets{}, from_server_packets{};
	size_t to_server = 0, to_client = 0;
	for (size_t i = 0; i < std::size(sends); ++i) {
		const Sent &s = sends[i];
		const std::vector<uint8_t> bytes = payload(s.size, uint8_t(i));
		Host &from = s.from_client ? *pair.client : *pair.server;
		const uint32_t peer = s.from_client ? pair.client_peer : pair.server_peer;
		REQUIRE(from.send(peer, s.channel, bytes.data(), bytes.size(), s.mode));
		uint64_t wire = 0;
		if (s.size > fragment) {
			wire = s.size + ((s.size + fragment - 1) / fragment) * kSendFragment;
		} else if (s.mode == Host::SendMode::Reliable) {
			wire = s.size + kSendReliable;
		} else if (s.mode == Host::SendMode::Unsequenced) {
			wire = s.size + kSendUnsequenced;
		} else {
			wire = s.size + kSendUnreliable;
		}
		(s.from_client ? from_client_bytes : from_server_bytes)[s.channel] += wire;
		(s.from_client ? from_client_packets : from_server_packets)[s.channel] += 1;
		(s.from_client ? to_server : to_client) += 1;
	}
	pair.client->flush();
	pair.server->flush();
	REQUIRE(pair.until([&] {
		return HostPair::count(pair.server_events, Host::EventType::Receive) == to_server &&
				HostPair::count(pair.client_events, Host::EventType::Receive) == to_client;
	}));

	const bf3::net::PeerCounters client = pair.client->peer_counters(pair.client_peer);
	const bf3::net::PeerCounters server = pair.server->peer_counters(pair.server_peer);
	uint64_t client_sent_sum = 0, server_sent_sum = 0;
	for (size_t c = 0; c < 3; ++c) {
		CAPTURE(c);
		CHECK(client.sent[c].bytes == from_client_bytes[c]);
		CHECK(client.sent[c].packets == from_client_packets[c]);
		CHECK(server.received[c].bytes == from_client_bytes[c]);
		CHECK(server.received[c].packets == from_client_packets[c]);
		CHECK(server.sent[c].bytes == from_server_bytes[c]);
		CHECK(server.sent[c].packets == from_server_packets[c]);
		CHECK(client.received[c].bytes == from_server_bytes[c]);
		CHECK(client.received[c].packets == from_server_packets[c]);
		client_sent_sum += client.sent[c].bytes;
		server_sent_sum += server.sent[c].bytes;
	}
	CHECK(client.sent[3].packets == 0);
	CHECK(server.received[3].packets == 0);
	const bf3::net::WireTotals client_wire = pair.client->wire_totals();
	const bf3::net::WireTotals server_wire = pair.server->wire_totals();
	CHECK(client_wire.sent_bytes >= client_sent_sum + 2 * client_wire.sent_datagrams);
	CHECK(server_wire.sent_bytes >= server_sent_sum + 2 * server_wire.sent_datagrams);
	CHECK(server_wire.received_bytes > client_sent_sum);
	CHECK(server_wire.received_bytes <= client_wire.sent_bytes);
	CHECK(client_wire.received_bytes > server_sent_sum);
}

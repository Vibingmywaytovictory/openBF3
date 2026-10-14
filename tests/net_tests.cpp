// ENet wrapper: loopback ping between two hosts in this process.
#include <doctest/doctest.h>

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"

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

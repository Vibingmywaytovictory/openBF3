#include <doctest/doctest.h>

#include "bf3/host/loopback_client.h"
#include "bf3/host/server_host.h"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("host: ServerHost ticks at 60 Hz and stops cleanly") {
	bf3::host::ServerHost server;
	std::string err;
	REQUIRE_MESSAGE(server.start({}, &err), err);
	CHECK(server.running());
	CHECK(server.port() != 0);
	CHECK(bf3::host::ServerHost::live_count() == 1);

	bf3::host::LoopbackClient client;
	REQUIRE(client.start(server.port(), 20, &err));

	std::this_thread::sleep_for(2000ms);
	const bf3::host::ServerStats s = server.stats();
	const bf3::host::LoopbackClientStats c = client.stats();
	MESSAGE("ticks ", s.ticks, " (", s.measured_hz, " Hz), step last ", s.last_step_ms, " ms p99 ", s.p99_step_ms, " ms max ",
			s.max_step_ms, " ms, overruns ", s.overruns, "; client pongs ", c.pongs_received, "/", c.pings_sent, " mean RTT ", c.mean_rtt_ms,
			" ms");
	CHECK(s.ticks >= 110);
	CHECK(s.ticks <= 130);
	CHECK(s.measured_hz == doctest::Approx(60.0).epsilon(0.05));
	CHECK(s.step_errors == 0);
	CHECK(s.peers == 1);
	CHECK(c.connected);
	CHECK(c.pongs_received > 10);

	client.stop();
	server.stop();
	CHECK(!server.running());
	CHECK(bf3::host::ServerHost::live_count() == 0);
	const uint64_t ticks = server.stats().ticks;
	std::this_thread::sleep_for(100ms);
	CHECK(server.stats().ticks == ticks);
	server.stop();
}

TEST_CASE("host: stop_all joins every running server") {
	bf3::host::ServerHost a;
	bf3::host::ServerHost b;
	bf3::host::ServerConfig no_net;
	no_net.enable_net = false;
	REQUIRE(a.start(no_net));
	REQUIRE(b.start(no_net));
	std::this_thread::sleep_for(100ms);
	CHECK(bf3::host::ServerHost::stop_all() == 2);
	CHECK(!a.running());
	CHECK(!b.running());
	CHECK(bf3::host::ServerHost::live_count() == 0);
}

#include <doctest/doctest.h>

#include "bf3/host/loopback_client.h"
#include "bf3/host/server_host.h"
#include "bf3/host/tick_clock.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace {

class SteppedClock final : public bf3::host::TickClock {
public:
	int64_t now_ns() override {
		std::lock_guard lock(mutex_);
		return now_;
	}

	void sleep_until_ns(int64_t p_deadline_ns) override {
		std::unique_lock lock(mutex_);
		parked_at_ = p_deadline_ns;
		changed_.notify_all();
		changed_.wait(lock, [&] { return p_deadline_ns <= limit_; });
		now_ = std::max(now_, p_deadline_ns);
		parked_at_ = kNotParked;
	}

	void run_until(int64_t p_limit_ns) {
		std::lock_guard lock(mutex_);
		limit_ = std::max(limit_, p_limit_ns);
		changed_.notify_all();
	}

	void stall_to(int64_t p_now_ns) {
		std::lock_guard lock(mutex_);
		now_ = std::max(now_, p_now_ns);
		limit_ = std::max(limit_, p_now_ns);
		changed_.notify_all();
	}

	bool parked_after(int64_t p_ns) {
		std::unique_lock lock(mutex_);
		return changed_.wait_for(lock, 10s, [&] { return parked_at_ > p_ns; });
	}

	void release() { run_until(std::numeric_limits<int64_t>::max()); }

private:
	static constexpr int64_t kNotParked = std::numeric_limits<int64_t>::min();
	std::mutex mutex_;
	std::condition_variable changed_;
	int64_t now_ = 0;
	int64_t limit_ = 0;
	int64_t parked_at_ = kNotParked;
};

}

TEST_CASE("host: ServerHost paces its ticks by its clock: one tick per period, an overrun resets the schedule") {
	const auto clock = std::make_shared<SteppedClock>();
	bf3::host::ServerConfig config;
	config.tick_hz = 60;
	config.enable_net = false;
	config.clock = clock;
	bf3::host::ServerHost server;
	REQUIRE(server.start(config));

	const int64_t kPeriodNs = 1'000'000'000LL / config.tick_hz;
	constexpr int64_t kTwoSecondsNs = 2'000'000'000LL;
	clock->run_until(kTwoSecondsNs);
	REQUIRE(clock->parked_after(kTwoSecondsNs));
	bf3::host::ServerStats s = server.stats();
	MESSAGE("ticks ", s.ticks, " (", s.measured_hz, " Hz), overruns ", s.overruns);
	CHECK(s.ticks == 121);
	CHECK(s.measured_hz == doctest::Approx(121.0 / (120.0 * double(kPeriodNs) / 1e9)));
	CHECK(s.overruns == 0);
	CHECK(s.step_errors == 0);

	const int64_t stalled_to = kTwoSecondsNs + 10 * kPeriodNs;
	clock->stall_to(stalled_to);
	REQUIRE(clock->parked_after(stalled_to));
	s = server.stats();
	CHECK(s.ticks == 123);
	CHECK(s.overruns == 1);

	clock->run_until(stalled_to + 60 * kPeriodNs);
	REQUIRE(clock->parked_after(stalled_to + 60 * kPeriodNs));
	s = server.stats();
	CHECK(s.ticks == 183);
	CHECK(s.overruns == 1);

	clock->release();
	server.stop();
	CHECK(!server.running());
}

TEST_CASE("host: ServerHost answers a loopback client and stops cleanly") {
	bf3::host::ServerHost server;
	std::string err;
	REQUIRE_MESSAGE(server.start({}, &err), err);
	CHECK(server.running());
	CHECK(server.port() != 0);
	CHECK(bf3::host::ServerHost::live_count() == 1);

	bf3::host::LoopbackClient client;
	REQUIRE(client.start(server.port(), 20, &err));

	const bool answered = bf3::test::wait_until([&] { return client.stats().pongs_received > 10; }, 10s, 20ms);
	const bf3::host::ServerStats s = server.stats();
	const bf3::host::LoopbackClientStats c = client.stats();
	MESSAGE("ticks ", s.ticks, " (", s.measured_hz, " Hz), step last ", s.last_step_ms, " ms p99 ", s.p99_step_ms,
			" ms max ", s.max_step_ms, " ms, overruns ", s.overruns, "; client pongs ", c.pongs_received, "/",
			c.pings_sent, " mean RTT ", c.mean_rtt_ms, " ms");
	CHECK(answered);
	CHECK(s.ticks > 0);
	CHECK(s.step_errors == 0);
	CHECK(s.peers == 1);
	CHECK(c.connected);

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

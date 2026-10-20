#include <doctest/doctest.h>

#include "instance_registry.h"

#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/loopback_client.h"
#include "bf3/host/server_host.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using bf3::host::detail::InstanceRegistry;

namespace {

struct Probe {
	explicit Probe(InstanceRegistry<Probe> &r_registry) :
			registry(r_registry) {}
	~Probe() {
		registry.retire(this);
		alive.store(0, std::memory_order_release);
		destroyed.store(true, std::memory_order_release);
	}
	void stop() {
		CHECK(alive.load(std::memory_order_acquire) == kAlive);
		stops.fetch_add(1, std::memory_order_acq_rel);
		if (on_stop) {
			on_stop();
		}
		registry.remove(this);
	}

	static constexpr uint32_t kAlive = 0xA11CE5u;
	InstanceRegistry<Probe> &registry;
	std::atomic<uint32_t> alive{ kAlive };
	std::atomic<int> stops{ 0 };
	std::atomic<bool> destroyed{ false };
	std::function<void()> on_stop;
};

bool wait_for(const std::atomic<bool> &p_flag, std::chrono::milliseconds p_timeout) {
	const auto end = std::chrono::steady_clock::now() + p_timeout;
	while (!p_flag.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > end) {
			return false;
		}
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

bool wait_until(const std::function<bool()> &p_done, std::chrono::milliseconds p_timeout) {
	const auto end = std::chrono::steady_clock::now() + p_timeout;
	while (!p_done()) {
		if (std::chrono::steady_clock::now() > end) {
			return false;
		}
		std::this_thread::yield();
	}
	return true;
}

size_t churn(int p_owners, int p_iterations, const std::function<void(int, int)> &p_round, const std::function<size_t()> &p_stop_all,
		size_t *r_stop_all_calls) {
	std::atomic<bool> done{ false };
	std::atomic<size_t> stopped{ 0 };
	std::atomic<size_t> calls{ 0 };
	std::thread stopper([&] {
		while (!done.load(std::memory_order_acquire)) {
			stopped.fetch_add(p_stop_all(), std::memory_order_relaxed);
			calls.fetch_add(1, std::memory_order_relaxed);
			std::this_thread::yield();
		}
	});
	std::vector<std::thread> owners;
	for (int t = 0; t < p_owners; ++t) {
		owners.emplace_back([&, t] {
			for (int i = 0; i < p_iterations; ++i) {
				p_round(t, i);
			}
		});
	}
	for (std::thread &o : owners) {
		o.join();
	}
	done.store(true, std::memory_order_release);
	stopper.join();
	if (r_stop_all_calls) {
		*r_stop_all_calls = calls.load();
	}
	return stopped.load();
}

enum class JitterMode {

	Spin,

	SpinSomeTicks,
};

void jitter(int p_thread, int p_iteration, JitterMode p_mode) {
	const int us = int((uint32_t(p_thread) * 2654435761u + uint32_t(p_iteration) * 40503u) % 700u);
	if (us > 350) {
		const std::chrono::microseconds pause(us - 350);
		if (p_mode == JitterMode::SpinSomeTicks && (p_thread + p_iteration) % 4 == 0) {
			std::this_thread::sleep_for(pause);
			return;
		}
		const auto end = std::chrono::steady_clock::now() + pause;
		while (std::chrono::steady_clock::now() < end) {
			std::this_thread::yield();
		}
	} else if (us > 100) {
		std::this_thread::yield();
	}
}

std::shared_ptr<const bf3::host::SharedLevel> floor_level() {
	std::vector<bf3::sim::StaticTriangle> t;
	for (float x = -32; x < 32; x += 8) {
		for (float z = -32; z < 32; z += 8) {
			bf3::sim::StaticTriangle a;
			a.v = { bf3::sim::Vec3{ x, 0, z }, bf3::sim::Vec3{ x, 0, z + 8 }, bf3::sim::Vec3{ x + 8, 0, z + 8 } };
			t.push_back(a);
			a.v = { bf3::sim::Vec3{ x, 0, z }, bf3::sim::Vec3{ x + 8, 0, z + 8 }, bf3::sim::Vec3{ x + 8, 0, z } };
			t.push_back(a);
		}
	}
	bf3::core::Result<std::shared_ptr<const bf3::sim::StaticWorldShapes>> shapes = bf3::sim::build_static_world(std::span<const bf3::sim::StaticTriangle>(t));
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	bf3::sim::SpawnPoint p;
	p.position = { 0, 0.05f, 0 };
	bf3::sim::MotorParams motor;
	const float heights[] = { 1.8f, 1.15f, 0.3f };
	const float eyes[] = { 1.6f, 1.0f, 0.3f };
	for (size_t i = 0; i < bf3::sim::kPoseCount; ++i) {
		motor.poses[i].height = heights[i];
		motor.poses[i].eye_height = eyes[i];
		motor.poses[i].step_height = 0.3f;
	}
	return bf3::host::synthetic_shared_level(*shapes, { p }, motor);
}

}

TEST_SUITE("host-registry") {
	TEST_CASE("host.registry: a destructor waits while stop_all is inside its instance, then the instance is gone") {
		InstanceRegistry<Probe> registry;
		auto probe = std::make_unique<Probe>(registry);
		Probe *raw = probe.get();
		std::atomic<bool> inside{ false }, release{ false };
		probe->on_stop = [&] {
			inside.store(true, std::memory_order_release);
			wait_for(release, 10s);
		};
		registry.add(raw);
		size_t called = 0;
		std::thread stop_all([&] { called = registry.for_each_pinned([](Probe *p) { p->stop(); }); });
		REQUIRE(wait_for(inside, 10s));
		std::atomic<bool> deleted{ false };
		std::thread owner([&] {
			probe.reset();
			deleted.store(true, std::memory_order_release);
		});

		REQUIRE(wait_until([&] { return registry.size() == 0; }, 10s));
		std::this_thread::sleep_for(20ms);
		CHECK(!deleted.load());
		CHECK(!raw->destroyed.load());
		release.store(true, std::memory_order_release);
		stop_all.join();
		owner.join();
		CHECK(deleted.load());
		CHECK(called == 1);
		CHECK(registry.size() == 0);
	}

	TEST_CASE("host.registry: an instance retired before stop_all reaches it is skipped, not called") {
		InstanceRegistry<Probe> registry;
		auto first = std::make_unique<Probe>(registry);
		auto second = std::make_unique<Probe>(registry);
		Probe *second_raw = second.get();
		std::atomic<bool> inside{ false }, release{ false };
		first->on_stop = [&] {
			inside.store(true, std::memory_order_release);
			wait_for(release, 10s);
		};
		std::atomic<int> second_stops{ 0 };
		second->on_stop = [&] { second_stops.fetch_add(1); };
		registry.add(first.get());
		registry.add(second_raw);
		size_t called = 0;
		std::thread stop_all([&] { called = registry.for_each_pinned([](Probe *p) { p->stop(); }); });
		REQUIRE(wait_for(inside, 10s));
		std::atomic<bool> deleted{ false };
		std::thread owner([&] {
			second.reset();
			deleted.store(true, std::memory_order_release);
		});

		REQUIRE(wait_until([&] { return registry.size() == 1; }, 10s));
		std::this_thread::sleep_for(20ms);
		CHECK(!deleted.load());
		release.store(true, std::memory_order_release);
		stop_all.join();
		owner.join();
		CHECK(deleted.load());
		CHECK(called == 1);
		CHECK(second_stops.load() == 0);
		CHECK(first->stops.load() == 1);
		CHECK(registry.size() == 0);
	}

	TEST_CASE("host.registry: 8 threads destroy 4,000 probes while 2 threads run stop_all") {
		InstanceRegistry<Probe> registry;
		std::atomic<bool> done{ false };
		std::atomic<size_t> stopped{ 0 };
		std::vector<std::thread> stoppers;
		for (int s = 0; s < 2; ++s) {
			stoppers.emplace_back([&] {
				while (!done.load(std::memory_order_acquire)) {
					stopped.fetch_add(registry.for_each_pinned([](Probe *p) { p->stop(); }));
				}
			});
		}
		std::vector<std::thread> owners;
		for (int t = 0; t < 8; ++t) {
			owners.emplace_back([&, t] {
				for (int i = 0; i < 500; ++i) {
					auto p = std::make_unique<Probe>(registry);
					registry.add(p.get());
					jitter(t, i, JitterMode::Spin);
					if (i % 3 == 0) {
						p->stop();
					}
				}
			});
		}
		for (std::thread &o : owners) {
			o.join();
		}
		done.store(true, std::memory_order_release);
		for (std::thread &s : stoppers) {
			s.join();
		}
		MESSAGE("probes stopped by stop_all: ", stopped.load(), " of 4000");
		CHECK(stopped.load() > 0);
		CHECK(registry.size() == 0);
	}

	TEST_CASE("host.registry: ServerHost::stop_all while owners start and destroy hosts") {
		bf3::host::ServerConfig config;
		config.enable_net = false;
		config.world.dynamic_bodies = 2;
		config.world.job_threads = 0;
		size_t calls = 0;
		const size_t stopped = churn(
				4, 40,
				[&](int p_thread, int p_iteration) {
					auto host = std::make_unique<bf3::host::ServerHost>();
					std::string err;
					REQUIRE_MESSAGE(host->start(config, &err), err);
					jitter(p_thread, p_iteration, JitterMode::SpinSomeTicks);
					if (p_iteration % 4 == 1) {
						host->stop();
					}
					host.reset();
				},
				[] { return bf3::host::ServerHost::stop_all(); }, &calls);
		MESSAGE("ServerHost: ", stopped, " of 160 stopped by stop_all over ", calls, " calls");
		CHECK(calls > 0);
		CHECK(bf3::host::ServerHost::live_count() == 0);
	}

	TEST_CASE("host.registry: LoopbackClient::stop_all while owners start and destroy clients") {
		bf3::host::ServerHost server;
		std::string err;
		REQUIRE_MESSAGE(server.start({}, &err), err);
		size_t calls = 0;
		const size_t stopped = churn(
				4, 40,
				[&](int p_thread, int p_iteration) {
					auto client = std::make_unique<bf3::host::LoopbackClient>();
					std::string e;
					REQUIRE_MESSAGE(client->start(server.port(), 100, &e), e);
					jitter(p_thread, p_iteration, JitterMode::SpinSomeTicks);
					client.reset();
				},
				[] { return bf3::host::LoopbackClient::stop_all(); }, &calls);
		MESSAGE("LoopbackClient: ", stopped, " of 160 stopped by stop_all over ", calls, " calls");
		CHECK(calls > 0);
		CHECK(bf3::host::LoopbackClient::stop_all() == 0);
		server.stop();
	}

	TEST_CASE("host.registry: ClientSession::stop_all while owners connect and destroy sessions") {
		const std::shared_ptr<const bf3::host::SharedLevel> level = floor_level();
		auto game = std::make_shared<bf3::host::GameServerConfig>();
		game->level = level;
		game->max_slots = 64;
		bf3::host::ServerConfig sc;
		sc.game = game;
		bf3::host::ServerHost server;
		std::string err;
		REQUIRE_MESSAGE(server.start(sc, &err), err);
		size_t calls = 0;
		std::atomic<int> started{ 0 };
		const size_t stopped = churn(
				3, 32,
				[&](int p_thread, int p_iteration) {
					auto session = std::make_unique<bf3::host::ClientSession>();
					bf3::host::ClientSessionConfig cc;
					cc.port = server.port();
					cc.name = "churn" + std::to_string(p_thread);
					const bf3::core::Status s = session->start(cc, level);
					REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
					started.fetch_add(1);
					jitter(p_thread, p_iteration, JitterMode::SpinSomeTicks);
					session.reset();
				},
				[] { return bf3::host::ClientSession::stop_all(); }, &calls);
		MESSAGE("ClientSession: ", stopped, " of ", started.load(), " stopped by stop_all over ", calls, " calls");
		CHECK(calls > 0);
		CHECK(started.load() == 96);
		CHECK(bf3::host::ClientSession::stop_all() == 0);
		server.stop();
	}
}

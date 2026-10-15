#include "bf3/host/loopback_client.h"

#include "instance_registry.h"

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"

#include <algorithm>
#include <vector>

namespace bf3::host {

namespace {
// H hygiene: pins instances against their destructor while stop_all is inside them (instance_registry.h).
detail::InstanceRegistry<LoopbackClient> g_clients;
} // namespace

LoopbackClient::~LoopbackClient() {
	g_clients.retire(this); // first: a concurrent stop_all is done with this instance once it returns
	stop();
}

bool LoopbackClient::start(uint16_t p_port, int p_ping_hz, std::string *r_error) {
	std::lock_guard lock(lifecycle_mutex_);
	if (thread_.joinable()) {
		if (r_error) {
			*r_error = "already running";
		}
		return false;
	}
	if (p_port == 0) {
		if (r_error) {
			*r_error = "no server port";
		}
		return false;
	}
	sent_ = received_ = rtt_sum_us_ = 0;
	last_rtt_us_ = max_rtt_us_ = enet_rtt_ms_ = 0;
	connected_ = false;
	stop_requested_ = false;
	running_ = true;
	thread_ = std::thread([this, p_port, p_ping_hz] { run(p_port, std::clamp(p_ping_hz, 1, 1000)); });
	g_clients.add(this);
	return true;
}

void LoopbackClient::stop() {
	std::lock_guard lock(lifecycle_mutex_);
	if (!thread_.joinable()) {
		return;
	}
	stop_requested_ = true;
	thread_.join();
	running_ = false;
	connected_ = false;
	g_clients.remove(this);
}

size_t LoopbackClient::stop_all() {
	return g_clients.for_each_pinned([](LoopbackClient *p_client) { p_client->stop(); });
}

LoopbackClientStats LoopbackClient::stats() const {
	LoopbackClientStats s;
	s.running = running_.load();
	s.connected = connected_.load();
	s.pings_sent = sent_.load();
	s.pongs_received = received_.load();
	s.last_rtt_ms = last_rtt_us_.load() / 1000.0;
	s.max_rtt_ms = max_rtt_us_.load() / 1000.0;
	s.mean_rtt_ms = s.pongs_received ? (static_cast<double>(rtt_sum_us_.load()) / static_cast<double>(s.pongs_received)) / 1000.0 : 0.0;
	s.enet_rtt_ms = enet_rtt_ms_.load();
	return s;
}

void LoopbackClient::run(uint16_t p_port, int p_ping_hz) {
	std::unique_ptr<net::Host> host = net::Host::create_client(2);
	if (!host) {
		return;
	}
	const int peer = host->connect("127.0.0.1", p_port);
	if (peer < 0) {
		return;
	}
	const uint64_t interval_ns = 1'000'000'000ull / static_cast<uint64_t>(p_ping_hz);
	uint64_t next_ping = net::monotonic_ns();
	uint32_t seq = 0;
	while (!stop_requested_.load(std::memory_order_acquire)) {
		net::Host::Event ev;
		// Short waits keep the thread responsive to stop() and timestamps precise.
		while (host->service(1, ev) > 0) {
			if (ev.type == net::Host::EventType::Connect) {
				connected_ = true;
			} else if (ev.type == net::Host::EventType::Disconnect) {
				connected_ = false;
			} else if (ev.type == net::Host::EventType::Receive) {
				uint32_t s = 0;
				uint64_t sent = 0;
				if (net::read_pong(ev.data, s, sent)) {
					const uint32_t rtt_us = static_cast<uint32_t>((net::monotonic_ns() - sent) / 1000);
					last_rtt_us_ = rtt_us;
					max_rtt_us_ = std::max(max_rtt_us_.load(), rtt_us);
					rtt_sum_us_ += rtt_us;
					received_++;
				}
			}
		}
		const uint64_t now = net::monotonic_ns();
		if (connected_ && now >= next_ping) {
			std::vector<uint8_t> ping = net::make_ping(seq++, 64);
			if (host->send(static_cast<uint32_t>(peer), 0, ping.data(), ping.size(), false)) {
				sent_++;
			}
			host->flush();
			next_ping = now + interval_ns;
			enet_rtt_ms_ = host->peer_rtt_ms(static_cast<uint32_t>(peer));
		}
	}
	host->disconnect_all();
}

} // namespace bf3::host

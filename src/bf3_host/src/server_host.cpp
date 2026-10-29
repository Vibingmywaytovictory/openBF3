#include "bf3/host/server_host.h"

#include "instance_registry.h"
#include "server_net_thread.h"

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"

#include <algorithm>
#include <chrono>

namespace bf3::host {

namespace {

detail::InstanceRegistry<ServerHost> g_registry;

void registry_add(ServerHost *p_host) {
	g_registry.add(p_host);
}

void registry_remove(ServerHost *p_host) {
	g_registry.remove(p_host);
}

net::Host::SendMode send_mode(const OutDatagram &p_datagram) {
	if (p_datagram.reliable) {
		return net::Host::SendMode::Reliable;
	}
	return p_datagram.channel == net::kChannelUnreliable ? net::Host::SendMode::Unsequenced
														 : net::Host::SendMode::Sequenced;
}

bool is_kick(std::span<const uint8_t> p_bytes) {
	return net::peek_type(p_bytes) == net::MessageType::Kick;
}
}

ServerHost::ServerHost() = default;

ServerHost::~ServerHost() {
	g_registry.retire(this);
	stop();
}

bool ServerHost::start(const ServerConfig &p_config, std::string *r_error) {
	std::lock_guard lock(lifecycle_mutex_);
	if (thread_.joinable()) {
		if (r_error) {
			*r_error = "already running";
		}
		return false;
	}
	config_ = p_config;
	config_.tick_hz = std::clamp(config_.tick_hz, 1, 1000);
	if (!config_.clock) {
		config_.clock = make_steady_tick_clock();
	}
	clock_.store(config_.clock, std::memory_order_release);

	if (config_.game) {
		config_.tick_hz = int(sim::kSimHz);
		game_ = std::make_unique<GameServer>();
		const core::Status s = game_->init(*config_.game);
		if (!s.ok()) {
			game_.reset();
			if (r_error) {
				*r_error = s.error().message;
			}
			return false;
		}
		dynamic_bodies_ = 0;
		if (!start_net(net::kChannelCount, r_error)) {
			game_.reset();
			return false;
		}
		ticks_ = 0;
		last_step_us_ = p99_step_us_ = max_step_us_ = last_tick_us_ = 0;
		overruns_ = pings_echoed_ = step_errors_ = tick_stalls_ = net_errors_ = 0;
		peers_ = 0;
		state_hash_ = 0;
		{
			std::lock_guard gl(game_stats_mutex_);
			game_stats_ = game_->stats();
		}
		stop_requested_.store(false, std::memory_order_release);
		running_.store(true, std::memory_order_release);
		start_ns_ = config_.clock->now_ns();
		thread_ = std::thread([this] { run(); });
		registry_add(this);
		return true;
	}

	world_ = std::make_unique<sim::PhysicsWorld>();
	if (!world_->init(config_.world)) {
		world_.reset();
		if (r_error) {
			*r_error = "physics world init failed";
		}
		return false;
	}
	dynamic_bodies_ = world_->dynamic_body_count();
	if (!start_net(2, r_error)) {
		world_.reset();
		return false;
	}

	ticks_ = 0;
	last_step_us_ = p99_step_us_ = max_step_us_ = last_tick_us_ = 0;
	overruns_ = pings_echoed_ = step_errors_ = tick_stalls_ = net_errors_ = 0;
	peers_ = 0;
	state_hash_ = world_->state_hash();
	{
		std::lock_guard tl(transforms_mutex_);
		world_->get_dynamic_transforms(transforms_);
	}
	stop_requested_.store(false, std::memory_order_release);
	running_.store(true, std::memory_order_release);
	start_ns_ = config_.clock->now_ns();
	thread_ = std::thread([this] { run(); });
	registry_add(this);
	return true;
}

bool ServerHost::start_net(size_t p_channels, std::string *r_error) {
	std::shared_ptr<ServerNetPort> port = config_.transport;
	if (port == nullptr && config_.enable_net) {
		std::string err;
		std::unique_ptr<net::Host> host =
				net::Host::create_server(config_.bind_ip, config_.port, config_.max_peers, p_channels, &err);
		if (!host) {
			if (r_error) {
				*r_error = err;
			}
			return false;
		}
		port = make_enet_port(std::move(host), { config_.peer_timeout_min_ms, config_.peer_timeout_max_ms });
	}
	if (port == nullptr) {
		net_.reset();
		return true;
	}
	net_ = std::make_unique<ServerNetThread>(std::move(port));
	port_.store(net_->bound_port(), std::memory_order_release);
	net_->start();
	return true;
}

void ServerHost::stop() {
	std::lock_guard lock(lifecycle_mutex_);
	if (!thread_.joinable()) {
		return;
	}
	stop_requested_.store(true, std::memory_order_release);
	thread_.join();
	registry_remove(this);
	if (net_) {
		net_->stop();
	}
	net_.reset();
	world_.reset();
	game_.reset();
	port_.store(0, std::memory_order_release);
	running_.store(false, std::memory_order_release);
}

size_t ServerHost::stop_all() {
	return g_registry.for_each_pinned([](ServerHost *p_host) { p_host->stop(); });
}

size_t ServerHost::live_count() {
	return g_registry.size();
}

ServerStats ServerHost::stats() const {
	ServerStats s;
	s.running = running();
	s.ticks = ticks_.load(std::memory_order_acquire);
	s.last_step_ms = last_step_us_.load(std::memory_order_relaxed) / 1000.0;
	s.p99_step_ms = p99_step_us_.load(std::memory_order_relaxed) / 1000.0;
	s.max_step_ms = max_step_us_.load(std::memory_order_relaxed) / 1000.0;
	s.last_tick_ms = last_tick_us_.load(std::memory_order_relaxed) / 1000.0;
	s.overruns = overruns_.load(std::memory_order_relaxed);
	s.state_hash = state_hash_.load(std::memory_order_relaxed);
	s.dynamic_bodies = dynamic_bodies_;
	s.active_bodies = active_bodies_.load(std::memory_order_relaxed);
	s.lowest_body_y = lowest_body_y_.load(std::memory_order_relaxed);
	s.port = port();
	s.peers = peers_.load(std::memory_order_relaxed);
	s.pings_echoed = pings_echoed_.load(std::memory_order_relaxed);
	s.step_errors = step_errors_.load(std::memory_order_relaxed) + net_errors_.load(std::memory_order_relaxed);
	s.tick_stalls = tick_stalls_.load(std::memory_order_relaxed);
	const std::shared_ptr<TickClock> clock = clock_.load(std::memory_order_acquire);
	const int64_t start = start_ns_.load(std::memory_order_relaxed);
	const double secs = clock ? static_cast<double>(clock->now_ns() - start) / 1e9 : 0.0;
	s.measured_hz = (s.running && secs > 0.0) ? static_cast<double>(s.ticks) / secs : 0.0;
	return s;
}

void ServerHost::copy_body_transforms(std::vector<sim::BodyTransform> &r_out) const {
	std::lock_guard lock(transforms_mutex_);
	r_out = transforms_;
}

GameServerStats ServerHost::game_stats() const {
	std::lock_guard lock(game_stats_mutex_);
	return game_stats_;
}

std::vector<net::PeerCounters> ServerHost::peer_counters() const {
	std::lock_guard lock(lifecycle_mutex_);
	return net_ ? net_->peer_counters() : std::vector<net::PeerCounters>();
}

void ServerHost::debug_stall(uint32_t p_ms) {
	stall_request_ms_.store(p_ms, std::memory_order_release);
}

void ServerHost::stall_if_requested() {
	if (const uint32_t stall = stall_request_ms_.exchange(0, std::memory_order_acq_rel); stall > 0) {
		tick_stalls_.fetch_add(1, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::milliseconds(stall));
	}
}

void ServerHost::run() {
	TickClock &tick_clock = *config_.clock;
	const int64_t period_ns = 1'000'000'000LL / config_.tick_hz;
	std::vector<uint32_t> window(600, 0);
	std::vector<uint32_t> sorted;
	size_t window_fill = 0;
	uint32_t max_us = 0;
	std::vector<sim::BodyTransform> scratch;
	int64_t next = tick_clock.now_ns();
	std::vector<net::Host::Event> events;
	std::vector<NetOut> sends;

	while (!stop_requested_.load(std::memory_order_acquire)) {
		stall_if_requested();
		const int64_t tick_start = tick_clock.now_ns();
		if (net_) {
			net_->pump();
			net_->take_events(events);
			net_errors_.store(net_->errors() + net_->inbound_dropped(), std::memory_order_relaxed);
		}

		if (game_) {
			for (const net::Host::Event &ev : events) {
				if (ev.type == net::Host::EventType::Connect) {
					game_->on_connect(ev.peer);
				} else if (ev.type == net::Host::EventType::Disconnect) {
					game_->on_disconnect(ev.peer);
				} else if (ev.type == net::Host::EventType::Receive) {
					game_->on_receive(ev.peer, ev.channel, ev.data);
				}
			}
			const int64_t step_start = tick_clock.now_ns();
			std::vector<OutDatagram> out;
			game_->tick(out);
			const uint32_t step_us = static_cast<uint32_t>((tick_clock.now_ns() - step_start) / 1000);
			if (net_) {
				for (OutDatagram &d : out) {
					const bool kick = is_kick(d.bytes);
					sends.push_back({ d.peer, d.channel, send_mode(d), kick, std::move(d.bytes) });
				}
				net_->send(sends);
				peers_.store(net_->connected_peers(), std::memory_order_relaxed);
			}
			{
				GameServerStats g = game_->stats();
				std::lock_guard gl(game_stats_mutex_);
				game_stats_ = std::move(g);
			}
			window[window_fill % window.size()] = step_us;
			window_fill++;
			max_us = std::max(max_us, step_us);
			if (window_fill % 60 == 0) {
				const size_t n = std::min(window_fill, window.size());
				sorted.assign(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(n));
				const size_t k = std::min(n - 1, (n * 99) / 100);
				std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
				p99_step_us_.store(sorted[k], std::memory_order_relaxed);
			}
			last_step_us_.store(step_us, std::memory_order_relaxed);
			max_step_us_.store(max_us, std::memory_order_relaxed);
			last_tick_us_.store(
					static_cast<uint32_t>((tick_clock.now_ns() - tick_start) / 1000), std::memory_order_relaxed);
			ticks_.fetch_add(1, std::memory_order_release);
			next += period_ns;
			const int64_t now = tick_clock.now_ns();
			if (now > next + 5 * period_ns) {
				overruns_.fetch_add(1, std::memory_order_relaxed);
				next = now;
			}
			tick_clock.sleep_until_ns(next);
			continue;
		}

		if (net_) {
			for (net::Host::Event &ev : events) {
				if (ev.type == net::Host::EventType::Receive && net::ping_to_pong(ev.data)) {
					sends.push_back({ ev.peer, ev.channel, net::Host::SendMode::Sequenced, false, std::move(ev.data) });
					pings_echoed_.fetch_add(1, std::memory_order_relaxed);
				}
			}
			net_->send(sends);
			peers_.store(net_->connected_peers(), std::memory_order_relaxed);
		}

		const int64_t step_start = tick_clock.now_ns();
		if (!world_->step()) {
			step_errors_.fetch_add(1, std::memory_order_relaxed);
		}
		const uint32_t step_us = static_cast<uint32_t>((tick_clock.now_ns() - step_start) / 1000);

		state_hash_.store(world_->state_hash(), std::memory_order_relaxed);
		active_bodies_.store(world_->active_body_count(), std::memory_order_relaxed);
		lowest_body_y_.store(world_->lowest_dynamic_y(), std::memory_order_relaxed);
		world_->get_dynamic_transforms(scratch);
		{
			std::lock_guard lock(transforms_mutex_);
			transforms_.swap(scratch);
		}
		window[window_fill % window.size()] = step_us;
		window_fill++;
		max_us = std::max(max_us, step_us);
		if (window_fill % 60 == 0) {
			const size_t n = std::min(window_fill, window.size());
			sorted.assign(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(n));
			const size_t k = std::min(n - 1, (n * 99) / 100);
			std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
			p99_step_us_.store(sorted[k], std::memory_order_relaxed);
		}
		last_step_us_.store(step_us, std::memory_order_relaxed);
		max_step_us_.store(max_us, std::memory_order_relaxed);
		last_tick_us_.store(
				static_cast<uint32_t>((tick_clock.now_ns() - tick_start) / 1000), std::memory_order_relaxed);
		ticks_.fetch_add(1, std::memory_order_release);

		next += period_ns;
		const int64_t now = tick_clock.now_ns();
		if (now > next + 5 * period_ns) {
			overruns_.fetch_add(1, std::memory_order_relaxed);
			next = now;
		}
		tick_clock.sleep_until_ns(next);
	}
}

}

#include "bf3/host/server_host.h"

#include "tick_timer.h"

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"

#include <algorithm>
#include <chrono>

namespace bf3::host {

// ENet endpoint of the server (kept out of the public header).
class NetEndpoint {
public:
	std::unique_ptr<net::Host> host;
};

namespace {

std::mutex g_registry_mutex;
std::vector<ServerHost *> g_registry;

void registry_add(ServerHost *p_host) {
	std::lock_guard lock(g_registry_mutex);
	g_registry.push_back(p_host);
}

void registry_remove(ServerHost *p_host) {
	std::lock_guard lock(g_registry_mutex);
	g_registry.erase(std::remove(g_registry.begin(), g_registry.end(), p_host), g_registry.end());
}

int64_t now_ns() {
	return static_cast<int64_t>(net::monotonic_ns());
}

} // namespace

ServerHost::ServerHost() = default;

ServerHost::~ServerHost() {
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

	// Build the world and bind the socket on the caller's thread so errors surface here; both
	// are handed to the server thread and touched only there afterwards.
	world_ = std::make_unique<sim::PhysicsWorld>();
	if (!world_->init(config_.world)) {
		world_.reset();
		if (r_error) {
			*r_error = "physics world init failed";
		}
		return false;
	}
	dynamic_bodies_ = world_->dynamic_body_count();
	net_ = std::make_unique<NetEndpoint>();
	if (config_.enable_net) {
		std::string err;
		net_->host = net::Host::create_server(config_.bind_ip, config_.port, config_.max_peers, 2, &err);
		if (!net_->host) {
			world_.reset();
			net_.reset();
			if (r_error) {
				*r_error = err;
			}
			return false;
		}
		port_.store(net_->host->bound_port(), std::memory_order_release);
	}

	ticks_ = 0;
	last_step_us_ = p99_step_us_ = max_step_us_ = last_tick_us_ = 0;
	overruns_ = pings_echoed_ = step_errors_ = 0;
	peers_ = 0;
	state_hash_ = world_->state_hash();
	{
		std::lock_guard tl(transforms_mutex_);
		world_->get_dynamic_transforms(transforms_);
	}
	stop_requested_.store(false, std::memory_order_release);
	running_.store(true, std::memory_order_release);
	start_ns_ = now_ns();
	thread_ = std::thread([this] { run(); });
	registry_add(this);
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
	if (net_ && net_->host) {
		net_->host->disconnect_all();
	}
	net_.reset();
	world_.reset();
	port_.store(0, std::memory_order_release);
	running_.store(false, std::memory_order_release);
}

size_t ServerHost::stop_all() {
	std::vector<ServerHost *> hosts;
	{
		std::lock_guard lock(g_registry_mutex);
		hosts = g_registry;
	}
	for (ServerHost *h : hosts) {
		h->stop();
	}
	return hosts.size();
}

size_t ServerHost::live_count() {
	std::lock_guard lock(g_registry_mutex);
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
	s.step_errors = step_errors_.load(std::memory_order_relaxed);
	const int64_t start = start_ns_.load(std::memory_order_relaxed);
	const double secs = static_cast<double>(now_ns() - start) / 1e9;
	s.measured_hz = (s.running && secs > 0.0) ? static_cast<double>(s.ticks) / secs : 0.0;
	return s;
}

void ServerHost::copy_body_transforms(std::vector<sim::BodyTransform> &r_out) const {
	std::lock_guard lock(transforms_mutex_);
	r_out = transforms_;
}

void ServerHost::run() {
	using clock = std::chrono::steady_clock;
	const auto period = std::chrono::nanoseconds(1'000'000'000LL / config_.tick_hz);
	detail::TickTimer timer;
	std::vector<uint32_t> window(600, 0); // step times (us) of the last 600 ticks
	std::vector<uint32_t> sorted;
	size_t window_fill = 0;
	uint32_t max_us = 0;
	std::vector<sim::BodyTransform> scratch;
	auto next = clock::now();

	while (!stop_requested_.load(std::memory_order_acquire)) {
		const auto tick_start = clock::now();

		// 1. network: drain events; echo pings (S0's only message).
		if (net_ && net_->host) {
			net::Host::Event ev;
			while (net_->host->service(0, ev) > 0) {
				if (ev.type == net::Host::EventType::Receive && net::ping_to_pong(ev.data)) {
					net_->host->send(ev.peer, ev.channel, ev.data.data(), ev.data.size(), false);
					pings_echoed_.fetch_add(1, std::memory_order_relaxed);
				}
			}
			net_->host->flush();
			peers_.store(static_cast<uint32_t>(net_->host->connected_peers()), std::memory_order_relaxed);
		}

		// 2. simulation: one fixed step of our own Jolt world.
		const auto step_start = clock::now();
		if (!world_->step()) {
			step_errors_.fetch_add(1, std::memory_order_relaxed);
		}
		const uint32_t step_us =
				static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - step_start).count());

		// 3. publish.
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
				static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - tick_start).count()),
				std::memory_order_relaxed);
		ticks_.fetch_add(1, std::memory_order_release);

		// 4. fixed schedule: absolute deadlines, so jitter does not accumulate into drift.
		next += period;
		const auto now = clock::now();
		if (now > next + 5 * period) {
			overruns_.fetch_add(1, std::memory_order_relaxed);
			next = now;
		}
		timer.sleep_until(next);
	}
}

} // namespace bf3::host

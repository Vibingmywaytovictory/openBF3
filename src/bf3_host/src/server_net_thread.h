#pragma once

#include "bf3/net/enet_host.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace bf3::host {

class ServerNetPort {
public:
	virtual ~ServerNetPort() = default;
	virtual int service(net::Host::Event &r_event) = 0;
	virtual bool send(
			uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) = 0;
	virtual void disconnect_later(uint32_t p_peer) = 0;
	virtual void flush() = 0;
	virtual void disconnect_all() = 0;
	virtual size_t connected_peers() const = 0;
	virtual uint16_t bound_port() const = 0;
	virtual net::PeerCounters peer_counters(uint32_t p_peer) const = 0;
	virtual size_t peer_capacity() const = 0;
};

struct PeerTimeout {
	uint32_t minimum_ms = 0;
	uint32_t maximum_ms = 0;
};

std::shared_ptr<ServerNetPort> make_enet_port(std::unique_ptr<net::Host> p_host, PeerTimeout p_timeout);

struct NetOut {
	uint32_t peer = 0;
	uint8_t channel = 0;
	net::Host::SendMode mode = net::Host::SendMode::Sequenced;
	bool disconnect_after = false;
	std::vector<uint8_t> bytes;
};

class ServerNetThread {
public:
	static constexpr std::chrono::milliseconds kIdleService{ 10 };
	static constexpr size_t kMaxInbound = 1u << 16;

	explicit ServerNetThread(std::shared_ptr<ServerNetPort> p_port);
	~ServerNetThread();
	ServerNetThread(const ServerNetThread &) = delete;
	ServerNetThread &operator=(const ServerNetThread &) = delete;

	void start();
	void stop();

	void pump();
	void take_events(std::vector<net::Host::Event> &r_events);
	void send(std::vector<NetOut> &r_out);

	uint16_t bound_port() const { return port_->bound_port(); }
	uint32_t connected_peers() const { return peers_.load(std::memory_order_relaxed); }
	uint64_t errors() const { return errors_.load(std::memory_order_relaxed); }
	uint64_t inbound_dropped() const { return inbound_dropped_.load(std::memory_order_relaxed); }
	std::vector<net::PeerCounters> peer_counters() const;

private:
	void run();
	void send_all(std::vector<NetOut> &r_out);
	void service_all(std::vector<net::Host::Event> &r_events);
	void publish();

	std::shared_ptr<ServerNetPort> port_;
	std::thread thread_;

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable pumped_;
	bool stop_ = false;
	bool running_ = false;
	uint64_t pump_requested_ = 0;
	uint64_t pump_done_ = 0;
	std::vector<net::Host::Event> inbound_;
	std::vector<NetOut> outbound_;

	mutable std::mutex counters_mutex_;
	std::vector<net::PeerCounters> counters_;

	std::atomic<uint32_t> peers_{ 0 };
	std::atomic<uint64_t> errors_{ 0 };
	std::atomic<uint64_t> inbound_dropped_{ 0 };
};

}

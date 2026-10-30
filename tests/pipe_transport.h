#pragma once

#include "bf3/net/enet_host.h"
#include "bf3/net/protocol.h"
#include "client_transport.h"
#include "server_net_thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace bf3::test::pipe {

using bf3::host::ClientTransport;
using bf3::host::ServerNetPort;

struct Pipe {
	struct Sent {
		uint32_t peer = 0;
		uint8_t channel = 0;
		std::vector<uint8_t> bytes;
	};

	mutable std::mutex mutex;
	std::condition_variable to_client_ready;
	std::deque<net::Host::Event> to_server;
	std::map<uint32_t, std::deque<Sent>> to_client;
	std::vector<Sent> sent;
	std::vector<std::pair<uint32_t, size_t>> disconnects;
	std::set<uint32_t> connected;
	std::atomic<uint64_t> services{ 0 };

	void connect(uint32_t p_peer) {
		std::lock_guard lock(mutex);
		connected.insert(p_peer);
		net::Host::Event e;
		e.type = net::Host::EventType::Connect;
		e.peer = p_peer;
		to_server.push_back(std::move(e));
	}
	void client_send(uint32_t p_peer, uint8_t p_channel, std::vector<uint8_t> p_bytes) {
		std::lock_guard lock(mutex);
		net::Host::Event e;
		e.type = net::Host::EventType::Receive;
		e.peer = p_peer;
		e.channel = p_channel;
		e.data = std::move(p_bytes);
		to_server.push_back(std::move(e));
	}
	bool to_server_empty() const {
		std::lock_guard lock(mutex);
		return to_server.empty();
	}
	size_t count_sent(uint32_t p_peer, net::MessageType p_type) const {
		std::lock_guard lock(mutex);
		size_t n = 0;
		for (const Sent &s : sent) {
			n += s.peer == p_peer && net::peek_type(s.bytes) == p_type ? 1 : 0;
		}
		return n;
	}
	std::optional<net::MatchInfo> match_info(uint32_t p_peer) const {
		std::lock_guard lock(mutex);
		for (const Sent &s : sent) {
			if (s.peer == p_peer && net::peek_type(s.bytes) == net::MessageType::MatchInfo) {
				core::Result<net::MatchInfo> m = net::decode_match_info(s.bytes);
				return m.ok() ? std::optional<net::MatchInfo>(*m) : std::nullopt;
			}
		}
		return std::nullopt;
	}
};

class PipeServerPort final : public ServerNetPort {
public:
	explicit PipeServerPort(std::shared_ptr<Pipe> p_pipe) : pipe_(std::move(p_pipe)) {}

	int service(net::Host::Event &r_event) override {
		pipe_->services.fetch_add(1);
		std::lock_guard lock(pipe_->mutex);
		if (pipe_->to_server.empty()) {
			return 0;
		}
		r_event = std::move(pipe_->to_server.front());
		pipe_->to_server.pop_front();
		return 1;
	}
	bool send(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode) override {
		std::lock_guard lock(pipe_->mutex);
		if (pipe_->connected.count(p_peer) == 0) {
			return false;
		}
		Pipe::Sent s{ p_peer, p_channel, std::vector<uint8_t>(p_bytes.begin(), p_bytes.end()) };
		pipe_->sent.push_back(s);
		pipe_->to_client[p_peer].push_back(std::move(s));
		pipe_->to_client_ready.notify_all();
		return true;
	}
	void disconnect_later(uint32_t p_peer) override {
		std::lock_guard lock(pipe_->mutex);
		pipe_->disconnects.push_back({ p_peer, pipe_->sent.size() });
		pipe_->connected.erase(p_peer);
		net::Host::Event e;
		e.type = net::Host::EventType::Disconnect;
		e.peer = p_peer;
		pipe_->to_server.push_back(std::move(e));
	}
	void flush() override {}
	void disconnect_all() override {
		std::lock_guard lock(pipe_->mutex);
		pipe_->connected.clear();
	}
	size_t connected_peers() const override {
		std::lock_guard lock(pipe_->mutex);
		return pipe_->connected.size();
	}
	uint16_t bound_port() const override { return 0; }
	net::PeerCounters peer_counters(uint32_t) const override { return {}; }
	size_t peer_capacity() const override { return 0; }

private:
	std::shared_ptr<Pipe> pipe_;
};

class PipeClientTransport final : public ClientTransport {
public:
	PipeClientTransport(std::shared_ptr<Pipe> p_pipe, uint32_t p_peer) : pipe_(std::move(p_pipe)), peer_(p_peer) {}

	int service(uint32_t p_wait_ms, net::Host::Event &r_event) override {
		std::unique_lock lock(pipe_->mutex);
		std::deque<Pipe::Sent> &in = pipe_->to_client[peer_];
		pipe_->to_client_ready.wait_for(lock, std::chrono::milliseconds(p_wait_ms), [&] { return !in.empty(); });
		if (in.empty()) {
			return 0;
		}
		r_event = net::Host::Event();
		r_event.type = net::Host::EventType::Receive;
		r_event.peer = peer_;
		r_event.channel = in.front().channel;
		r_event.data = std::move(in.front().bytes);
		in.pop_front();
		return 1;
	}
	void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode) override {
		pipe_->client_send(peer_, p_channel, std::vector<uint8_t>(p_bytes.begin(), p_bytes.end()));
	}
	void flush() override {}
	void disconnect_all() override {}
	uint32_t rtt_ms() const override { return 0; }

private:
	std::shared_ptr<Pipe> pipe_;
	uint32_t peer_ = 0;
};

}

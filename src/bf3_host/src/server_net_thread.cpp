#include "server_net_thread.h"

#include "bf3/net/protocol.h"

#include <utility>

namespace bf3::host {

namespace {

class EnetPort final : public ServerNetPort {
public:
	EnetPort(std::unique_ptr<net::Host> p_host, PeerTimeout p_timeout) :
			host_(std::move(p_host)), timeout_(p_timeout) {}

	int service(net::Host::Event &r_event) override {
		const int r = host_->service(0, r_event);
		if (r > 0 && r_event.type == net::Host::EventType::Connect &&
				(timeout_.minimum_ms != 0 || timeout_.maximum_ms != 0)) {
			host_->set_peer_timeout(r_event.peer, 0, timeout_.minimum_ms, timeout_.maximum_ms);
		}
		return r;
	}
	bool send(
			uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) override {
		return host_->send(p_peer, p_channel, p_bytes.data(), p_bytes.size(), p_mode);
	}
	void disconnect_later(uint32_t p_peer) override { host_->disconnect_later(p_peer); }
	void flush() override { host_->flush(); }
	void disconnect_all() override { host_->disconnect_all(); }
	size_t connected_peers() const override { return host_->connected_peers(); }
	uint16_t bound_port() const override { return host_->bound_port(); }
	net::PeerCounters peer_counters(uint32_t p_peer) const override { return host_->peer_counters(p_peer); }
	size_t peer_capacity() const override { return host_->peer_capacity(); }

private:
	std::unique_ptr<net::Host> host_;
	PeerTimeout timeout_;
};

}

std::shared_ptr<ServerNetPort> make_enet_port(std::unique_ptr<net::Host> p_host, PeerTimeout p_timeout) {
	return std::make_shared<EnetPort>(std::move(p_host), p_timeout);
}

ServerNetThread::ServerNetThread(std::shared_ptr<ServerNetPort> p_port) : port_(std::move(p_port)) {}

ServerNetThread::~ServerNetThread() {
	stop();
}

void ServerNetThread::start() {
	std::lock_guard lock(mutex_);
	if (running_) {
		return;
	}
	stop_ = false;
	running_ = true;
	thread_ = std::thread([this] { run(); });
}

void ServerNetThread::stop() {
	{
		std::lock_guard lock(mutex_);
		if (!running_) {
			return;
		}
		stop_ = true;
	}
	wake_.notify_all();
	thread_.join();
	std::vector<NetOut> rest;
	{
		std::lock_guard lock(mutex_);
		rest.swap(outbound_);
		running_ = false;
	}
	pumped_.notify_all();
	send_all(rest);
	port_->disconnect_all();
	publish();
}

void ServerNetThread::pump() {
	std::unique_lock lock(mutex_);
	if (!running_ || stop_) {
		return;
	}
	const uint64_t want = ++pump_requested_;
	wake_.notify_one();
	pumped_.wait(lock, [&] { return pump_done_ >= want || stop_; });
}

void ServerNetThread::take_events(std::vector<net::Host::Event> &r_events) {
	r_events.clear();
	std::lock_guard lock(mutex_);
	r_events.swap(inbound_);
}

void ServerNetThread::send(std::vector<NetOut> &r_out) {
	if (r_out.empty()) {
		return;
	}
	{
		std::lock_guard lock(mutex_);
		if (outbound_.empty()) {
			outbound_.swap(r_out);
		} else {
			for (NetOut &o : r_out) {
				outbound_.push_back(std::move(o));
			}
		}
	}
	r_out.clear();
	wake_.notify_one();
}

std::vector<net::PeerCounters> ServerNetThread::peer_counters() const {
	std::lock_guard lock(counters_mutex_);
	return counters_;
}

void ServerNetThread::send_all(std::vector<NetOut> &r_out) {
	if (r_out.empty()) {
		return;
	}
	for (const NetOut &o : r_out) {
		if (!port_->send(o.peer, o.channel, o.bytes, o.mode)) {
			errors_.fetch_add(1, std::memory_order_relaxed);
		}
		if (o.disconnect_after) {
			port_->disconnect_later(o.peer);
		}
	}
	r_out.clear();
	port_->flush();
}

void ServerNetThread::service_all(std::vector<net::Host::Event> &r_events) {
	r_events.clear();
	net::Host::Event ev;
	int got = 0;
	while ((got = port_->service(ev)) > 0) {
		r_events.push_back(std::move(ev));
	}
	if (got < 0) {
		errors_.fetch_add(1, std::memory_order_relaxed);
	}
}

void ServerNetThread::publish() {
	peers_.store(static_cast<uint32_t>(port_->connected_peers()), std::memory_order_relaxed);
	std::lock_guard lock(counters_mutex_);
	counters_.resize(port_->peer_capacity());
	for (size_t i = 0; i < counters_.size(); ++i) {
		counters_[i] = port_->peer_counters(static_cast<uint32_t>(i));
	}
}

void ServerNetThread::queue_inbound(net::Host::Event &&p_event, std::vector<uint32_t> &r_overflowed) {
	if (p_event.type == net::Host::EventType::Connect) {
		severed_.erase(p_event.peer);
	}
	if (p_event.type != net::Host::EventType::Receive) {
		inbound_.push_back(std::move(p_event));
		return;
	}
	if (severed_.count(p_event.peer) == 0 && inbound_.size() < kMaxInbound) {
		inbound_.push_back(std::move(p_event));
		return;
	}
	inbound_dropped_.fetch_add(1, std::memory_order_relaxed);
	if (p_event.channel != net::kChannelUnreliable && severed_.insert(p_event.peer).second) {
		r_overflowed.push_back(p_event.peer);
	}
}

void ServerNetThread::run() {
	std::vector<NetOut> out;
	std::vector<net::Host::Event> in;
	std::vector<uint32_t> overflowed;
	for (;;) {
		uint64_t pump_seq = 0;
		{
			std::unique_lock lock(mutex_);
			wake_.wait_for(
					lock, kIdleService, [&] { return stop_ || !outbound_.empty() || pump_requested_ > pump_done_; });
			if (stop_) {
				break;
			}
			out.swap(outbound_);
			pump_seq = pump_requested_;
		}
		send_all(out);
		service_all(in);
		{
			std::lock_guard lock(mutex_);
			for (net::Host::Event &e : in) {
				queue_inbound(std::move(e), overflowed);
			}
			pump_done_ = pump_seq;
		}
		pumped_.notify_all();
		for (const uint32_t peer : overflowed) {
			port_->disconnect_later(peer);
		}
		overflowed.clear();
		publish();
	}
}

}

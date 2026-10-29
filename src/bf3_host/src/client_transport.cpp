#include "client_transport.h"

#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"

#include <utility>

namespace bf3::host {

namespace {

class EnetTransport final : public ClientTransport {
public:
	EnetTransport(std::unique_ptr<net::Host> p_host, uint32_t p_peer) : host_(std::move(p_host)), peer_(p_peer) {}

	int service(uint32_t p_wait_ms, net::Host::Event &r_event) override { return host_->service(p_wait_ms, r_event); }
	void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) override {
		host_->send(peer_, p_channel, p_bytes.data(), p_bytes.size(), p_mode);
	}
	void flush() override { host_->flush(); }
	void disconnect_all() override { host_->disconnect_all(); }
	uint32_t rtt_ms() const override { return host_->peer_rtt_ms(peer_); }

private:
	std::unique_ptr<net::Host> host_;
	uint32_t peer_ = 0;
};

}

core::Result<std::unique_ptr<ClientTransport>> connect_enet_transport(const EnetConnect &p_connect) {
	std::string err;
	std::unique_ptr<net::Host> host = net::Host::create_client(net::kChannelCount, &err);
	if (host == nullptr) {
		return core::make_error(err);
	}
	const int peer = host->connect(p_connect.host.c_str(), p_connect.port);
	if (peer < 0) {
		return core::make_error("cannot connect to " + p_connect.host);
	}
	if (p_connect.peer_timeout_min_ms != 0 || p_connect.peer_timeout_max_ms != 0) {
		host->set_peer_timeout(uint32_t(peer), 0, p_connect.peer_timeout_min_ms, p_connect.peer_timeout_max_ms);
	}
	const uint64_t t0 = net::monotonic_ns();
	bool connected = false;
	while (!connected && net::monotonic_ns() - t0 < uint64_t(p_connect.timeout_ms) * 1'000'000ull) {
		net::Host::Event ev;
		const int r = host->service(10, ev);
		if (r < 0) {
			break;
		}
		connected = r > 0 && ev.type == net::Host::EventType::Connect;
	}
	if (!connected) {
		return core::make_error("no connection to " + p_connect.host + ":" + std::to_string(p_connect.port) +
				" within " + std::to_string(p_connect.timeout_ms) + " ms");
	}
	return std::unique_ptr<ClientTransport>(std::make_unique<EnetTransport>(std::move(host), uint32_t(peer)));
}

}

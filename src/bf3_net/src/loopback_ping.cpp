#include "bf3/net/loopback_ping.h"

#include "bf3/net/enet_host.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bf3::net {

uint64_t monotonic_ns() {
	return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::vector<uint8_t> make_ping(uint32_t p_seq, size_t p_payload_bytes) {
	std::vector<uint8_t> d(std::max(p_payload_bytes, kPingHeaderBytes), 0);
	const uint64_t now = monotonic_ns();
	std::memcpy(d.data(), &kPingMagic, 4);
	std::memcpy(d.data() + 4, &p_seq, 4);
	std::memcpy(d.data() + 8, &now, 8);
	return d;
}

bool ping_to_pong(std::vector<uint8_t> &r_data) {
	uint32_t magic = 0;
	if (r_data.size() < kPingHeaderBytes) {
		return false;
	}
	std::memcpy(&magic, r_data.data(), 4);
	if (magic != kPingMagic) {
		return false;
	}
	std::memcpy(r_data.data(), &kPongMagic, 4);
	return true;
}

bool read_pong(const std::vector<uint8_t> &p_data, uint32_t &r_seq, uint64_t &r_sent_ns) {
	uint32_t magic = 0;
	if (p_data.size() < kPingHeaderBytes) {
		return false;
	}
	std::memcpy(&magic, p_data.data(), 4);
	if (magic != kPongMagic) {
		return false;
	}
	std::memcpy(&r_seq, p_data.data() + 4, 4);
	std::memcpy(&r_sent_ns, p_data.data() + 8, 8);
	return true;
}

PingStats loopback_ping(int p_count, size_t p_payload_bytes, uint32_t p_timeout_ms) {
	PingStats st;
	std::string err;
	std::unique_ptr<Host> server = Host::create_server("127.0.0.1", 0, 4, 2, &err);
	std::unique_ptr<Host> client = server ? Host::create_client(2, &err) : nullptr;
	if (!server || !client) {
		st.error = err;
		return st;
	}
	st.port = server->bound_port();

	// Both hosts live on this thread, so service them alternately with zero timeouts.
	auto pump = [&](Host &h, auto &&on_event) {
		Host::Event ev;
		while (h.service(0, ev) > 0) {
			on_event(ev);
		}
	};
	const uint64_t t_connect = monotonic_ns();
	const int peer = client->connect("127.0.0.1", st.port);
	bool connected = false;
	int server_peer = -1;
	while (peer >= 0 && !connected && monotonic_ns() - t_connect < 3'000'000'000ull) {
		pump(*server, [&](Host::Event &ev) {
			if (ev.type == Host::EventType::Connect) {
				server_peer = static_cast<int>(ev.peer);
			}
		});
		pump(*client, [&](Host::Event &ev) { connected |= ev.type == Host::EventType::Connect; });
	}
	if (!connected) {
		st.error = "connect timed out";
		return st;
	}
	st.connect_ms = static_cast<double>(monotonic_ns() - t_connect) / 1e6;

	std::vector<double> rtts;
	rtts.reserve(static_cast<size_t>(p_count));
	for (int i = 0; i < p_count; ++i) {
		std::vector<uint8_t> ping = make_ping(static_cast<uint32_t>(i), p_payload_bytes);
		client->send(static_cast<uint32_t>(peer), 0, ping.data(), ping.size(), false);
		client->flush();
		st.sent++;
		bool got = false;
		const uint64_t t0 = monotonic_ns();
		while (!got && monotonic_ns() - t0 < uint64_t(p_timeout_ms) * 1'000'000ull) {
			pump(*server, [&](Host::Event &ev) {
				if (ev.type == Host::EventType::Receive && ping_to_pong(ev.data)) {
					server->send(ev.peer, ev.channel, ev.data.data(), ev.data.size(), false);
					server->flush();
				}
			});
			pump(*client, [&](Host::Event &ev) {
				uint32_t seq = 0;
				uint64_t sent = 0;
				if (ev.type == Host::EventType::Receive && read_pong(ev.data, seq, sent) && seq == static_cast<uint32_t>(i)) {
					rtts.push_back(static_cast<double>(monotonic_ns() - sent) / 1000.0);
					got = true;
				}
			});
		}
	}
	st.received = static_cast<int>(rtts.size());
	st.enet_rtt_ms = client->peer_rtt_ms(static_cast<uint32_t>(peer));
	if (!rtts.empty()) {
		std::sort(rtts.begin(), rtts.end());
		double sum = 0.0;
		for (double r : rtts) {
			sum += r;
		}
		st.mean_us = sum / static_cast<double>(rtts.size());
		st.min_us = rtts.front();
		st.max_us = rtts.back();
		st.p50_us = rtts[rtts.size() / 2];
		st.p99_us = rtts[std::min(rtts.size() - 1, (rtts.size() * 99) / 100)];
	}
	client->disconnect_all();
	(void)server_peer;
	st.ok = st.received == st.sent && st.sent > 0;
	if (!st.ok && st.error.empty()) {
		st.error = std::to_string(st.sent - st.received) + " pings lost";
	}
	return st;
}

} // namespace bf3::net

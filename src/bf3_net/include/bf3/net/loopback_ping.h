#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::net {

constexpr uint32_t kPingMagic = 0x474e4950u;
constexpr uint32_t kPongMagic = 0x474e4f50u;
constexpr size_t kPingHeaderBytes = 16;

uint64_t monotonic_ns();
std::vector<uint8_t> make_ping(uint32_t p_seq, size_t p_payload_bytes);

bool ping_to_pong(std::vector<uint8_t> &r_data);

bool read_pong(const std::vector<uint8_t> &p_data, uint32_t &r_seq, uint64_t &r_sent_ns);

struct PingStats {
	bool ok = false;
	int sent = 0;
	int received = 0;
	double connect_ms = 0.0;
	double mean_us = 0.0;
	double min_us = 0.0;
	double p50_us = 0.0;
	double p99_us = 0.0;
	double max_us = 0.0;
	uint32_t enet_rtt_ms = 0;
	uint16_t port = 0;
	std::string error;
};

PingStats loopback_ping(int p_count, size_t p_payload_bytes = 64, uint32_t p_timeout_ms = 1000);

}

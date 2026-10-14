#pragma once
// Ping/pong datagrams and an in-process loopback RTT test: two ENet hosts on 127.0.0.1.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::net {

constexpr uint32_t kPingMagic = 0x474e4950u; // "PING"
constexpr uint32_t kPongMagic = 0x474e4f50u; // "PONG"
constexpr size_t kPingHeaderBytes = 16; // magic u32, sequence u32, send time u64 (ns)

uint64_t monotonic_ns();
std::vector<uint8_t> make_ping(uint32_t p_seq, size_t p_payload_bytes);
// Turns a received PING into the PONG to send back. Returns false if p_data is not a PING.
bool ping_to_pong(std::vector<uint8_t> &r_data);
// Reads a PONG: sequence and the original send time.
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
	uint32_t enet_rtt_ms = 0; // ENet's own smoothed estimate on the client peer
	uint16_t port = 0;
	std::string error;
};

// One ping in flight at a time (no bursts: probe §6.6 showed burst loss on loopback).
PingStats loopback_ping(int p_count, size_t p_payload_bytes = 64, uint32_t p_timeout_ms = 1000);

} // namespace bf3::net

#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "client_transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bf3::host {

enum class SnapshotOrder : uint8_t { Newest, Late, Repeat };

class ClientLink {
public:
	explicit ClientLink(const MonotonicClock &p_clock) :
			clock_(p_clock) {}

	void configure(const net::NetSimConfig &p_netsim);
	void attach(std::unique_ptr<ClientTransport> p_transport);
	void close();

	bool connected() const { return connected_; }
	void drop(const std::string &p_reason);
	void refuse(const char *p_reason);
	void shutdown();

	void submit(uint8_t p_channel, std::vector<uint8_t> p_bytes, uint64_t p_now);
	void release(uint64_t p_now);
	uint64_t wait_deadline(uint64_t p_deadline) const;
	void receive(uint32_t p_wait_ms);
	bool poll(uint64_t p_now, net::NetSim::Datagram &r_datagram);

	bool have_snapshot() const { return have_snapshot_; }
	uint16_t newest_seq() const { return newest_seq_; }
	SnapshotOrder arrive(uint16_t p_seq);
	void send_commands(const sim::Command &p_command, uint32_t p_others_ack_bits, uint64_t p_now);

	void queue_debug(const net::DebugRequest &p_request);
	void queue_kit_select(const net::KitSelect &p_select);
	void queue_spawn_request(uint8_t p_kit);
	void queue_suicide_request();
	void queue_loadout_request(const net::LoadoutRequest &p_request);
	void report(net::DebugReport p_report);
	bool take_report(net::DebugReport &r_report);
	void send_requests(uint64_t p_now);

	double transport_rtt_ms() const;
	const net::NetSimStats &out_stats() const { return out_.stats(); }
	const net::NetSimStats &in_stats() const { return in_.stats(); }
	void stats(ClientStats &r_stats) const;

private:
	void fail(const std::string &p_reason);

	const MonotonicClock &clock_;
	std::unique_ptr<ClientTransport> transport_;
	net::NetSim out_;
	net::NetSim in_;
	bool connected_ = true;
	std::array<char, 128> last_error_{};
	bool refused_ = false;
	bool transport_disconnected_ = false;
	bool disconnect_pending_ = false;
	uint64_t disconnect_at_ = 0;

	bool have_snapshot_ = false;
	uint16_t newest_seq_ = 0;
	uint32_t ack_bits_ = 0;
	std::deque<sim::Command> history_;

	std::mutex requests_mutex_;
	std::deque<net::DebugRequest> debug_out_;
	std::deque<net::KitSelect> kit_out_;
	std::deque<uint8_t> spawn_out_;
	uint32_t suicide_out_ = 0;
	std::deque<net::LoadoutRequest> loadout_out_;
	std::deque<net::DebugReport> reports_;

	uint64_t snapshots_received_ = 0;
	uint64_t snapshots_lost_ = 0;
	uint64_t snapshots_late_ = 0;
	uint64_t commands_sent_ = 0;
	uint64_t spawn_requests_ = 0;
};

}

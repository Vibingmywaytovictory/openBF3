#include "client_link.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace bf3::host {

void ClientLink::configure(const net::NetSimConfig &p_netsim) {
	out_ = net::NetSim(p_netsim);
	net::NetSimConfig in_config = p_netsim;
	in_config.seed = p_netsim.seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
	in_ = net::NetSim(in_config);
}

void ClientLink::attach(std::unique_ptr<ClientTransport> p_transport) {
	transport_ = std::move(p_transport);
	connected_ = true;
}

void ClientLink::close() {
	transport_.reset();
}

void ClientLink::fail(const std::string &p_reason) {
	const size_t n = std::min(p_reason.size(), last_error_.size() - 1);
	std::memcpy(last_error_.data(), p_reason.data(), n);
	last_error_[n] = '\0';
}

void ClientLink::drop(const std::string &p_reason) {
	if (connected_) {
		fail(p_reason);
	}
	connected_ = false;
}

void ClientLink::refuse(const char *p_reason) {
	refused_ = true;
	fail(p_reason);

	if (transport_ != nullptr && connected_) {
		const std::vector<uint8_t> bye = net::encode_bye();
		transport_->send(net::kChannelReliable, bye, net::Host::SendMode::Reliable);
		transport_->flush();
		transport_->disconnect_all();
	}
	connected_ = false;
}

void ClientLink::shutdown() {
	if (transport_ != nullptr) {
		if (connected_) {
			const std::vector<uint8_t> bye = net::encode_bye();
			transport_->send(net::kChannelReliable, bye, net::Host::SendMode::Reliable);
			transport_->flush();
		}
		transport_->disconnect_all();
	}
}

void ClientLink::submit(uint8_t p_channel, std::vector<uint8_t> p_bytes, uint64_t p_now) {
	if (p_bytes.empty()) {
		return;
	}
	net::NetSim::Datagram d;
	d.channel = p_channel;
	d.reliable = p_channel != net::kChannelUnreliable;
	d.bytes = std::move(p_bytes);
	out_.submit(std::move(d), p_now);
}

void ClientLink::release(uint64_t p_now) {
	bool sent = false;
	net::NetSim::Datagram d;
	while (out_.poll(p_now, d)) {
		if (transport_ != nullptr && connected_) {
			transport_->send(d.channel, d.bytes, d.reliable ? net::Host::SendMode::Reliable : net::Host::SendMode::Unsequenced);
			sent = true;
		}
	}
	if (sent) {
		transport_->flush();
	}
}

uint64_t ClientLink::wait_deadline(uint64_t p_deadline) const {
	uint64_t deadline = p_deadline;
	if (const uint64_t r = in_.next_release_ns(); r != 0) {
		deadline = std::min(deadline, r);
	}
	if (const uint64_t r = out_.next_release_ns(); r != 0) {
		deadline = std::min(deadline, r);
	}
	if (disconnect_pending_) {
		deadline = std::min(deadline, disconnect_at_);
	}
	return deadline;
}

void ClientLink::receive(uint32_t p_wait_ms) {
	net::Host::Event ev;
	int got = transport_->service(p_wait_ms, ev);
	while (got > 0) {
		const uint64_t at = clock_();
		if (ev.type == net::Host::EventType::Receive) {
			net::NetSim::Datagram d;
			d.peer = ev.peer;
			d.channel = ev.channel;
			d.reliable = ev.channel != net::kChannelUnreliable;
			d.bytes = std::move(ev.data);
			in_.submit(std::move(d), at);
		} else if (ev.type == net::Host::EventType::Disconnect && !disconnect_pending_) {
			disconnect_pending_ = true;
			disconnect_at_ = std::max(at, in_.last_reliable_release_ns(ev.peer));
		}
		got = transport_->service(0, ev);
	}
	if (got < 0) {
		drop("ENet service failed");
	}
}

bool ClientLink::poll(uint64_t p_now, net::NetSim::Datagram &r_datagram) {
	const uint64_t next = in_.next_release_ns();
	if (disconnect_pending_ && p_now >= disconnect_at_ && (next == 0 || next > disconnect_at_)) {
		disconnect_pending_ = false;
		drop("disconnected by the server");
		server_disconnected_ = true;
	}
	return in_.poll(p_now, r_datagram);
}

SnapshotOrder ClientLink::arrive(uint16_t p_seq) {
	++snapshots_received_;
	if (have_snapshot_ && !net::seq_newer(p_seq, newest_seq_)) {
		if (p_seq == newest_seq_) {
			return SnapshotOrder::Repeat;
		}
		++snapshots_late_;
		const uint16_t back = uint16_t(newest_seq_ - p_seq);
		if (back >= 1 && back <= 32) {
			ack_bits_ |= 1u << (back - 1u);
		}
		return SnapshotOrder::Late;
	}
	if (have_snapshot_) {
		const uint16_t gap = uint16_t(p_seq - newest_seq_);
		snapshots_lost_ += gap > 1 ? uint64_t(gap - 1u) : 0u;
		ack_bits_ = gap >= 32 ? 0u : (ack_bits_ << gap) | (1u << (gap - 1u));
	}
	have_snapshot_ = true;
	newest_seq_ = p_seq;
	return SnapshotOrder::Newest;
}

void ClientLink::send_commands(const sim::Command &p_command, uint32_t p_others_ack_bits, uint64_t p_now) {
	history_.push_back(p_command);
	while (history_.size() > net::kCommandsPerPacket) {
		history_.pop_front();
	}
	net::CommandPacket p;
	p.newest_snapshot = newest_seq_;
	p.snapshot_ack_bits = ack_bits_;
	p.others_ack_bits = p_others_ack_bits;
	p.count = uint8_t(history_.size());
	for (size_t i = 0; i < history_.size(); ++i) {
		p.commands[i] = history_[history_.size() - 1 - i];
	}
	submit(net::kChannelUnreliable, net::encode_command_packet(p), p_now);
	++commands_sent_;
	release(p_now);
}

void ClientLink::queue_debug(const net::DebugRequest &p_request) {
	std::lock_guard lock(requests_mutex_);
	debug_out_.push_back(p_request);
}

void ClientLink::queue_kit_select(const net::KitSelect &p_select) {
	std::lock_guard lock(requests_mutex_);
	if (kit_out_.size() >= 16) {
		kit_out_.pop_front();
	}
	kit_out_.push_back(p_select);
}

void ClientLink::queue_spawn_request(uint8_t p_kit) {
	std::lock_guard lock(requests_mutex_);
	if (spawn_out_.size() >= 16) {
		spawn_out_.pop_front();
	}
	spawn_out_.push_back(p_kit);
}

void ClientLink::queue_suicide_request() {
	std::lock_guard lock(requests_mutex_);
	suicide_out_ = 1;
}

void ClientLink::queue_loadout_request(const net::LoadoutRequest &p_request) {
	std::lock_guard lock(requests_mutex_);
	if (loadout_out_.size() >= 16) {
		loadout_out_.pop_front();
	}
	loadout_out_.push_back(p_request);
}

void ClientLink::report(net::DebugReport p_report) {
	std::lock_guard lock(requests_mutex_);
	reports_.push_back(std::move(p_report));
	while (reports_.size() > 64) {
		reports_.pop_front();
	}
}

bool ClientLink::take_report(net::DebugReport &r_report) {
	std::lock_guard lock(requests_mutex_);
	if (reports_.empty()) {
		return false;
	}
	r_report = std::move(reports_.front());
	reports_.pop_front();
	return true;
}

void ClientLink::send_requests(uint64_t p_now) {
	std::deque<net::DebugRequest> requests;
	std::deque<net::KitSelect> kits;
	std::deque<uint8_t> spawns;
	{
		std::lock_guard lock(requests_mutex_);
		requests.swap(debug_out_);
	}
	for (const net::DebugRequest &q : requests) {
		submit(net::kChannelReliable, net::encode_debug(q), p_now);
	}
	{
		std::lock_guard lock(requests_mutex_);
		kits.swap(kit_out_);
	}
	for (const net::KitSelect &k : kits) {
		submit(net::kChannelReliable, net::encode_kit_select(k), p_now);
	}
	{
		std::lock_guard lock(requests_mutex_);
		spawns.swap(spawn_out_);
	}
	for (const uint8_t kit : spawns) {
		submit(net::kChannelReliable, net::encode_spawn_request({ kit, 0 }), p_now);
		++spawn_requests_;
	}
	uint32_t suicides = 0;
	{
		std::lock_guard lock(requests_mutex_);
		std::swap(suicides, suicide_out_);
	}
	if (suicides != 0) {
		submit(net::kChannelReliable, net::encode_suicide_request(), p_now);
	}
	std::deque<net::LoadoutRequest> loadouts;
	{
		std::lock_guard lock(requests_mutex_);
		loadouts.swap(loadout_out_);
	}
	for (const net::LoadoutRequest &l : loadouts) {
		submit(net::kChannelReliable, net::encode_loadout_request(l), p_now);
	}
}

double ClientLink::transport_rtt_ms() const {
	return transport_ != nullptr ? double(transport_->rtt_ms()) : 0.0;
}

void ClientLink::stats(ClientStats &r_stats) const {
	r_stats.last_error = last_error_;
	r_stats.refused = refused_;
	r_stats.server_disconnected = server_disconnected_;
	r_stats.snapshots_received = snapshots_received_;
	r_stats.snapshots_lost = snapshots_lost_;
	r_stats.snapshots_late = snapshots_late_;
	r_stats.commands_sent = commands_sent_;
	r_stats.spawn_requests = spawn_requests_;
}

}

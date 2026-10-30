#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/enet_host.h"
#include "bf3/net/protocol.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::host {

class Outbox {
public:
	static constexpr size_t kMaxPendingReplies = 1024;

	void send(uint32_t p_peer, uint8_t p_channel, std::vector<uint8_t> p_bytes) {
		OutDatagram d;
		d.peer = p_peer;
		d.channel = p_channel;
		d.reliable = p_channel != net::kChannelUnreliable;
		d.bytes = std::move(p_bytes);
		sent_[p_peer] += net::enet_command_bytes(
				d.bytes.size(), d.reliable ? net::Host::SendMode::Reliable : net::Host::SendMode::Unsequenced);
		pending_.push_back(std::move(d));
	}

	uint64_t sent_bytes(uint32_t p_peer) const {
		const auto it = sent_.find(p_peer);
		return it == sent_.end() ? 0 : it->second;
	}

	bool replies_full() const { return pending_.size() >= kMaxPendingReplies; }

	void drain(std::vector<OutDatagram> &r_out) {
		for (OutDatagram &d : pending_) {
			r_out.push_back(std::move(d));
		}
		pending_.clear();
	}

private:
	std::vector<OutDatagram> pending_;
	std::unordered_map<uint32_t, uint64_t> sent_;
};

}

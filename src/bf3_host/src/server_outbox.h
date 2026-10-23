#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"

#include <cstddef>
#include <cstdint>
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
		pending_.push_back(std::move(d));
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
};

}

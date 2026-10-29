#pragma once

#include "bf3/core/cache/digest.h"
#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::host {

class ServerHandshake {
public:
	void configure(uint64_t p_game_build, const SharedLevel &p_level, size_t p_slots);

	std::optional<net::Kick> hello(const net::Hello &p_hello);
	net::Kick full();
	void describe(size_t p_slot, net::MatchInfo &r_match);
	std::optional<net::Kick> ready(size_t p_slot, const net::Ready &p_ready);
	net::Kick mismatch(uint8_t p_components);
	net::Kick kick(net::KickReason p_code, std::string p_detail);

	const KickCounts &kicks() const { return kicks_; }

private:
	uint64_t game_build_ = 0;
	uint64_t collision_digest_ = 0;
	std::optional<core::Sha256Digest> source_digest_;
	std::vector<net::ComponentDigests> expected_;
	KickCounts kicks_{};
};

}

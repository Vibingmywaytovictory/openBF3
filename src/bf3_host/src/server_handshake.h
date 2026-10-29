#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace bf3::host {

class ServerHandshake {
public:
	void configure(uint64_t p_game_build);

	std::optional<net::Kick> hello(const net::Hello &p_hello);
	net::Kick full();
	net::Kick kick(net::KickReason p_code, std::string p_detail);

	const KickCounts &kicks() const { return kicks_; }

private:
	uint64_t game_build_ = 0;
	KickCounts kicks_{};
};

}

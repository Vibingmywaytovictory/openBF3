#pragma once

#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"

#include <cstdint>
#include <string>

namespace bf3::host {

std::string match_refusal(const net::MatchInfo &p_match, const SharedLevel &p_level);

net::ComponentDigests client_digests(const net::MatchInfo &p_match, const SharedLevel &p_level, uint64_t p_motor_hash);

uint8_t client_mismatch(const net::MatchInfo &p_match, const SharedLevel &p_level, const net::ComponentDigests &p_own,
		uint8_t p_refused);

}

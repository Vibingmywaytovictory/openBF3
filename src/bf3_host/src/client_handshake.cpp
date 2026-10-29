#include "client_handshake.h"

#include "bf3/host/client_session.h"
#include "bf3/sim/sim_types.h"

namespace bf3::host {

std::string match_refusal(const net::MatchInfo &p_match, const SharedLevel &p_level) {
	if (p_match.protocol != net::kProtocolVersion) {
		return "protocol mismatch: the server speaks protocol " + std::to_string(p_match.protocol) + ", this client " +
				std::to_string(net::kProtocolVersion);
	}
	if (p_match.sim_hz != sim::kSimHz) {
		return "simulation rate mismatch: the server steps at " + std::to_string(p_match.sim_hz) +
				" Hz, this client at " + std::to_string(sim::kSimHz) + " Hz";
	}
	return level_refusal(p_match, p_level.level, p_level.mode);
}

net::ComponentDigests client_digests(const net::MatchInfo &p_match, const SharedLevel &p_level, uint64_t p_motor_hash) {
	net::ComponentDigests c;
	c.motor = p_motor_hash;
	c.weapon = p_match.weapon_hash;
	c.kit = p_match.kit_hash;
	const bool tdm = (p_match.rules_flags & net::rules_flags::kTdm) != 0;
	c.logic = tdm && p_level.logic != nullptr ? p_level.logic_digest : 0;
	c.rules = p_match.rules_flags;
	c.sim = sim_digest(p_level.collision_digest, c);
	return c;
}

uint8_t client_mismatch(const net::MatchInfo &p_match, const SharedLevel &p_level, const net::ComponentDigests &p_own,
		uint8_t p_refused) {
	uint8_t mask = p_refused;
	if (p_match.source_digest && p_level.source_digest && *p_match.source_digest != *p_level.source_digest) {
		mask |= net::mismatch_bits::kSource;
	}
	if (mask == 0 && p_match.sim_digest != 0 && p_match.sim_digest != p_own.sim) {
		mask |= net::mismatch_bits::kCollision;
	}
	return mask;
}

}

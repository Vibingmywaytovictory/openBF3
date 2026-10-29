#include "server_handshake.h"

#include "bf3/core/util/format.h"

#include <array>
#include <utility>

namespace bf3::host {

namespace {

constexpr const char *kSourceDetail = "level data differs from the server's (source digest)";

struct ComponentName {
	uint8_t bit;
	net::KickReason code;
	const char *name;
};

constexpr std::array<ComponentName, 7> kComponents{ {
		{ net::mismatch_bits::kSource, net::KickReason::Source, "source" },
		{ net::mismatch_bits::kCollision, net::KickReason::Collision, "collision" },
		{ net::mismatch_bits::kMotor, net::KickReason::Motor, "motor" },
		{ net::mismatch_bits::kWeapon, net::KickReason::Weapon, "weapon" },
		{ net::mismatch_bits::kKit, net::KickReason::Kit, "kit" },
		{ net::mismatch_bits::kLogic, net::KickReason::Logic, "logic" },
		{ net::mismatch_bits::kRules, net::KickReason::Rules, "rules" },
} };

constexpr std::array<const char *, kKickReasonCount> kReasonNames{ "Protocol", "Build", "Full", "Source", "Collision",
	"Motor", "Weapon", "Kit", "Logic", "Rules", "LoadTimeout", "LoadStalled" };

uint8_t named_differences(const net::ComponentDigests &p_want, const net::ComponentDigests &p_got) {
	uint8_t mask = 0;
	mask |= p_got.motor != p_want.motor ? net::mismatch_bits::kMotor : 0u;
	mask |= p_got.weapon != p_want.weapon ? net::mismatch_bits::kWeapon : 0u;
	mask |= p_got.kit != p_want.kit ? net::mismatch_bits::kKit : 0u;
	mask |= p_got.logic != p_want.logic ? net::mismatch_bits::kLogic : 0u;
	mask |= p_got.rules != p_want.rules ? net::mismatch_bits::kRules : 0u;
	return mask;
}
}

const char *kick_reason_name(net::KickReason p_code) {
	const size_t i = size_t(p_code);
	return i < kReasonNames.size() ? kReasonNames[i] : "Unknown";
}

std::string kick_counts_text(const KickCounts &p_kicks) {
	std::string out;
	for (size_t i = 0; i < p_kicks.size(); ++i) {
		out += core::fmt("%s%s=%llu", out.empty() ? "" : " ", kReasonNames[i], (unsigned long long)p_kicks[i]);
	}
	return out;
}

std::string install_difference_text(uint8_t p_components) {
	std::string names;
	for (const ComponentName &c : kComponents) {
		if ((p_components & c.bit) != 0) {
			names += (names.empty() ? "" : ", ") + std::string(c.name);
		}
	}
	return "install differs: " + names;
}

void ServerHandshake::configure(uint64_t p_game_build, const SharedLevel &p_level, size_t p_slots) {
	game_build_ = p_game_build;
	collision_digest_ = p_level.collision_digest;
	source_digest_ = p_level.source_digest;
	expected_.assign(p_slots, net::ComponentDigests{});
}

net::Kick ServerHandshake::kick(net::KickReason p_code, std::string p_detail) {
	++kicks_[size_t(p_code)];
	return net::Kick{ std::move(p_detail), p_code };
}

std::optional<net::Kick> ServerHandshake::hello(const net::Hello &p_hello) {
	if (p_hello.protocol != net::kProtocolVersion) {
		++kicks_[size_t(net::KickReason::Protocol)];
		return net::protocol_kick(p_hello.protocol);
	}
	if (p_hello.game_build != game_build_) {
		return kick(net::KickReason::Build,
				core::fmt("game build %016llx differs from the server's %016llx",
						(unsigned long long)p_hello.game_build, (unsigned long long)game_build_));
	}
	return std::nullopt;
}

net::Kick ServerHandshake::full() {
	return kick(net::KickReason::Full, "server full");
}

void ServerHandshake::describe(size_t p_slot, net::MatchInfo &r_match) {
	net::ComponentDigests c;
	c.motor = r_match.motor_hash;
	c.weapon = r_match.weapon_hash;
	c.kit = r_match.kit_hash;
	c.logic = r_match.logic_digest;
	c.rules = r_match.rules_flags;
	c.sim = sim_digest(collision_digest_, c);
	r_match.sim_digest = c.sim;
	if (p_slot < expected_.size()) {
		expected_[p_slot] = c;
	}
}

std::optional<net::Kick> ServerHandshake::ready(size_t p_slot, const net::Ready &p_ready) {
	if (p_ready.source_digest && source_digest_ && *p_ready.source_digest != *source_digest_) {
		return mismatch(net::mismatch_bits::kSource);
	}
	if (!p_ready.digests || p_slot >= expected_.size()) {
		return std::nullopt;
	}
	const net::ComponentDigests &want = expected_[p_slot];
	uint8_t mask = named_differences(want, *p_ready.digests);
	if (mask == 0 && p_ready.digests->sim != want.sim) {
		mask = net::mismatch_bits::kCollision;
	}
	if (mask == 0) {
		return std::nullopt;
	}
	return mismatch(mask);
}

net::Kick ServerHandshake::mismatch(uint8_t p_components) {
	if ((p_components & net::mismatch_bits::kSource) != 0) {
		return kick(net::KickReason::Source, kSourceDetail);
	}
	for (const ComponentName &c : kComponents) {
		if ((p_components & c.bit) != 0) {
			return kick(c.code, install_difference_text(p_components));
		}
	}
	return kick(net::KickReason::Source, kSourceDetail);
}
}

#include "server_handshake.h"

#include "bf3/core/util/format.h"

#include <array>
#include <utility>

namespace bf3::host {

namespace {

constexpr std::array<const char *, kKickReasonCount> kReasonNames{ "Protocol", "Build", "Full", "Source", "Collision",
	"Motor", "Weapon", "Kit", "Logic", "Rules", "LoadTimeout", "LoadStalled" };

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

void ServerHandshake::configure(uint64_t p_game_build) {
	game_build_ = p_game_build;
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

}

#pragma once

#include "m2a_check.h"

#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3LevelSession;
struct GameState;
struct TdmState;

enum class M2bCase : uint8_t { Play = 0 };
inline constexpr uint32_t kM2bCaseCount = 1;
inline constexpr std::array<std::string_view, kM2bCaseCount> kM2bCaseNames{ "play" };

enum class M2bTrack : uint8_t { Gx = 0, Ui = 1, Cam = 2 };
inline constexpr std::array<std::string_view, 3> kM2bTrackNames{ "gx", "ui", "cam" };

constexpr std::string_view m2b_case_name(M2bCase p_case) {
	return uint32_t(p_case) < kM2bCaseCount ? kM2bCaseNames[uint32_t(p_case)] : std::string_view("?");
}

constexpr std::optional<M2bCase> m2b_case_from_name(std::string_view p_name) {
	for (uint32_t i = 0; i < kM2bCaseCount; ++i) {
		if (kM2bCaseNames[i] == p_name) {
			return M2bCase(i);
		}
	}
	return std::nullopt;
}

inline core::Result<std::vector<M2bCase>> parse_m2b_cases(std::string_view p_arg) {
	if (p_arg.empty()) {
		return core::make_error("--m2b-check: no case given");
	}
	std::vector<M2bCase> out;
	size_t start = 0;
	while (start <= p_arg.size()) {
		const size_t comma = p_arg.find(',', start);
		const std::string_view item = p_arg.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
		const std::optional<M2bCase> c = m2b_case_from_name(item);
		if (!c) {
			return core::make_error("--m2b-check: unknown case '" + std::string(item) + "'");
		}
		for (M2bCase have : out) {
			if (have == *c) {
				return core::make_error("--m2b-check: case '" + std::string(item) + "' given twice");
			}
		}
		out.push_back(*c);
		if (comma == std::string_view::npos) {
			break;
		}
		start = comma + 1;
	}
	return out;
}

struct M2bCaseResult {
	M2bCase which = M2bCase::Play;
	M2aOutcome outcome = M2aOutcome::Skip;
	std::string reason;
	std::vector<M2aRow> rows;
};

struct M2bCheckOptions {
	double play_seconds = 90.0;
	double case_timeout_s = 180.0;
	std::filesystem::path shots_dir;
};

class M2bCheck {
public:
	M2bCheck(Bf3LevelSession &r_session, GameState &r_game, TdmState &r_m2b, std::vector<M2bCase> p_cases, const M2bCheckOptions &p_options);
	~M2bCheck();
	M2bCheck(const M2bCheck &) = delete;
	M2bCheck &operator=(const M2bCheck &) = delete;

	bool frame(uint64_t p_now_ns, double p_dt);
	int exit_code() const;
	std::vector<M2bCaseResult> results() const;
	std::string report() const;

private:
	struct State;
	std::unique_ptr<State> state_;
};

}

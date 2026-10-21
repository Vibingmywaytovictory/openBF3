#pragma once

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

enum class M2aCase : uint8_t { Range = 0, Reload = 1, Kits = 2, Hud = 3, Ads = 4, Audio = 5, Fx = 6, Play = 7, Firefight = 8 };
inline constexpr uint32_t kM2aCaseCount = 9;
inline constexpr std::array<std::string_view, kM2aCaseCount> kM2aCaseNames{ "range", "reload", "kits", "hud", "ads", "audio", "fx", "play", "firefight" };

inline constexpr std::array<M2aCase, 7> kM2aAllCases{ M2aCase::Range, M2aCase::Reload, M2aCase::Kits, M2aCase::Hud, M2aCase::Ads, M2aCase::Audio,
	M2aCase::Fx };

enum class M2aTrack : uint8_t { Wave2 = 0, Bt = 1, Tp = 2, Au3 = 3, Fx3 = 4 };

constexpr std::string_view m2a_case_name(M2aCase p_case) {
	return uint32_t(p_case) < kM2aCaseCount ? kM2aCaseNames[uint32_t(p_case)] : std::string_view("?");
}

constexpr std::optional<M2aCase> m2a_case_from_name(std::string_view p_name) {
	for (uint32_t i = 0; i < kM2aCaseCount; ++i) {
		if (kM2aCaseNames[i] == p_name) {
			return M2aCase(i);
		}
	}
	return std::nullopt;
}

constexpr M2aTrack m2a_case_track(M2aCase p_case) {
	switch (p_case) {
		case M2aCase::Audio:
			return M2aTrack::Au3;
		case M2aCase::Fx:
			return M2aTrack::Fx3;
		case M2aCase::Firefight:
			return M2aTrack::Bt;
		default:
			return M2aTrack::Wave2;
	}
}

inline core::Result<std::vector<M2aCase>> parse_m2a_cases(std::string_view p_arg) {
	if (p_arg == "all") {
		return std::vector<M2aCase>(kM2aAllCases.begin(), kM2aAllCases.end());
	}
	std::vector<M2aCase> out;
	if (p_arg.empty()) {
		return core::make_error("--m2a-check: no case given");
	}
	size_t start = 0;
	while (start <= p_arg.size()) {
		const size_t comma = p_arg.find(',', start);
		const std::string_view item = p_arg.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
		const std::optional<M2aCase> c = m2a_case_from_name(item);
		if (!c) {
			return core::make_error("--m2a-check: unknown case '" + std::string(item) + "'");
		}
		for (M2aCase have : out) {
			if (have == *c) {
				return core::make_error("--m2a-check: case '" + std::string(item) + "' given twice");
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

enum class M2aOutcome : uint8_t { Pass = 0, Fail = 1, Skip = 2 };

struct M2aRow {
	std::string name;
	double value = 0.0;
	std::string rule;
	bool pass = false;
};

struct M2aCaseResult {
	M2aCase which = M2aCase::Range;
	M2aOutcome outcome = M2aOutcome::Skip;
	std::string reason;
	std::vector<M2aRow> rows;
};

struct M2aCheckOptions {
	double play_seconds = 300.0;
	double case_timeout_s = 180.0;
	std::filesystem::path record_wav;
	std::filesystem::path shots_dir;
};

class M2aCheck {
public:
	M2aCheck(Bf3LevelSession &r_session, GameState &r_game, std::vector<M2aCase> p_cases, const M2aCheckOptions &p_options);
	~M2aCheck();
	M2aCheck(const M2aCheck &) = delete;
	M2aCheck &operator=(const M2aCheck &) = delete;

	bool frame(uint64_t p_now_ns, double p_dt);
	int exit_code() const;
	std::vector<M2aCaseResult> results() const;
	std::string report() const;

private:
	struct State;
	std::unique_ptr<State> state_;
};

}

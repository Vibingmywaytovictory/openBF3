#pragma once
// The M2b in-game check (docs/design/m2b.md §1 E9, §11 gx row; owner gx; W2.0 contract, docs/design/m2b-w2.md).
// Godot-free header (m2b.contract2 compiles it): the case list, its parser and the driver the level session runs once
// per display frame after Done (gx's frame line). The case bodies are gx's (m2b_check.cpp).
//
// Command line (the level session's args, after "--"):
//   --m2b-check=play           the scripted play run (E9; godot.m2b.play is a window row: the wave-2 merge pass only)
//   --m2b-play-seconds=<s>     its length (default 90)
//   --m2b-shots=<dir>          where it saves its screenshots (b\shots\gx or scratch, never the install)
//   --kill-limit=<n> --preround=<s> --bots=<n> --deploy-screen=<0|1>   as in play (m2b_game.h M2bOptions)
// The play case (E9): the player stands in the open so bots find it; the spawn screen appears at join and after each
// own death (>= 1), and Deploy spawns the chosen kit; kill-feed lines = the deaths received; the scoreboard's kills per
// team = MatchState; the kill card names the killer and the weapon; the round-end screen appears on RoundOver; 0
// missing font, icon or string lookups (TdmHudRecords); the ctest script fails it on any ERROR line. Rows of a part
// whose track has not landed (OPENBF3_M2B_HAVE_UI / _CAM = 0) are skipped with "waiting for <track>", never passed.
// E12 (godot.m2b_load) is the godot_m1_load.cmake script on the new scene: no case here.

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
struct M2aGame; // level_session_m2a.h
struct M2bGame; // level_session_m2b.h

enum class M2bCase : uint8_t { Play = 0 };
inline constexpr uint32_t kM2bCaseCount = 1;
inline constexpr std::array<std::string_view, kM2bCaseCount> kM2bCaseNames{ "play" };

// The track a row waits for; until it is merged the row is skipped with "waiting for <track>".
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

// "play" -> cases in order. Refuses an empty list, an empty item, an unknown name and a repeated case (hostile
// command lines fail with an error, never a partial list), as parse_m2a_cases does.
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
	std::string reason; // Skip: "waiting for <track>" or what is missing; Fail: the first failing row
	std::vector<M2aRow> rows; // the same row format as the M2a check (the ctest scripts parse one format)
};

struct M2bCheckOptions {
	double play_seconds = 90.0; // E9
	double case_timeout_s = 180.0;
	std::filesystem::path shots_dir;
};

// Runs the cases in order on the session. Main thread.
class M2bCheck {
public:
	M2bCheck(Bf3LevelSession &r_session, M2aGame &r_game, M2bGame &r_m2b, std::vector<M2bCase> p_cases, const M2bCheckOptions &p_options);
	~M2bCheck();
	M2bCheck(const M2bCheck &) = delete;
	M2bCheck &operator=(const M2bCheck &) = delete;

	bool frame(uint64_t p_now_ns, double p_dt); // true when every case is finished
	int exit_code() const; // 0: no case failed (skips are not failures; the report lists them)
	std::vector<M2bCaseResult> results() const;
	std::string report() const; // "PASS|FAIL|SKIP <case>.<row> value rule" lines, as M2aCheck::report

private:
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace bf3::godot_side

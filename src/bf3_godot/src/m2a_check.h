#pragma once
// The M2a in-game check (docs/design/m2a.md §15 gx row, E1, E3-E5, E7-E11, E14, E19; owner gx; W3.0 contract,
// docs/design/m2a-w3.md). Godot-free header (the m2a.contract3 test compiles it): the case list, its parser and the
// driver the level session runs once per display frame after Done (gx's m2a_frame hook). The case bodies are gx's
// (m2a_check.cpp); a case whose track has not landed is skipped with a reason, never passed.
//
// Command line (the level session's args):
//   --m2a-check=<case>[,<case>...]   cases in order; "all" = range, reload, kits, hud, ads, audio, fx
//   --m2a-firefight                  the firefight case alone (FirefightConfig mode Pairs with the observer; E13 / E14
//                                    windowed; godot.m2a_firefight is a timing row: the integrator's)
// Cases:
//   range      E1 / E3: M16A3 fire held from a full magazine through Bf3InputMapper, server trace vs predicted shots,
//              one 1p muzzle start per predicted shot; semi taps one shot per tap
//   reload     E4: reload timing, commit at ReloadThreshold, cancel by switch
//   kits       E5: every kit item deploys, fires, reloads; K / T send KitSelect (ClientSession::send_kit_select)
//   hud        E10: the HUD rows (hud's check through the session)
//   ads        E11: ADS camera, FOVs, look scale
//   audio      E7 (au3): the listener holds, Bf3GameAudio's recording judged by game_audio_check; item 4 auditions
//   fx         E8 (fx3): muzzle presence, oilfire start_effect, cells, atlas, cues
//   play       the scripted 5-minute play run: the input mapper drives move / look / fire / reload / switch / die /
//              respawn against the bots; passes with 0 errors in the log, every subsystem built, events flowing
//   firefight  E13 / E14 with bt's bots (--m2a-firefight)

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

enum class M2aCase : uint8_t { Range = 0, Reload = 1, Kits = 2, Hud = 3, Ads = 4, Audio = 5, Fx = 6, Play = 7, Firefight = 8 };
inline constexpr uint32_t kM2aCaseCount = 9;
inline constexpr std::array<std::string_view, kM2aCaseCount> kM2aCaseNames{ "range", "reload", "kits", "hud", "ads", "audio", "fx", "play", "firefight" };
// "all": the functional cases of godot.m2a (play and firefight are run on their own).
inline constexpr std::array<M2aCase, 7> kM2aAllCases{ M2aCase::Range, M2aCase::Reload, M2aCase::Kits, M2aCase::Hud, M2aCase::Ads, M2aCase::Audio,
	M2aCase::Fx };

// The track a case waits for; until it is merged the case is skipped with "waiting for <track>".
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

// "range,reload" -> cases in order; "all" -> kM2aAllCases (alone only). Refuses an empty list, an empty item, an
// unknown name and a repeated case (hostile command lines fail with an error, never a partial list).
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
	std::string reason; // Skip: "waiting for <track>" or what is missing; Fail: the first failing row
	std::vector<M2aRow> rows;
};

struct M2aCheckOptions {
	double play_seconds = 300.0; // the play case
	double case_timeout_s = 180.0; // per case
	std::filesystem::path record_wav; // --audio-record (scratch or the user cache, never the install)
	std::filesystem::path shots_dir; // screenshots (b\shots\gx or scratch)
};

// Runs the cases in order on the session. Main thread.
class M2aCheck {
public:
	M2aCheck(Bf3LevelSession &r_session, std::vector<M2aCase> p_cases, const M2aCheckOptions &p_options);
	~M2aCheck();
	M2aCheck(const M2aCheck &) = delete;
	M2aCheck &operator=(const M2aCheck &) = delete;

	// One display frame; true when every case is finished (then exit_code and report are final).
	bool frame(uint64_t p_now_ns, double p_dt);
	int exit_code() const; // 0: no case failed (skips are not failures; the report lists them)
	std::vector<M2aCaseResult> results() const;
	std::string report() const; // "PASS|FAIL|SKIP <case>.<row> value rule" lines, the format the ctest scripts parse

private:
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace bf3::godot_side

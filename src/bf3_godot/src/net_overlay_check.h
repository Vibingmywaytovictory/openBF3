#pragma once

#include "net_overlay.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr double kNetOverlayCheckSeconds = 90.0;
inline constexpr double kNetOverlayCheckSettleSeconds = 2.0;
inline constexpr double kNetOverlayCheckOpenSeconds = 10.0;
inline constexpr double kNetOverlayCheckCompareSeconds = 20.0;
inline constexpr double kNetOverlayCheckDeploySeconds = 2.0;
inline constexpr double kNetOverlayCheckHideSeconds = 0.5;
inline constexpr uint32_t kNetOverlayCheckFrames = 300;
inline constexpr uint32_t kNetOverlayCheckRevisions = 4;

struct NetOverlayCheckIo {
	bool alive = false;
	std::function<void()> deploy;
	std::function<void()> toggle_key;
	std::function<bool(const std::string &)> shot;
};

class NetOverlayCheck {
public:
	explicit NetOverlayCheck(std::string p_shots_dir) : shots_dir_(std::move(p_shots_dir)) {}

	bool step(double p_now_s, const NetOverlay &p_overlay, const bf3::host::NetStats &p_net,
			const std::optional<bf3::audio::SoundFateTotals> &p_sounds, const NetOverlayCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return done_ && rows_ > 0 && failures_ == 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { Deploy, Hidden, Opening, Compare, Hiding, Done };

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void enter(Phase p_phase, double p_now_s);
	void compare(const NetOverlay &p_overlay);
	void finish_compare(const NetOverlay &p_overlay, const bf3::host::NetStats &p_net,
			const std::optional<bf3::audio::SoundFateTotals> &p_sounds);

	std::string shots_dir_;
	Phase phase_ = Phase::Deploy;
	double start_s_ = -1.0;
	double phase_s_ = 0.0;
	double deploy_s_ = -1.0;
	uint64_t seen_revision_ = UINT64_MAX;
	uint32_t revisions_ = 0;
	uint32_t frames_ = 0;
	uint32_t mismatches_ = 0;
	uint32_t hidden_frames_ = 0;
	uint32_t off_colour_frames_ = 0;
	bool shot_taken_ = false;
	std::string first_mismatch_;
	std::vector<bf3::core::ui::DebugFieldText> expected_;
	std::vector<std::string> lines_;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	bool done_ = false;
};

}

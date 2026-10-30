#pragma once

#include "minimap_debug.h"

#include "bf3/core/ui/minimap.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3NativeHud;
class Bf3MinimapHud;

inline constexpr double kMinimapModesWaitSeconds = 150.0;
inline constexpr double kMinimapModesDownedSeconds = 60.0;
inline constexpr double kMinimapModesSizeSeconds = 3.0;
inline constexpr uint32_t kMinimapModesSettleFrames = 4;
inline constexpr float kMinimapModesOffsetMetres = 45.0f;
inline constexpr float kMinimapModesScaleTolerance = 0.02f;
inline constexpr int32_t kMinimapModesScaleSpanPixels = 64;
inline constexpr float kMinimapModesLargeBoxGrowth = 1.5f;
inline constexpr int32_t kMinimapModesFirstLargeZoomStep = 1;

struct MinimapModesIo {
	bool alive = false;
	bool spawn_screen = false;
	std::function<void()> deploy;
	std::function<std::string(int32_t)> concept_press;
};

struct MinimapObjectiveTally {
	float world_x = 0.0f;
	float world_z = 0.0f;
	bool hq = false;
	uint32_t drawn = 0;
	uint32_t good = 0;
	float worst_texels = 0.0f;
	uint32_t outside_mask = 0;
};

class MinimapModesCheck {
public:
	MinimapModesCheck(std::string p_level, std::string p_mode);
	bool step(Bf3NativeHud &r_hud, const MinimapModesIo &p_io, double p_delta);
	int exit_code() const { return done_ && failures_ == 0 ? 0 : 1; }
	std::string report() const;

private:
	enum class Phase : uint8_t {
		Wait,
		Overview,
		Deploy,
		Team,
		Small,
		ToggleLarge,
		LargeWhole,
		Zoom,
		Large,
		Downed,
		Done
	};
	bool load(Bf3NativeHud &r_hud);
	void tally(const MinimapDebugFrame &p_frame, std::map<size_t, MinimapObjectiveTally> &r_tally, bool p_only_target,
			size_t p_target);
	std::optional<bf3::core::ui::MinimapUv> uv_at(
			const MinimapDebugFrame &p_frame, float p_local_x, float p_local_y) const;
	void objective_rows(const std::string &p_view, const std::map<size_t, MinimapObjectiveTally> &p_tally);
	void whole_view_row(const MinimapDebugFrame &p_frame);
	bool sweep(Bf3NativeHud &r_hud, std::map<size_t, MinimapObjectiveTally> &r_tally);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	std::string level_;
	std::string mode_;
	std::string last_key_;
	Phase phase_ = Phase::Wait;
	double clock_ = 0.0;
	double phase_start_ = 0.0;
	uint32_t frames_ = 0;
	size_t sweep_index_ = 0;
	int32_t zoom_step_before_ = 0;
	float small_box_width_ = 0.0f;
	uint8_t downed_slot_ = 0xFF;
	Bf3MinimapHud *source_ = nullptr;
	bf3::core::ui::MinimapMapping mapping_;
	uint32_t satellite_side_ = 0;
	float threshold_ = 0.5f;
	bf3::core::ui::L8Image field_;
	std::vector<MinimapObjectiveTally> objectives_;
	std::map<size_t, MinimapObjectiveTally> overview_;
	std::map<size_t, MinimapObjectiveTally> small_;
	std::map<size_t, MinimapObjectiveTally> large_whole_;
	std::map<size_t, MinimapObjectiveTally> large_;
	bool done_ = false;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	std::vector<std::string> lines_;
};

}

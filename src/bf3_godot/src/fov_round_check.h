#pragma once

#include <cstdint>
#include <string>

namespace bf3::godot_side {

class GameSettings;

inline constexpr double kFovRoundCheckRaise = 10.0;
inline constexpr float kFovRoundCheckTolerance = 0.01f;

struct FovRoundView {
	bool hip = false;
	bool round_over = false;
	float camera_fov = 0.0f;
};

class FovRoundCheck {
public:
	void frame(GameSettings &r_settings, const FovRoundView &p_view);
	bool passed() const { return step_ == Step::Passed; }
	std::string report() const;

private:
	enum class Step : uint8_t { FirstHip, SameRound, NextRound, Passed, Failed };
	void fail(std::string p_why);

	Step step_ = Step::FirstHip;
	float level_fov_ = 0.0f;
	float saved_fov_ = 0.0f;
	float same_round_fov_ = 0.0f;
	uint32_t same_round_frames_ = 0;
	uint32_t stale_frames_ = 0;
	std::string failure_;
};

}

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3NativeHud;

inline constexpr double kIcons3dWaitSeconds = 60.0;
inline constexpr double kIcons3dSettleSeconds = 1.5;
inline constexpr double kIcons3dFrontSeconds = 1.5;
inline constexpr double kIcons3dGoneSeconds = 8.0;
inline constexpr uint32_t kIcons3dSettleFrames = 6;
inline constexpr float kIcons3dThrowPitch = 0.12f;
inline constexpr float kIcons3dStageTolerance = 0.5f;
inline constexpr float kIcons3dPercentTolerance = 0.5f;

struct Icons3dIo {
	bool alive = false;
	float yaw = 0.0f;
	std::function<void(float, float)> view;
	std::function<void()> throw_grenade;
	std::function<bool(uint8_t, uint32_t)> grenade_live;
};

class Icons3dCheck {
public:
	bool step(Bf3NativeHud &r_hud, const Icons3dIo &p_io, double p_delta);
	int exit_code() const { return done_ && failures_ == 0 && rows_ > 0 ? 0 : 1; }
	std::string report() const;

private:
	enum class Phase : uint8_t { Wait, Throw, Front, Turn, Gone, Done };
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	bool front_row(Bf3NativeHud &r_hud);
	bool snap_row(Bf3NativeHud &r_hud);

	Phase phase_ = Phase::Wait;
	double clock_ = 0.0;
	double phase_start_ = 0.0;
	uint32_t frames_ = 0;
	float yaw_ = 0.0f;
	uint32_t tag_id_ = 0;
	uint32_t front_frames_ = 0;
	uint32_t front_bad_ = 0;
	float front_near_ = 1e9f;
	float front_far_ = 0.0f;
	float front_min_scale_ = 100.0f;
	float front_min_alpha_ = 100.0f;
	std::string front_detail_;
	float front_worst_px_ = 0.0f;
	uint8_t grenade_slot_ = 0;
	uint32_t grenade_shot_ = 0;
	uint32_t normal_frame_ = 0;
	bool done_ = false;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	std::vector<std::string> lines_;
};

}

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3NativeHud;
class CommoFrame;

inline constexpr double kCommoRoseWaitSeconds = 60.0;
inline constexpr double kCommoRoseSettleSeconds = 1.0;
inline constexpr double kCommoRoseHoldWaitSeconds = 0.6;
inline constexpr double kCommoRoseAimSeconds = 0.4;
inline constexpr float kCommoRoseAimReach = 3.0f;

struct CommoRoseIo {
	bool alive = false;
	const CommoFrame *commo = nullptr;
	std::function<void(bool)> hold;
	std::function<void(float, float)> cursor;
};

class CommoRoseCheck {
public:
	bool step(Bf3NativeHud &r_hud, const CommoRoseIo &p_io, double p_delta);
	int exit_code() const { return done_ && failures_ == 0 && rows_ > 0 ? 0 : 1; }
	std::string report() const;

private:
	enum class Phase : uint8_t { Wait, Hold, Aim, Release, Done };
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	Phase phase_ = Phase::Wait;
	double clock_ = 0.0;
	double phase_start_ = 0.0;
	int32_t want_button_ = -1;
	float aim_x_ = 0.0f;
	float aim_y_ = 0.0f;
	std::string want_instance_;
	uint64_t picks_before_ = 0;
	uint64_t orders_before_ = 0;
	bool done_ = false;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	std::vector<std::string> lines_;
};

}

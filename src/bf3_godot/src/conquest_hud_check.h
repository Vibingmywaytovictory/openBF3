#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3NativeHud;

inline constexpr double kConquestHudWaitSeconds = 150.0;
inline constexpr double kConquestHudRedeploySeconds = 2.0;
inline constexpr uint32_t kConquestHudSettleFrames = 30;

struct ConquestHudIo {
	bool alive = false;
	bool spawn_screen = false;
	bool spawn_ticket_counter = false;
	std::function<void()> deploy;
	std::function<bool(const std::string &)> screenshot;
};

class ConquestHudCheck {
public:
	explicit ConquestHudCheck(std::string p_shot);
	bool step(Bf3NativeHud &r_hud, const ConquestHudIo &p_io, double p_delta);
	int exit_code() const { return done_ && failures_ == 0 ? 0 : 1; }
	std::string report() const;

private:
	enum class Phase : uint8_t { Deploy, Settle, Done };
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void verify(Bf3NativeHud &r_hud, const ConquestHudIo &p_io);

	std::string shot_;
	Phase phase_ = Phase::Deploy;
	double clock_ = 0.0;
	double deploy_clock_ = -1.0e9;
	uint32_t frames_ = 0;
	uint32_t deploys_ = 0;
	bool saw_spawn_counter_ = false;
	bool done_ = false;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	std::vector<std::string> lines_;
};

}

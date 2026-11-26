#pragma once

#include "bf3/host/client_session.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace bf3::godot_side {

class Bf3LevelSession;

struct BotVehiclesInputs {
	std::function<bool(const std::string &)> shot;
};

class BotVehiclesCheck {
public:
	explicit BotVehiclesCheck(std::string p_shots);
	bool step(Bf3LevelSession &r_session, const BotVehiclesInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitSeated, Frame, Drive, Done };

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void aim(Bf3LevelSession &r_session) const;
	bool follow(const host::PresentationFrame &p_frame);

	std::string shots_;
	Phase phase_ = Phase::WaitSeated;
	double t_ = 0.0;
	double framed_ = 0.0;
	uint16_t spawner_ = 0;
	sim::Vec3 at_{};
	sim::Vec3 boarded_{};
	double driving_ = 0.0;
	float heading_ = 0.0f;
	uint32_t seated_ = 0;
	uint32_t vehicles_ = 0;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}

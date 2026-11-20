#pragma once

#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bf3::host {
class ClientSession;
struct PresentationFrame;
}

namespace bf3::sim {
struct WaterSpot;
class WaterSurface;
class CollisionWorld;
}

namespace bf3::godot_side {

class Bf3LevelSession;

struct WaterRunInputs {
	bool alive = false;
	std::function<bool(const std::string &)> shot;
};

class WaterRunCheck {
public:
	explicit WaterRunCheck(std::string p_shots);
	~WaterRunCheck();
	bool step(Bf3LevelSession &r_session, const WaterRunInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t {
		WaitAlive,
		OwnArrive,
		OwnIn,
		OwnStill,
		OwnOut,
		JoinOther,
		OtherArrive,
		OtherIn,
		OtherStill,
		Done
	};

	struct Runner {
		uint32_t swim_frames = 0;
		uint32_t shots = 0;
	};

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void teleport(bf3::host::ClientSession &r_client);
	bool arrived(const bf3::sim::Vec3 &p_at) const;
	void water_rows(const bf3::sim::SoldierState &p_state, const Runner &p_runner, const char *p_who);
	void drive_other(bool p_forward);
	void aim_free_camera(Bf3LevelSession &r_session);
	void shoot(Runner &r_runner, const WaterRunInputs &p_in, const char *p_who);

	std::string shots_;
	Phase phase_ = Phase::WaitAlive;
	double t_ = 0.0;
	double waited_ = 0.0;
	double next_request_ = 0.0;
	uint32_t request_id_ = 0;
	std::unique_ptr<bf3::sim::WaterSpot> spot_;
	float yaw_ = 0.0f;
	float body_under_water_ = 0.0f;
	Runner own_;
	Runner other_;
	std::unique_ptr<bf3::host::PresentationFrame> other_frame_;
	bf3::host::ClientSession *other_client_ = nullptr;
	const bf3::sim::WaterSurface *water_ = nullptr;
	const bf3::sim::CollisionWorld *world_ = nullptr;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}

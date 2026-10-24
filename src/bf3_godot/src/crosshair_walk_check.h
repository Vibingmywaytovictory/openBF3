#pragma once

#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <string>

namespace bf3::host {
struct PresentedWeapon;
}

namespace bf3::sim::weapons {
struct WeaponTable;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3NativeHud;

struct CrosshairWalkInputs {
	bool alive = false;
	uint64_t own_shots = 0;
	const bf3::host::PresentedWeapon *weapon = nullptr;
	const bf3::sim::weapons::WeaponTable *table = nullptr;
};

class CrosshairWalkRun {
public:
	bool step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const CrosshairWalkInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitAlive, Arrive, Stand, Walk, Stop, Done };

	struct Tally {
		uint32_t frames = 0;
		uint32_t matched = 0;
		float first_bad = -1.0f;
	};

	void teleport(Bf3LevelSession &r_session);
	void sample(Tally &r_tally, float p_arm, double p_want);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void finish_rows(const CrosshairWalkInputs &p_in);

	Phase phase_ = Phase::WaitAlive;
	double t_ = 0.0;
	double waited_ = 0.0;
	float yaw_ = 0.0f;
	bf3::sim::Vec3 feet_{};
	uint32_t request_id_ = 0;
	uint64_t shots_at_start_ = 0;
	int32_t magazine_at_start_ = 0;
	double stand_arm_ = 0.0;
	double moving_arm_ = 0.0;
	float walked_m_ = 0.0f;
	bf3::sim::Vec3 walk_from_{};
	Tally stand_;
	Tally walk_;
	Tally stop_;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}

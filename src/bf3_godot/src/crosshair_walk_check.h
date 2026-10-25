#pragma once

#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace bf3::host {
struct PresentedWeapon;
struct PresentationFrame;
struct SharedLevel;
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

	struct Ramp {
		uint32_t frames = 0;
		uint32_t matched = 0;
		uint32_t between = 0;
		double largest_step = 0.0;
		double last = -1.0;
		float first_bad = -1.0f;
		double first_bad_want = -1.0;
	};

	void teleport(Bf3LevelSession &r_session);
	void sample(Tally &r_tally, float p_arm, double p_want);
	void sample_ramp(Ramp &r_ramp, float p_arm, double p_want);
	double blend_arm(const bf3::host::PresentationFrame &p_frame, const CrosshairWalkInputs &p_in) const;
	void ramp_row(const std::string &p_name, const Ramp &p_ramp, const std::string &p_what);
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
	std::shared_ptr<const bf3::host::SharedLevel> level_;
	double offset_ = 0.0;
	double drawn_arm_ = -1.0;
	Ramp rise_;
	Ramp fall_;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}

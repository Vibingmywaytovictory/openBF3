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
struct VaultSpot;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3Viewmodel;

struct VaultRunViewmodel {
	uint8_t clip = 0xFF;
	float seconds = 0.0f;
	float camera_metres = 0.0f;
	float camera_degrees = 0.0f;
};

struct VaultRunInputs {
	bool alive = false;
	std::function<bool(const std::string &)> shot;
	std::function<VaultRunViewmodel()> own_viewmodel;
	std::function<bool(uint8_t, uint8_t &, float &)> crowd_vault;
	std::function<uint64_t()> foley_vaults;
};

VaultRunViewmodel vault_run_viewmodel(const Bf3Viewmodel *p_viewmodel);

class VaultRunCheck {
public:
	explicit VaultRunCheck(std::string p_shots);
	~VaultRunCheck();
	bool step(Bf3LevelSession &r_session, const VaultRunInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t {
		WaitAlive,
		OwnArrive,
		OwnRun,
		OwnAfter,
		JoinOther,
		OtherAlive,
		OtherArrive,
		OtherRun,
		OtherAfter,
		Done
	};

	struct Runner {
		bool pressed = false;
		bool vaulted = false;
		uint8_t kind = 0;
		uint8_t variant = 0;
		float distance = 0.0f;
		float highest = 0.0f;
		double since_press = 0.0;
		uint32_t shots = 0;
		uint32_t vault_frames = 0;
		uint32_t clip_frames = 0;
		uint32_t wrong_clip = 0;
		uint32_t timed = 0;
		float clip_seconds = 0.0f;
		float time_error = 0.0f;
		float lead = -1.0f;
		float lag = 0.0f;
		float camera_metres = 0.0f;
		float camera_degrees = 0.0f;
		uint64_t vaults_before = 0;
		uint64_t vaults_after = 0;
	};

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void teleport(bf3::host::ClientSession &r_client);
	bool arrived(const bf3::sim::Vec3 &p_at) const;
	void follow(Runner &r_runner, const bf3::sim::SoldierState &p_state, const VaultRunInputs &p_in, const char *p_who,
			double p_delta);
	void runner_rows(const Runner &p_runner, const bf3::sim::SoldierState &p_state, const char *p_who);
	void watch_own_clip(const bf3::sim::SoldierState &p_state, const VaultRunInputs &p_in);
	void watch_other_clip(const bf3::sim::SoldierState &p_state, const VaultRunInputs &p_in);
	void clip_rows(const Runner &p_runner, const char *p_who, const char *p_clip);
	void drive_other(bool p_forward);
	void aim_free_camera(Bf3LevelSession &r_session);

	std::string shots_;
	Phase phase_ = Phase::WaitAlive;
	double t_ = 0.0;
	double waited_ = 0.0;
	double next_request_ = 0.0;
	uint32_t request_id_ = 0;
	std::unique_ptr<bf3::sim::VaultSpot> spot_;
	float yaw_ = 0.0f;
	Runner own_;
	Runner other_;
	uint32_t other_jumps_ = 0;
	uint8_t other_slot_ = 0xFF;
	std::unique_ptr<bf3::host::PresentationFrame> other_frame_;
	bf3::host::ClientSession *other_client_ = nullptr;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}

#pragma once

#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3NativeHud;
class Bf3Viewmodel;
class EorFlash;
class GameVehicles;

struct VehicleDriveInputs {
	bool alive = false;
	bool spawn_screen = false;
	uint8_t own_slot = 0xFF;
	const GameVehicles *vehicles = nullptr;
	const Bf3Viewmodel *own_vm = nullptr;
	Bf3NativeHud *hud = nullptr;
	const godot::Camera3D *camera = nullptr;
	uint32_t camera_counter = 0;
	const EorFlash *flash = nullptr;
	float vehicle_black = 0.0f;
	std::function<void()> deploy;
	std::function<bool(const std::string &)> shot;
};

class VehicleDriveRun {
public:
	VehicleDriveRun(std::string p_shots, bool p_zoom) : shots_(std::move(p_shots)), zoom_(p_zoom) {}
	bool step(Bf3LevelSession &r_session, const VehicleDriveInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t {
		WaitScreen,
		Deploying,
		Pick,
		Walk,
		Enter,
		Settle,
		Drive,
		ThirdPerson,
		Stop,
		Seat,
		Exit,
		TankPick,
		TankWalk,
		TankEnter,
		Zoom,
		Unzoom,
		TankDrive,
		TankTurn,
		TurretTurn,
		TankChase,
		Done
	};

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void enter(Phase p_phase);
	bool fail(const std::string &p_name, const std::string &p_detail);
	void shoot(const VehicleDriveInputs &p_in, const char *p_name);
	void hud_row(const VehicleDriveInputs &p_in, uint8_t p_seat);
	void turret_rows(Bf3LevelSession &r_session, const VehicleDriveInputs &p_in);
	void track_fade(const VehicleDriveInputs &p_in);

	std::string shots_;
	bool zoom_ = false;
	float regular_fov_ = 0.0f;
	float turn_ = 0.0f;
	float shown_black_ = 0.0f;
	uint32_t fade_frames_ = 0;
	uint32_t fade_misses_ = 0;
	Phase phase_ = Phase::WaitScreen;
	double t_ = 0.0;
	double next_press_ = 0.0;
	uint16_t spawner_ = 0xFFFF;
	uint32_t request_id_ = 0;
	float start_x_ = 0, start_z_ = 0, heading_x_ = 0, heading_z_ = 0;
	float first_wheel_angle_ = 0;
	float start_heading_ = 0;
	godot::Transform3D last_wheel_;
	uint32_t wheel_frames_ = 0;
	uint32_t wheel_turns_ = 0;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}

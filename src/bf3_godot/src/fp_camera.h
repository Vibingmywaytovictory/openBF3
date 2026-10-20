#pragma once

#include "bf3/host/client_session.h"

#include <godot_cpp/variant/vector3.hpp>

namespace godot {
class Camera3D;
class Node;
}

namespace bf3::godot_side {

godot::Vector3 presented_eye(const bf3::host::PresentationFrame &p_frame, double p_fraction);

float presented_eye_height(const bf3::host::PresentationFrame &p_frame, double p_fraction);

class FpCamera {
public:
	static constexpr float kFovDegrees = 70.0f;

	void attach(godot::Node *p_parent);

	godot::Vector3 update_first_person(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns, float p_yaw, float p_pitch);

	void update_free(double p_delta, const godot::Vector3 &p_move, float p_factor, float p_yaw, float p_pitch);

	void set_free(bool p_on);
	bool free() const { return free_; }
	void place_free(const godot::Vector3 &p_position, float p_yaw, float p_pitch);
	void scale_free_speed(float p_factor);
	float free_speed() const { return free_speed_; }

	godot::Camera3D *first_person() const { return fp_; }
	godot::Camera3D *free_camera() const { return free_cam_; }
	godot::Vector3 last_eye() const { return last_eye_; }
	godot::Vector3 free_position() const { return free_position_; }
	float last_fraction() const { return float(last_fraction_); }

private:
	godot::Camera3D *fp_ = nullptr;
	godot::Camera3D *free_cam_ = nullptr;
	bool free_ = false;
	godot::Vector3 last_eye_;
	double last_fraction_ = 0.0;
	godot::Vector3 free_position_;
	float free_speed_ = 8.0f;
};

}

namespace bf3::godot_side {

struct ViewEffects {
	float world_fov_deg = FpCamera::kFovDegrees;
	float recoil_pitch_deg = 0, recoil_yaw_deg = 0;
	float kick_pitch_deg = 0, kick_yaw_deg = 0;
};

void apply_view_effects(godot::Camera3D *p_camera, const godot::Vector3 &p_eye, double p_yaw, double p_pitch, const ViewEffects &p_effects);

void camera_view_angles_of(const godot::Camera3D *p_camera, double &r_yaw, double &r_pitch);

}

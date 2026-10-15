#pragma once
// The M1 cameras (docs/design/m1.md §11.5). Main thread only.
//
// * First person: a Camera3D at the presented eye: own_prev -> own_curr feet and eye_height_prev -> eye_height_curr,
//   both blended at present_own_fraction(frame, now) (the publish fraction advanced to the display frame's time,
//   docs §9.5), plus visual_offset. Yaw and pitch come from the local mouse state every frame (InputMapper::view),
//   not from the frame, so look has no network latency. Basis rotate_y(yaw + pi) * rotate_x(pitch): the sim's yaw 0
//   looks down +Z and Godot cameras look down -Z (sim_types.h). FOV 70 degrees vertical [inferred placeholder: BF3's
//   FOV setting is not read in M1].
// * Free camera (F6): a detached fly camera (WASD, Q/E down/up, mouse, Shift x6, wheel speed) that starts at the
//   first-person eye; the soldier keeps simulating on the neutral command (input_mapper.h).

#include "bf3/host/client_session.h"

#include <godot_cpp/variant/vector3.hpp>

namespace godot {
class Camera3D;
class Node;
} // namespace godot

namespace bf3::godot_side {

// The presented eye position of the own soldier at blend fraction p_fraction (0 = own_prev, 1 = own_curr): blended
// feet + visual_offset + blended eye height. A pure function of the frame (the E2 recording reads the camera, which
// is placed here).
godot::Vector3 presented_eye(const bf3::host::PresentationFrame &p_frame, double p_fraction);
// The blended eye height alone (crouch / prone checks and the HUD).
float presented_eye_height(const bf3::host::PresentationFrame &p_frame, double p_fraction);

class FpCamera {
public:
	static constexpr float kFovDegrees = 70.0f;

	// Creates the two Camera3D nodes under p_parent; the first-person one is current.
	void attach(godot::Node *p_parent);
	// First person at the presented eye for display time p_now_ns, looking along p_yaw / p_pitch (radians, sim
	// convention). Ignored while the free camera is on. Returns the eye it placed.
	godot::Vector3 update_first_person(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns, float p_yaw, float p_pitch);
	// Free camera: moves by p_move (x right, y up, z forward; each -1..1) x speed x p_factor over p_delta seconds,
	// looking along p_yaw / p_pitch.
	void update_free(double p_delta, const godot::Vector3 &p_move, float p_factor, float p_yaw, float p_pitch);
	// Switches the current camera; the free camera starts at the last first-person eye unless placed.
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
	float free_speed_ = 8.0f; // m/s
};

} // namespace bf3::godot_side

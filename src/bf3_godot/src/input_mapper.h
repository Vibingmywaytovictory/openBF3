#pragma once
// Input -> bf3::host::InputSample for the M1 session (docs/design/m1.md §11.6). Main thread only.
//
// * The actions are built in code at start (InputMap; no project-file actions), with BF3 PC defaults: W/A/S/D move,
//   Left Shift sprint (held), Space jump (counted presses), Left Ctrl crouch, Z prone [inferred: BF3 PC defaults as
//   commonly documented, docs §16 question 11, to verify in S5], F6 free camera, mouse look; Q/E down/up for the free
//   camera only. Keys are physical (layout-independent), so W is the key at the US W position on any layout.
// * Crouch and prone are toggles resolved here into a requested pose: crouch toggles between crouch and stand, prone
//   between prone and stand (crouch while prone goes to crouch, prone while crouched goes to prone).
// * Mouse look updates the soldier's view (yaw, pitch) at once; the pitch is clamped to +-89.99 degrees, the clamp
//   sim::quantize_pitch applies, so the camera never shows a pitch the soldier cannot have (docs §11.5). Yaw follows
//   the sim convention (0 looks down +Z, a quarter turn down +X): moving the mouse right turns right, which lowers
//   the yaw.
// * Spawn view (docs §11.6): on each new life (PresentationFrame::own_curr.life newer than the adopted one) the view
//   is set to own_curr's yaw and pitch and the requested pose to its target pose, and the life is reported in
//   InputSample::life only then. Until then the client commands the spawn view itself (client_session.h step 6).
// * Free camera (F6): the mouse and the movement keys drive the free camera's own view; the soldier gets the
//   neutral command (InputSample::free_camera) and keeps simulating.
// * The mouse is captured in game and released on Esc; a click captures it again.
// * Scripted input: --m1-check and the shot harness drive the same mapper through script_hold / script_press /
//   script_view instead of the devices, so the check exercises the toggle, clamp and adoption code the player uses.

#include "bf3/host/client_session.h"
#include "bf3/sim/command.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>

namespace bf3::godot_side {

enum class InputAction : uint8_t {
	Forward = 0,
	Back,
	Left,
	Right,
	Sprint,
	Jump,
	Crouch,
	Prone,
	FreeCamera,
	Up, // free camera only
	Down,
	Count,
};
inline constexpr size_t kInputActionCount = size_t(InputAction::Count);

const char *input_action_name(InputAction p_action); // the InputMap action name ("bf3_forward", ...)

struct ViewAngles {
	float yaw = 0.0f; // radians, sim convention
	float pitch = 0.0f; // radians, + = up, clamped to +-pitch_limit()
};

class InputMapper {
public:
	// +-89.99 degrees: sim::pitch_radians(sim::kPitchLimit).
	static float pitch_limit();
	static float clamp_pitch(float p_pitch);

	// Adds the bf3_* actions with their default keys to the InputMap (idempotent).
	void install_actions();
	// Device input: mouse motion (look), the toggle and press actions, Esc / click for the mouse capture. Ignored
	// while scripted.
	void handle_event(const godot::Ref<godot::InputEvent> &p_event);
	// Held actions from the devices (call once per frame before sample()); ignored while scripted.
	void poll_devices();
	void set_capture(bool p_capture); // capture the mouse (not while scripted or headless)
	bool captured() const { return captured_; }

	// Scripted input (the checks): replaces the devices until set_scripted(false).
	void set_scripted(bool p_scripted);
	bool scripted() const { return scripted_; }
	void script_hold(InputAction p_action, bool p_held);
	void script_press(InputAction p_action); // one press (toggles, jump, free camera)
	void script_view(float p_yaw, float p_pitch); // the active view (soldier, or the free camera when it is on)
	void release_all(); // nothing held (keeps the view, the pose and the adopted life)

	// The newest presentation: adopts a new life's spawn view (call every frame before sample()).
	void adopt(const bf3::host::PresentationFrame &p_frame);
	// A new ClientSession numbers lives from its own spawn: forget the adopted life (the next spawn is adopted).
	void reset_life();
	// The requested pose (the checks align it with a server-side Teleport, which stands the soldier up).
	void set_requested_pose(bf3::sim::Pose p_pose) { pose_ = p_pose; }
	bf3::sim::Pose requested_pose() const { return pose_; }

	bf3::host::InputSample sample() const;
	const ViewAngles &view() const { return view_; }
	const ViewAngles &free_view() const { return free_view_; }
	bool free_camera() const { return free_camera_; }
	void set_free_camera(bool p_on); // also what F6 does
	bool held(InputAction p_action) const { return held_[size_t(p_action)]; }
	uint32_t adopted_life() const { return life_; }
	uint64_t adoptions() const { return adoptions_; }
	// Free-camera movement axes from the held actions: x right, y up, z forward (each -1..1); the speed factor (Shift).
	godot::Vector3 free_move() const;
	float free_speed_factor() const { return held(InputAction::Sprint) ? 6.0f : 1.0f; }

	float mouse_sensitivity = 0.0022f; // radians per pixel ("raw-ish": no acceleration, no smoothing)

private:
	void press(InputAction p_action);
	void look(float p_dx, float p_dy);

	std::array<bool, kInputActionCount> held_{};
	ViewAngles view_;
	ViewAngles free_view_;
	bf3::sim::Pose pose_ = bf3::sim::Pose::Stand;
	uint32_t jump_presses_ = 0;
	uint32_t life_ = 0;
	uint64_t adoptions_ = 0;
	bool free_camera_ = false;
	bool scripted_ = false;
	bool captured_ = false;
	bool actions_installed_ = false;
};

} // namespace bf3::godot_side

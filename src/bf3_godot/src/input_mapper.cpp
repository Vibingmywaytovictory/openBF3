#include "input_mapper.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_map.hpp>

#include <algorithm>

namespace bf3::godot_side {

using namespace godot;
namespace sim = bf3::sim;

namespace {

struct ActionKey {
	InputAction action;
	const char *name;
	Key key;
};
// BF3 PC defaults (docs §11.6): crouch Left Ctrl, prone Z [inferred, docs §16 question 11].
constexpr ActionKey kActions[] = {
	{ InputAction::Forward, "bf3_forward", KEY_W },
	{ InputAction::Back, "bf3_back", KEY_S },
	{ InputAction::Left, "bf3_left", KEY_A },
	{ InputAction::Right, "bf3_right", KEY_D },
	{ InputAction::Sprint, "bf3_sprint", KEY_SHIFT },
	{ InputAction::Jump, "bf3_jump", KEY_SPACE },
	{ InputAction::Crouch, "bf3_crouch", KEY_CTRL },
	{ InputAction::Prone, "bf3_prone", KEY_Z },
	{ InputAction::FreeCamera, "bf3_free_camera", KEY_F6 },
	{ InputAction::Up, "bf3_up", KEY_E },
	{ InputAction::Down, "bf3_down", KEY_Q },
};
static_assert(sizeof(kActions) / sizeof(kActions[0]) == kInputActionCount);

float wrap_yaw(float p_yaw) {
	// Keep the float small so the camera basis never loses precision over a long session.
	while (p_yaw > sim::kPi) {
		p_yaw -= sim::kTwoPi;
	}
	while (p_yaw < -sim::kPi) {
		p_yaw += sim::kTwoPi;
	}
	return p_yaw;
}

} // namespace

const char *input_action_name(InputAction p_action) {
	const size_t i = size_t(p_action);
	return i < kInputActionCount ? kActions[i].name : "bf3_unknown";
}

float InputMapper::pitch_limit() {
	return sim::pitch_radians(sim::kPitchLimit);
}

float InputMapper::clamp_pitch(float p_pitch) {
	const float limit = pitch_limit();
	return std::clamp(p_pitch, -limit, limit);
}

void InputMapper::install_actions() {
	if (actions_installed_) {
		return;
	}
	InputMap *map = InputMap::get_singleton();
	for (const ActionKey &a : kActions) {
		if (map->has_action(a.name)) {
			continue; // a project-file action of the same name wins (rebinding)
		}
		map->add_action(a.name);
		Ref<InputEventKey> key;
		key.instantiate();
		key->set_physical_keycode(a.key);
		map->action_add_event(a.name, key);
	}
	actions_installed_ = true;
}

void InputMapper::set_capture(bool p_capture) {
	if (scripted_ || DisplayServer::get_singleton()->get_name() == String("headless")) {
		captured_ = false;
		return;
	}
	captured_ = p_capture;
	Input::get_singleton()->set_mouse_mode(p_capture ? Input::MOUSE_MODE_CAPTURED : Input::MOUSE_MODE_VISIBLE);
}

void InputMapper::look(float p_dx, float p_dy) {
	ViewAngles &v = free_camera_ ? free_view_ : view_;
	v.yaw = wrap_yaw(v.yaw - p_dx * mouse_sensitivity);
	v.pitch = clamp_pitch(v.pitch - p_dy * mouse_sensitivity);
}

void InputMapper::handle_event(const Ref<InputEvent> &p_event) {
	if (scripted_ || p_event.is_null()) {
		return;
	}
	if (const InputEventMouseMotion *mm = Object::cast_to<InputEventMouseMotion>(p_event.ptr())) {
		if (captured_) {
			const Vector2 rel = mm->get_relative();
			look(rel.x, rel.y);
		}
		return;
	}
	if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		if (mb->is_pressed() && !captured_ && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			set_capture(true);
		}
		return;
	}
	if (const InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr())) {
		if (key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_ESCAPE) {
			set_capture(false);
			return;
		}
	}
	// exact_match false: the actions carry no modifiers, and an exact match would also demand the event's modifier
	// mask be empty, so Space / Ctrl / Z would do nothing while Shift (sprint) is held. Any modifier state matches.
	for (const InputAction a : { InputAction::Jump, InputAction::Crouch, InputAction::Prone, InputAction::FreeCamera }) {
		if (p_event->is_action_pressed(input_action_name(a), false, false)) {
			press(a);
		}
	}
}

void InputMapper::poll_devices() {
	if (scripted_) {
		return;
	}
	Input *in = Input::get_singleton();
	for (size_t i = 0; i < kInputActionCount; ++i) {
		held_[i] = in->is_action_pressed(kActions[i].name);
	}
}

void InputMapper::set_scripted(bool p_scripted) {
	scripted_ = p_scripted;
	held_.fill(false);
	if (scripted_ && captured_) {
		Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
		captured_ = false;
	}
}

void InputMapper::script_hold(InputAction p_action, bool p_held) {
	if (size_t(p_action) < kInputActionCount) {
		held_[size_t(p_action)] = p_held;
	}
}

void InputMapper::script_press(InputAction p_action) {
	press(p_action);
}

void InputMapper::script_view(float p_yaw, float p_pitch) {
	ViewAngles &v = free_camera_ ? free_view_ : view_;
	v.yaw = wrap_yaw(p_yaw);
	v.pitch = clamp_pitch(p_pitch);
}

void InputMapper::release_all() {
	held_.fill(false);
}

void InputMapper::press(InputAction p_action) {
	switch (p_action) {
		case InputAction::Jump:
			if (!free_camera_) {
				++jump_presses_;
			}
			break;
		case InputAction::Crouch:
			if (!free_camera_) {
				pose_ = pose_ == sim::Pose::Crouch ? sim::Pose::Stand : sim::Pose::Crouch;
			}
			break;
		case InputAction::Prone:
			if (!free_camera_) {
				pose_ = pose_ == sim::Pose::Prone ? sim::Pose::Stand : sim::Pose::Prone;
			}
			break;
		case InputAction::FreeCamera:
			set_free_camera(!free_camera_);
			break;
		default:
			break;
	}
}

void InputMapper::set_free_camera(bool p_on) {
	if (p_on && !free_camera_) {
		free_view_ = view_; // the fly camera starts looking where the soldier looks
	}
	free_camera_ = p_on;
}

void InputMapper::adopt(const bf3::host::PresentationFrame &p_frame) {
	if (!p_frame.valid || p_frame.own_curr.life <= life_) {
		return;
	}
	life_ = p_frame.own_curr.life;
	view_.yaw = wrap_yaw(sim::yaw_radians(p_frame.own_curr.yaw));
	view_.pitch = clamp_pitch(sim::pitch_radians(p_frame.own_curr.pitch));
	pose_ = p_frame.own_curr.target_pose;
	++adoptions_;
}

void InputMapper::reset_life() {
	life_ = 0;
}

bf3::host::InputSample InputMapper::sample() const {
	bf3::host::InputSample s;
	s.yaw = view_.yaw;
	s.pitch = view_.pitch;
	s.life = life_;
	s.jump_presses = jump_presses_;
	s.pose_request = bf3::host::PoseRequest::Set;
	s.pose = pose_;
	s.free_camera = free_camera_;
	if (!free_camera_) {
		s.move_forward = float(int(held(InputAction::Forward)) - int(held(InputAction::Back)));
		s.move_right = float(int(held(InputAction::Right)) - int(held(InputAction::Left)));
		s.held_buttons = held(InputAction::Sprint) ? sim::buttons::kSprint : 0u;
	}
	return s;
}

Vector3 InputMapper::free_move() const {
	if (!free_camera_) {
		return Vector3();
	}
	return Vector3(float(int(held(InputAction::Right)) - int(held(InputAction::Left))), float(int(held(InputAction::Up)) - int(held(InputAction::Down))),
			float(int(held(InputAction::Forward)) - int(held(InputAction::Back))));
}

} // namespace bf3::godot_side

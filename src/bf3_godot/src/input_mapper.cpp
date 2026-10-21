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

	while (p_yaw > sim::kPi) {
		p_yaw -= sim::kTwoPi;
	}
	while (p_yaw < -sim::kPi) {
		p_yaw += sim::kTwoPi;
	}
	return p_yaw;
}

}

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
			continue;
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
	if (tdm_handle_event(p_event)) {
		return;
	}
	if (weapon_handle_event(p_event)) {
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
		free_view_ = view_;
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
	weapon_sample(s);
	return s;
}

Vector3 InputMapper::free_move() const {
	if (!free_camera_) {
		return Vector3();
	}
	return Vector3(float(int(held(InputAction::Right)) - int(held(InputAction::Left))), float(int(held(InputAction::Up)) - int(held(InputAction::Down))),
			float(int(held(InputAction::Forward)) - int(held(InputAction::Back))));
}

}

#include "bf3/sim/weapons/weapon_input.h"

namespace bf3::godot_side {

namespace {

struct WeaponActionKey {
	InputMapper::WeaponAction action;
	const char *name;
	Key key;
	MouseButton button;
};

constexpr WeaponActionKey kWeaponActions[] = {
	{ InputMapper::WeaponAction::Fire, "bf3_fire", KEY_NONE, MOUSE_BUTTON_LEFT },
	{ InputMapper::WeaponAction::Ads, "bf3_ads", KEY_NONE, MOUSE_BUTTON_RIGHT },
	{ InputMapper::WeaponAction::Reload, "bf3_reload", KEY_R, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::Primary, "bf3_primary", KEY_1, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::Sidearm, "bf3_sidearm", KEY_2, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::Grenade, "bf3_grenade", KEY_G, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::Knife, "bf3_knife", KEY_F, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::FireMode, "bf3_fire_mode", KEY_B, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::Kit, "bf3_kit", KEY_K, MOUSE_BUTTON_NONE },
	{ InputMapper::WeaponAction::Team, "bf3_team", KEY_T, MOUSE_BUTTON_NONE },
};
static_assert(sizeof(kWeaponActions) / sizeof(kWeaponActions[0]) == InputMapper::kWeaponActionCount);

constexpr uint8_t kWheelSlots[] = { 0, 1, 2, 6, 7 };

}

const char *InputMapper::weapon_action_name(WeaponAction p_action) {
	const size_t i = size_t(p_action);
	return i < kWeaponActionCount ? kWeaponActions[i].name : "bf3_unknown";
}

void InputMapper::enable_weapons(bool p_on) {
	weapon_.enabled = p_on;
	if (!p_on || weapon_.installed) {
		return;
	}
	InputMap *map = InputMap::get_singleton();
	for (const WeaponActionKey &a : kWeaponActions) {
		if (map->has_action(a.name)) {
			continue;
		}
		map->add_action(a.name);
		if (a.key != KEY_NONE) {
			Ref<InputEventKey> key;
			key.instantiate();
			key->set_physical_keycode(a.key);
			map->action_add_event(a.name, key);
		} else {
			Ref<InputEventMouseButton> mb;
			mb.instantiate();
			mb->set_button_index(a.button);
			map->action_add_event(a.name, mb);
		}
	}
	weapon_.installed = true;
}

void InputMapper::set_weapons_allowed(bool p_allowed) {
	if (weapon_.allowed && !p_allowed) {
		weapon_.held.fill(false);
		weapon_.reload_until_ns = 0;
	}
	weapon_.allowed = p_allowed;
}

void InputMapper::set_weapon_slots(uint8_t p_active, uint8_t p_pending, uint16_t p_filled) {
	weapon_.active = p_active;
	weapon_.pending = p_pending;
	weapon_.filled = p_filled;

	if (weapon_.wanted != 0xFF && (weapon_.wanted == p_active || weapon_.wanted == p_pending || (p_filled & (1u << weapon_.wanted)) == 0)) {
		weapon_.wanted = 0xFF;
	}
}

void InputMapper::select_weapon(uint8_t p_slot) {
	if (p_slot < sim::weapons::kMaxWeaponSlots && (weapon_.filled == 0 || (weapon_.filled & (1u << p_slot)) != 0)) {
		weapon_.wanted = p_slot;
	}
}

void InputMapper::cycle_weapon(int p_direction) {
	constexpr int n = int(sizeof(kWheelSlots));
	const uint8_t from = weapon_.wanted != 0xFF ? weapon_.wanted : (weapon_.pending != 0xFF ? weapon_.pending : weapon_.active);
	int at = 0;
	for (int i = 0; i < n; ++i) {
		if (kWheelSlots[i] == from) {
			at = i;
		}
	}
	for (int step = 1; step <= n; ++step) {
		const uint8_t s = kWheelSlots[size_t(((at + p_direction * step) % n + n) % n)];
		if ((weapon_.filled & (1u << s)) != 0) {
			if (s != from) {
				weapon_.wanted = s;
			}
			return;
		}
	}
}

bool InputMapper::weapon_handle_event(const Ref<InputEvent> &p_event) {
	if (!weapon_.enabled) {
		return false;
	}
	if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		if (!captured_) {
			if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {

				set_capture(true);
				weapon_.swallow_fire = true;
				++weapon_.swallowed_clicks;
			}
			return true;
		}
		if (!mb->is_pressed() || free_camera_) {
			return false;
		}
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT && weapon_.allowed && !weapon_.swallow_fire) {
			++weapon_.fire_presses;
		} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP && weapon_.allowed) {
			cycle_weapon(-1);
		} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN && weapon_.allowed) {
			cycle_weapon(1);
		}
		return false;
	}
	if (const InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr())) {
		if (key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_ESCAPE) {

			weapon_.held.fill(false);
			weapon_.reload_until_ns = 0;
			return false;
		}
	}
	if (!captured_) {
		return false;
	}
	for (const WeaponActionKey &a : kWeaponActions) {
		if (a.key == KEY_NONE || !p_event->is_action_pressed(a.name, false, false)) {
			continue;
		}
		script_press(a.action);
	}
	return false;
}

void InputMapper::script_hold(WeaponAction p_action, bool p_held) {
	if (size_t(p_action) < kWeaponActionCount) {
		weapon_.held[size_t(p_action)] = p_held;
	}
}

void InputMapper::script_press(WeaponAction p_action) {
	switch (p_action) {
		case WeaponAction::Fire:
			if (weapon_.allowed && !free_camera_) {
				++weapon_.fire_presses;
			}
			break;
		case WeaponAction::Reload:
			if (weapon_.allowed && !free_camera_) {
				weapon_.reload_until_ns = (weapon_.now_ns != 0 ? weapon_.now_ns : 1) + kReloadHoldNs;
			}
			break;
		case WeaponAction::Primary:
			select_weapon(0);
			break;
		case WeaponAction::Sidearm:
			select_weapon(1);
			break;
		case WeaponAction::Grenade:
			select_weapon(6);
			break;
		case WeaponAction::Knife:
			select_weapon(7);
			break;
		case WeaponAction::FireMode:
			if (weapon_.allowed && !free_camera_) {
				weapon_.mode_counter = uint8_t((weapon_.mode_counter + 1u) & 3u);
			}
			break;
		case WeaponAction::Kit:
			++weapon_.kit_presses;
			break;
		case WeaponAction::Team:
			++weapon_.team_presses;
			break;
		default:
			break;
	}
}

void InputMapper::script_select(uint8_t p_slot) {
	if (p_slot == 0xFF) {
		weapon_.wanted = 0xFF;
	} else {
		select_weapon(p_slot);
	}
}

uint32_t InputMapper::take_kit_presses() {
	const uint32_t n = weapon_.kit_presses;
	weapon_.kit_presses = 0;
	return n;
}

uint32_t InputMapper::take_team_presses() {
	const uint32_t n = weapon_.team_presses;
	weapon_.team_presses = 0;
	return n;
}

void InputMapper::weapon_frame(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns) {
	if (!weapon_.enabled) {
		return;
	}
	weapon_.frame = &p_frame;
	weapon_.now_ns = p_now_ns;
	if (life_ != weapon_.life) {

		weapon_.life = life_;
		weapon_.wanted = 0xFF;
	}
	if (scripted_) {
		return;
	}
	if (!captured_) {
		weapon_.held.fill(false);
		return;
	}
	Input *in = Input::get_singleton();
	for (const WeaponActionKey &a : kWeaponActions) {
		weapon_.held[size_t(a.action)] = in->is_action_pressed(a.name);
	}
	if (weapon_.swallow_fire) {
		if (weapon_.held[size_t(WeaponAction::Fire)]) {
			weapon_.held[size_t(WeaponAction::Fire)] = false;
		} else {
			weapon_.swallow_fire = false;
		}
	}
}

void InputMapper::weapon_sample(bf3::host::InputSample &r_sample) const {
	if (!weapon_.enabled) {
		return;
	}
	if (weapon_.frame != nullptr) {
		bf3::host::set_view_sample(r_sample, *weapon_.frame, weapon_.now_ns);
	}
	if (!scripted_ && !captured_ && DisplayServer::get_singleton()->get_name() != String("headless")) {

		r_sample.move_forward = 0.0f;
		r_sample.move_right = 0.0f;
		r_sample.held_buttons = 0;
		return;
	}
	r_sample.fire_presses = weapon_.fire_presses;
	if (!weapon_.allowed || free_camera_) {
		return;
	}
	sim::weapons::WeaponInput w;
	w.fire = weapon_.held[size_t(WeaponAction::Fire)];
	w.zoom = weapon_.held[size_t(WeaponAction::Ads)];
	w.reload = weapon_.held[size_t(WeaponAction::Reload)] || (weapon_.reload_until_ns != 0 && weapon_.now_ns < weapon_.reload_until_ns);
	w.fire_mode_counter = weapon_.mode_counter;
	w.select_slot = weapon_.wanted;
	r_sample.held_buttons = sim::weapons::with_weapon_input(r_sample.held_buttons, w);
}

}

namespace bf3::godot_side {

namespace {

struct TdmActionKey {
	InputMapper::TdmAction action;
	const char *name;
	Key key;
	Key key2;
};

constexpr TdmActionKey kTdmActions[] = {
	{ InputMapper::TdmAction::Gadget, "bf3_gadget", KEY_3, KEY_NONE },
	{ InputMapper::TdmAction::Scoreboard, "bf3_scoreboard", KEY_TAB, KEY_NONE },
	{ InputMapper::TdmAction::Deploy, "bf3_deploy", KEY_ENTER, KEY_KP_ENTER },
	{ InputMapper::TdmAction::Kit1, "bf3_kit_1", KEY_1, KEY_NONE },
	{ InputMapper::TdmAction::Kit2, "bf3_kit_2", KEY_2, KEY_NONE },
	{ InputMapper::TdmAction::Kit3, "bf3_kit_3", KEY_3, KEY_NONE },
	{ InputMapper::TdmAction::Kit4, "bf3_kit_4", KEY_4, KEY_NONE },
};
static_assert(sizeof(kTdmActions) / sizeof(kTdmActions[0]) == InputMapper::kTdmActionCount);

}

const char *InputMapper::tdm_action_name(TdmAction p_action) {
	const size_t i = size_t(p_action);
	return i < kTdmActionCount ? kTdmActions[i].name : "bf3_unknown";
}

void InputMapper::enable_tdm(bool p_on) {
	tdm_.enabled = p_on;
	if (!p_on || tdm_.installed) {
		return;
	}
	InputMap *map = InputMap::get_singleton();
	for (const TdmActionKey &a : kTdmActions) {
		if (map->has_action(a.name)) {
			continue;
		}
		map->add_action(a.name);
		for (const Key k : { a.key, a.key2 }) {
			if (k == KEY_NONE) {
				continue;
			}
			Ref<InputEventKey> key;
			key.instantiate();
			key->set_physical_keycode(k);
			map->action_add_event(a.name, key);
		}
	}
	tdm_.installed = true;
}

void InputMapper::set_spawn_screen(bool p_up) {
	if (p_up && !tdm_.spawn_screen) {
		tdm_.kit = -1;
		tdm_.deploy_presses = 0;
	}
	tdm_.spawn_screen = p_up;
}

bool InputMapper::scoreboard_held() const {
	if (!tdm_.enabled) {
		return false;
	}
	if (scripted_) {
		return tdm_.scoreboard_scripted;
	}
	return Input::get_singleton()->is_action_pressed(kTdmActions[size_t(TdmAction::Scoreboard)].name);
}

int InputMapper::take_kit() {
	const int k = tdm_.kit;
	tdm_.kit = -1;
	return k;
}

uint32_t InputMapper::take_deploy_presses() {
	const uint32_t n = tdm_.deploy_presses;
	tdm_.deploy_presses = 0;
	return n;
}

void InputMapper::script_press(TdmAction p_action) {
	switch (p_action) {
		case TdmAction::Gadget:
			if (!tdm_.spawn_screen) {
				select_weapon(2);
			} else {
				tdm_.kit = 2;
			}
			break;
		case TdmAction::Deploy:
			if (tdm_.spawn_screen) {
				++tdm_.deploy_presses;
			}
			break;
		case TdmAction::Kit1:
		case TdmAction::Kit2:
		case TdmAction::Kit3:
		case TdmAction::Kit4:
			if (tdm_.spawn_screen) {
				tdm_.kit = int(p_action) - int(TdmAction::Kit1);
			} else if (p_action == TdmAction::Kit3) {
				select_weapon(2);
			}
			break;
		default:
			break;
	}
}

void InputMapper::script_hold(TdmAction p_action, bool p_held) {
	if (p_action == TdmAction::Scoreboard) {
		tdm_.scoreboard_scripted = p_held;
	}
}

bool InputMapper::tdm_handle_event(const Ref<InputEvent> &p_event) {
	if (!tdm_.enabled) {
		return false;
	}
	if (tdm_.spawn_screen) {
		if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {

			if (mb->is_pressed()) {
				++tdm_.consumed_clicks;
			}
			return true;
		}
		if (Object::cast_to<InputEventMouseMotion>(p_event.ptr()) != nullptr) {
			return true;
		}
		for (const TdmAction a : { TdmAction::Kit1, TdmAction::Kit2, TdmAction::Kit3, TdmAction::Kit4, TdmAction::Deploy }) {
			if (p_event->is_action_pressed(tdm_action_name(a), false, false)) {
				script_press(a);
				return true;
			}
		}
		return false;
	}
	if (captured_ && p_event->is_action_pressed(tdm_action_name(TdmAction::Gadget), false, false)) {
		script_press(TdmAction::Gadget);
	}
	return false;
}

}

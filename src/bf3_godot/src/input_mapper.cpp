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
	if (m2b_handle_event(p_event)) { // M2b gx [mapper]: the spawn screen's keys and clicks, 3, Tab
		return; // M2b gx [mapper]
	} // M2b gx [mapper]
	if (m2a_handle_event(p_event)) { // M2a gx [mapper]: the weapon actions; the capture click never fires
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
	m2a_sample(s); // M2a gx [mapper]: weapon bits, fire_presses, the view sample (no-op unless m2a_enable)
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

// ---- M2a gx (docs/design/m2a.md §5.1; docs/design/m2a-gx.md) -----------------------------------------------------

#include "bf3/sim/weapons/weapon_input.h"

namespace bf3::godot_side {

namespace {

struct M2aActionKey {
	InputMapper::M2aAction action;
	const char *name;
	Key key; // KEY_NONE: a mouse button
	MouseButton button;
};
// m2a.md §5.1: fire left mouse, ADS right mouse (held), R reload, 1 primary, 2 sidearm, G grenade, F knife, B fire
// mode, K / T kit and team for the next spawn (physical keys, as M1's).
constexpr M2aActionKey kM2aActions[] = {
	{ InputMapper::M2aAction::Fire, "bf3_fire", KEY_NONE, MOUSE_BUTTON_LEFT },
	{ InputMapper::M2aAction::Ads, "bf3_ads", KEY_NONE, MOUSE_BUTTON_RIGHT },
	{ InputMapper::M2aAction::Reload, "bf3_reload", KEY_R, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::Primary, "bf3_primary", KEY_1, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::Sidearm, "bf3_sidearm", KEY_2, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::Grenade, "bf3_grenade", KEY_G, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::Knife, "bf3_knife", KEY_F, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::FireMode, "bf3_fire_mode", KEY_B, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::Kit, "bf3_kit", KEY_K, MOUSE_BUTTON_NONE },
	{ InputMapper::M2aAction::Team, "bf3_team", KEY_T, MOUSE_BUTTON_NONE },
};
static_assert(sizeof(kM2aActions) / sizeof(kM2aActions[0]) == InputMapper::kM2aActionCount);

// The weapon slots the wheel visits, in order (m2a.md §5.6: primary, sidearm, grenade, knife).
constexpr uint8_t kM2aWheelSlots[] = { 0, 1, 2, 6, 7 }; // M2b gx [wheel]: slot 2 (the gadget; empty in M2a's kits, so the wheel skips it there)

} // namespace

const char *InputMapper::m2a_action_name(M2aAction p_action) {
	const size_t i = size_t(p_action);
	return i < kM2aActionCount ? kM2aActions[i].name : "bf3_unknown";
}

void InputMapper::m2a_enable(bool p_on) {
	m2a_.enabled = p_on;
	if (!p_on || m2a_.installed) {
		return;
	}
	InputMap *map = InputMap::get_singleton();
	for (const M2aActionKey &a : kM2aActions) {
		if (map->has_action(a.name)) {
			continue; // a project-file action of the same name wins (rebinding)
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
	m2a_.installed = true;
}

void InputMapper::m2a_set_weapons_allowed(bool p_allowed) {
	if (m2a_.allowed && !p_allowed) {
		m2a_.held.fill(false);
		m2a_.reload_until_ns = 0;
	}
	m2a_.allowed = p_allowed;
}

void InputMapper::m2a_set_slots(uint8_t p_active, uint8_t p_pending, uint16_t p_filled) {
	m2a_.active = p_active;
	m2a_.pending = p_pending;
	m2a_.filled = p_filled;
	// The wanted slot is a level, but kept only until the sim takes it: a stale level would switch back to the grenade
	// after a throw, or re-select a slot after the next spawn.
	if (m2a_.wanted != 0xFF && (m2a_.wanted == p_active || m2a_.wanted == p_pending || (p_filled & (1u << m2a_.wanted)) == 0)) {
		m2a_.wanted = 0xFF;
	}
}

void InputMapper::m2a_select(uint8_t p_slot) {
	if (p_slot < sim::weapons::kMaxWeaponSlots && (m2a_.filled == 0 || (m2a_.filled & (1u << p_slot)) != 0)) {
		m2a_.wanted = p_slot;
	}
}

void InputMapper::m2a_cycle(int p_direction) {
	constexpr int n = int(sizeof(kM2aWheelSlots));
	const uint8_t from = m2a_.wanted != 0xFF ? m2a_.wanted : (m2a_.pending != 0xFF ? m2a_.pending : m2a_.active);
	int at = 0;
	for (int i = 0; i < n; ++i) {
		if (kM2aWheelSlots[i] == from) {
			at = i;
		}
	}
	for (int step = 1; step <= n; ++step) {
		const uint8_t s = kM2aWheelSlots[size_t(((at + p_direction * step) % n + n) % n)];
		if ((m2a_.filled & (1u << s)) != 0) {
			if (s != from) {
				m2a_.wanted = s;
			}
			return;
		}
	}
}

bool InputMapper::m2a_handle_event(const Ref<InputEvent> &p_event) {
	if (!m2a_.enabled) {
		return false;
	}
	if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		if (!captured_) {
			if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
				// The click that captures the mouse never fires (the review's playability rule): the fire level
				// stays off until this button goes up.
				set_capture(true);
				m2a_.swallow_fire = true;
				++m2a_.swallowed_clicks;
			}
			return true;
		}
		if (!mb->is_pressed() || free_camera_) {
			return false;
		}
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT && m2a_.allowed && !m2a_.swallow_fire) {
			++m2a_.fire_presses;
		} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP && m2a_.allowed) {
			m2a_cycle(-1);
		} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN && m2a_.allowed) {
			m2a_cycle(1);
		}
		return false;
	}
	if (const InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr())) {
		if (key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_ESCAPE) {
			// Esc releases every held button (and M1's handler releases the mouse).
			m2a_.held.fill(false);
			m2a_.reload_until_ns = 0;
			return false;
		}
	}
	if (!captured_) {
		return false;
	}
	for (const M2aActionKey &a : kM2aActions) {
		if (a.key == KEY_NONE || !p_event->is_action_pressed(a.name, false, false)) {
			continue;
		}
		m2a_script_press(a.action);
	}
	return false;
}

void InputMapper::m2a_script_hold(M2aAction p_action, bool p_held) {
	if (size_t(p_action) < kM2aActionCount) {
		m2a_.held[size_t(p_action)] = p_held;
	}
}

void InputMapper::m2a_script_press(M2aAction p_action) {
	switch (p_action) {
		case M2aAction::Fire:
			if (m2a_.allowed && !free_camera_) {
				++m2a_.fire_presses;
			}
			break;
		case M2aAction::Reload:
			if (m2a_.allowed && !free_camera_) {
				m2a_.reload_until_ns = (m2a_.now_ns != 0 ? m2a_.now_ns : 1) + kM2aReloadHoldNs;
			}
			break;
		case M2aAction::Primary:
			m2a_select(0);
			break;
		case M2aAction::Sidearm:
			m2a_select(1);
			break;
		case M2aAction::Grenade:
			m2a_select(6);
			break;
		case M2aAction::Knife:
			m2a_select(7);
			break;
		case M2aAction::FireMode:
			if (m2a_.allowed && !free_camera_) {
				m2a_.mode_counter = uint8_t((m2a_.mode_counter + 1u) & 3u);
			}
			break;
		case M2aAction::Kit:
			++m2a_.kit_presses;
			break;
		case M2aAction::Team:
			++m2a_.team_presses;
			break;
		default:
			break;
	}
}

void InputMapper::m2a_script_select(uint8_t p_slot) {
	if (p_slot == 0xFF) {
		m2a_.wanted = 0xFF;
	} else {
		m2a_select(p_slot);
	}
}

uint32_t InputMapper::m2a_take_kit_presses() {
	const uint32_t n = m2a_.kit_presses;
	m2a_.kit_presses = 0;
	return n;
}

uint32_t InputMapper::m2a_take_team_presses() {
	const uint32_t n = m2a_.team_presses;
	m2a_.team_presses = 0;
	return n;
}

void InputMapper::m2a_frame(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns) {
	if (!m2a_.enabled) {
		return;
	}
	m2a_.frame = &p_frame;
	m2a_.now_ns = p_now_ns;
	if (life_ != m2a_.life) {
		// A new life: its weapons start on the primary (wp's spawn rule), so the wanted slot starts from scratch.
		m2a_.life = life_;
		m2a_.wanted = 0xFF;
	}
	if (scripted_) {
		return;
	}
	if (!captured_) {
		m2a_.held.fill(false);
		return;
	}
	Input *in = Input::get_singleton();
	for (const M2aActionKey &a : kM2aActions) {
		m2a_.held[size_t(a.action)] = in->is_action_pressed(a.name);
	}
	if (m2a_.swallow_fire) {
		if (m2a_.held[size_t(M2aAction::Fire)]) {
			m2a_.held[size_t(M2aAction::Fire)] = false;
		} else {
			m2a_.swallow_fire = false;
		}
	}
}

void InputMapper::m2a_sample(bf3::host::InputSample &r_sample) const {
	if (!m2a_.enabled) {
		return;
	}
	if (m2a_.frame != nullptr) {
		bf3::host::set_view_sample(r_sample, *m2a_.frame, m2a_.now_ns);
	}
	if (!scripted_ && !captured_ && DisplayServer::get_singleton()->get_name() != String("headless")) {
		// The mouse is released (Esc, or not yet captured): no buttons and no movement.
		r_sample.move_forward = 0.0f;
		r_sample.move_right = 0.0f;
		r_sample.held_buttons = 0;
		return;
	}
	r_sample.fire_presses = m2a_.fire_presses;
	if (!m2a_.allowed || free_camera_) {
		return;
	}
	sim::weapons::WeaponInput w;
	w.fire = m2a_.held[size_t(M2aAction::Fire)];
	w.zoom = m2a_.held[size_t(M2aAction::Ads)];
	w.reload = m2a_.held[size_t(M2aAction::Reload)] || (m2a_.reload_until_ns != 0 && m2a_.now_ns < m2a_.reload_until_ns);
	w.fire_mode_counter = m2a_.mode_counter;
	w.select_slot = m2a_.wanted;
	r_sample.held_buttons = sim::weapons::with_weapon_input(r_sample.held_buttons, w);
}

} // namespace bf3::godot_side

// ---- M2b gx (docs/design/m2b.md §4, §11 gx row; docs/design/m2b-gx.md) ------------------------------------------

namespace bf3::godot_side {

namespace {

struct M2bActionKey {
	InputMapper::M2bAction action;
	const char *name;
	Key key;
	Key key2; // KEY_NONE: none
};
// m2b.md §1.1: the defibrillator on 3 (weapon slot 2), Tab the scoreboard, Enter deploys, 1-4 the spawn screen's kits
// (physical keys, as M1's and M2a's; 1 and 2 are the weapon keys while alive, the kit keys only on the spawn screen).
constexpr M2bActionKey kM2bActions[] = {
	{ InputMapper::M2bAction::Gadget, "bf3_gadget", KEY_3, KEY_NONE },
	{ InputMapper::M2bAction::Scoreboard, "bf3_scoreboard", KEY_TAB, KEY_NONE },
	{ InputMapper::M2bAction::Deploy, "bf3_deploy", KEY_ENTER, KEY_KP_ENTER },
	{ InputMapper::M2bAction::Kit1, "bf3_kit_1", KEY_1, KEY_NONE },
	{ InputMapper::M2bAction::Kit2, "bf3_kit_2", KEY_2, KEY_NONE },
	{ InputMapper::M2bAction::Kit3, "bf3_kit_3", KEY_3, KEY_NONE },
	{ InputMapper::M2bAction::Kit4, "bf3_kit_4", KEY_4, KEY_NONE },
};
static_assert(sizeof(kM2bActions) / sizeof(kM2bActions[0]) == InputMapper::kM2bActionCount);

} // namespace

const char *InputMapper::m2b_action_name(M2bAction p_action) {
	const size_t i = size_t(p_action);
	return i < kM2bActionCount ? kM2bActions[i].name : "bf3_unknown";
}

void InputMapper::m2b_enable(bool p_on) {
	m2b_.enabled = p_on;
	if (!p_on || m2b_.installed) {
		return;
	}
	InputMap *map = InputMap::get_singleton();
	for (const M2bActionKey &a : kM2bActions) {
		if (map->has_action(a.name)) {
			continue; // a project-file action of the same name wins (rebinding)
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
	m2b_.installed = true;
}

void InputMapper::m2b_set_spawn_screen(bool p_up) {
	if (p_up && !m2b_.spawn_screen) {
		m2b_.kit = -1; // presses from before the screen came up are not a choice
		m2b_.deploy_presses = 0;
	}
	m2b_.spawn_screen = p_up;
}

bool InputMapper::m2b_scoreboard_held() const {
	if (!m2b_.enabled) {
		return false;
	}
	if (scripted_) {
		return m2b_.scoreboard_scripted;
	}
	return Input::get_singleton()->is_action_pressed(kM2bActions[size_t(M2bAction::Scoreboard)].name);
}

int InputMapper::m2b_take_kit() {
	const int k = m2b_.kit;
	m2b_.kit = -1;
	return k;
}

uint32_t InputMapper::m2b_take_deploy_presses() {
	const uint32_t n = m2b_.deploy_presses;
	m2b_.deploy_presses = 0;
	return n;
}

void InputMapper::m2b_script_press(M2bAction p_action) {
	switch (p_action) {
		case M2bAction::Gadget:
			if (!m2b_.spawn_screen) {
				m2a_select(2);
			} else {
				m2b_.kit = 2;
			}
			break;
		case M2bAction::Deploy:
			if (m2b_.spawn_screen) {
				++m2b_.deploy_presses;
			}
			break;
		case M2bAction::Kit1:
		case M2bAction::Kit2:
		case M2bAction::Kit3:
		case M2bAction::Kit4:
			if (m2b_.spawn_screen) {
				m2b_.kit = int(p_action) - int(M2bAction::Kit1);
			} else if (p_action == M2bAction::Kit3) {
				m2a_select(2); // 3 while alive is the gadget slot
			}
			break;
		default:
			break;
	}
}

void InputMapper::m2b_script_hold(M2bAction p_action, bool p_held) {
	if (p_action == M2bAction::Scoreboard) {
		m2b_.scoreboard_scripted = p_held;
	}
}

bool InputMapper::m2b_handle_event(const Ref<InputEvent> &p_event) {
	if (!m2b_.enabled) {
		return false;
	}
	if (m2b_.spawn_screen) {
		if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
			// The spawn screen's buttons take their clicks before _unhandled_input; a click beside them must not capture
			// the mouse (M1's and M2a's handlers would) while the screen is up.
			if (mb->is_pressed()) {
				++m2b_.consumed_clicks;
			}
			return true;
		}
		if (Object::cast_to<InputEventMouseMotion>(p_event.ptr()) != nullptr) {
			return true; // no look while the screen is up (the mouse is free)
		}
		for (const M2bAction a : { M2bAction::Kit1, M2bAction::Kit2, M2bAction::Kit3, M2bAction::Kit4, M2bAction::Deploy }) {
			if (p_event->is_action_pressed(m2b_action_name(a), false, false)) {
				m2b_script_press(a);
				return true; // 1 / 2 are not weapon selections here
			}
		}
		return false;
	}
	if (captured_ && p_event->is_action_pressed(m2b_action_name(M2bAction::Gadget), false, false)) {
		m2b_script_press(M2bAction::Gadget);
	}
	return false;
}

} // namespace bf3::godot_side

#include "input_mapper.h"

#include "bf3/sim/vehicles/vehicle_input.h"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_map.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace bf3::godot_side {

using namespace godot;
namespace sim = bf3::sim;

namespace {

struct VehicleActionKey {
	InputMapper::VehicleAction action;
	const char *name;
	Key key;
};

constexpr VehicleActionKey kVehicleActions[] = {
	{ InputMapper::VehicleAction::Use, "bf3_use", KEY_E },
	{ InputMapper::VehicleAction::Exit, "bf3_vehicle_exit", KEY_E },
	{ InputMapper::VehicleAction::Seat1, "bf3_seat_1", KEY_F1 },
	{ InputMapper::VehicleAction::Seat2, "bf3_seat_2", KEY_F2 },
	{ InputMapper::VehicleAction::Seat3, "bf3_seat_3", KEY_F3 },
	{ InputMapper::VehicleAction::Seat4, "bf3_seat_4", KEY_F4 },
	{ InputMapper::VehicleAction::Seat5, "bf3_seat_5", KEY_F5 },
	{ InputMapper::VehicleAction::Seat6, "bf3_seat_6", KEY_F6 },
	{ InputMapper::VehicleAction::Camera, "bf3_vehicle_camera", KEY_C },
	{ InputMapper::VehicleAction::Throttle, "bf3_vehicle_throttle", KEY_W },
	{ InputMapper::VehicleAction::Brake, "bf3_vehicle_brake", KEY_S },
	{ InputMapper::VehicleAction::SteerLeft, "bf3_vehicle_steer_left", KEY_A },
	{ InputMapper::VehicleAction::SteerRight, "bf3_vehicle_steer_right", KEY_D },
};
static_assert(sizeof(kVehicleActions) / sizeof(kVehicleActions[0]) == InputMapper::kVehicleActionCount);
static_assert([] {
	for (size_t i = 0; i < InputMapper::kVehicleActionCount; ++i) {
		if (size_t(kVehicleActions[i].action) != i) {
			return false;
		}
	}
	return true;
}());
static_assert(size_t(InputMapper::VehicleAction::Seat6) - size_t(InputMapper::VehicleAction::Seat1) + 1 ==
		size_t(sim::vehicles::kSeatKeys));

float wrapped(float p_yaw) {
	return std::remainder(p_yaw, sim::kTwoPi);
}

}

const char *InputMapper::vehicle_action_name(VehicleAction p_action) {
	const size_t i = size_t(p_action);
	return i < kVehicleActionCount ? kVehicleActions[i].name : "bf3_unknown";
}

void InputMapper::enable_vehicles() {
	if (vehicle_.installed) {
		return;
	}
	InputMap *map = InputMap::get_singleton();
	for (const VehicleActionKey &a : kVehicleActions) {
		if (map->has_action(a.name)) {
			continue;
		}
		map->add_action(a.name);
		Ref<InputEventKey> key;
		key.instantiate();
		key->set_physical_keycode(a.key);
		map->action_add_event(a.name, key);
	}
	vehicle_.installed = true;
}

void InputMapper::set_seated(bool p_seated) {
	if (p_seated != vehicle_.seated) {
		vehicle_.heading_valid = false;
	}
	vehicle_.seated = p_seated;
}

bool InputMapper::held(VehicleAction p_action) const {
	return size_t(p_action) < kVehicleActionCount && vehicle_.held[size_t(p_action)];
}

void InputMapper::script_hold(VehicleAction p_action, bool p_held) {
	if (size_t(p_action) < kVehicleActionCount) {
		vehicle_.held[size_t(p_action)] = p_held;
	}
}

void InputMapper::script_press(VehicleAction p_action) {
	if (p_action == VehicleAction::Camera) {
		++vehicle_.camera_presses;
	}
}

uint32_t InputMapper::take_camera_presses() {
	return std::exchange(vehicle_.camera_presses, 0u);
}

void InputMapper::hold_view_on(float p_heading, float p_left, float p_right, float p_up, float p_down) {
	if (free_camera_) {
		return;
	}
	float relative = vehicle_.heading_valid ? wrapped(view_.yaw - vehicle_.heading) : 0.0f;
	relative = std::clamp(relative, -p_right, p_left);
	vehicle_.heading = p_heading;
	vehicle_.heading_valid = true;
	view_.yaw = wrapped(p_heading + relative);
	view_.pitch = std::clamp(view_.pitch, std::max(-p_down, -pitch_limit()), std::min(p_up, pitch_limit()));
}

void InputMapper::vehicle_poll() {
	if (scripted_ || !vehicle_.installed) {
		return;
	}
	if (!game_input_) {
		vehicle_.held.fill(false);
		vehicle_.camera_was = false;
		return;
	}
	Input *in = Input::get_singleton();
	for (size_t i = 0; i < kVehicleActionCount; ++i) {
		vehicle_.held[i] = in->is_action_pressed(kVehicleActions[i].name);
	}
	const bool camera = vehicle_.held[size_t(VehicleAction::Camera)];
	if (camera && !vehicle_.camera_was && vehicle_.seated) {
		++vehicle_.camera_presses;
	}
	vehicle_.camera_was = camera;
}

void InputMapper::vehicle_sample(bf3::host::InputSample &r_sample) const {
	if (free_camera_) {
		return;
	}
	if (held(vehicle_.seated ? VehicleAction::Exit : VehicleAction::Use)) {
		r_sample.held_buttons |= sim::buttons::kUse;
	}
	std::optional<uint8_t> seat;
	for (uint8_t k = 0; k < sim::vehicles::kSeatKeys && !seat; ++k) {
		if (held(VehicleAction(uint8_t(VehicleAction::Seat1) + k))) {
			seat = k;
		}
	}
	r_sample.held_buttons = sim::vehicles::with_seat_request(r_sample.held_buttons, seat);
}

void InputMapper::vehicle_axes(bf3::host::InputSample &r_sample) const {
	if (!vehicle_.seated) {
		return;
	}
	r_sample.move_forward = float(int(held(VehicleAction::Throttle)) - int(held(VehicleAction::Brake)));
	r_sample.move_right = float(int(held(VehicleAction::SteerRight)) - int(held(VehicleAction::SteerLeft)));
}
}

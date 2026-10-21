#include "bf3/sim/vehicle_model.h"

#include "bf3/core/util/key_values.h"

#include <string>

namespace bf3::sim {

namespace {

constexpr std::string_view kTesters[] = { "cylinder", "sphere", "ray" };
static_assert(std::size(kTesters) == size_t(WheelCollisionTester::Ray) + 1 && sizeof(WheelCollisionTester) == 1);
constexpr std::string_view kSteerModels[] = { "scaled_lock", "sensitivity_radians" };
static_assert(
		std::size(kSteerModels) == size_t(SteerAngleModel::SensitivityRadians) + 1 && sizeof(SteerAngleModel) == 1);

using Key = core::KeyDef<VehicleTuning>;
using core::KeyKind;

#define BF3_VEHICLE_FIELD(f) [](VehicleTuning &t) -> void * { return &t.f; }

const Key kKeys[] = {
	{ "gravity", KeyKind::Float, 0.01f, 100.0f, BF3_VEHICLE_FIELD(gravity) },
	{ "ground_friction", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(ground_friction) },
	{ "static_sag_fraction", KeyKind::Float, 0.01f, 0.95f, BF3_VEHICLE_FIELD(static_sag_fraction) },
	{ "damping_ratio", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(damping_ratio) },
	{ "suspension_min_fraction", KeyKind::Float, 0.0f, 0.9f, BF3_VEHICLE_FIELD(suspension_min_fraction) },
	{ "wheel_angular_damping", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(wheel_angular_damping) },
	{ "brake_torque_per_force", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(brake_torque_per_force) },
	{ "hand_brake_torque", KeyKind::Float, 0.0f, 1e6f, BF3_VEHICLE_FIELD(hand_brake_torque) },
	{ "longitudinal_grip", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(longitudinal_grip) },
	{ "lateral_grip", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(lateral_grip) },
	{ "tester", KeyKind::Choice, 0, 0, BF3_VEHICLE_FIELD(tester), kTesters },
	{ "steer_model", KeyKind::Choice, 0, 0, BF3_VEHICLE_FIELD(steer_model), kSteerModels },
	{ "steer_max_degrees", KeyKind::Float, 0.0f, 89.0f, BF3_VEHICLE_FIELD(steer_max_degrees) },
	{ "engine_inertia", KeyKind::Float, 0.001f, 1000.0f, BF3_VEHICLE_FIELD(engine_inertia) },
	{ "engine_angular_damping", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(engine_angular_damping) },
	{ "shift_up_rpm", KeyKind::Float, 1.0f, 100000.0f, BF3_VEHICLE_FIELD(shift_up_rpm) },
	{ "shift_down_rpm", KeyKind::Float, 0.0f, 100000.0f, BF3_VEHICLE_FIELD(shift_down_rpm) },
	{ "switch_time", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(switch_time) },
	{ "clutch_release_time", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(clutch_release_time) },
	{ "switch_latency", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(switch_latency) },
	{ "clutch_strength", KeyKind::Float, 0.0f, 10000.0f, BF3_VEHICLE_FIELD(clutch_strength) },
	{ "differential_limited_slip", KeyKind::Float, 1.0f, 1e30f, BF3_VEHICLE_FIELD(differential_limited_slip) },
	{ "anti_roll_scale", KeyKind::Float, 0.0f, 1e9f, BF3_VEHICLE_FIELD(anti_roll_scale) },
	{ "drag_scale", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(drag_scale) },
	{ "max_pitch_roll_degrees", KeyKind::Float, 1.0f, 180.0f, BF3_VEHICLE_FIELD(max_pitch_roll_degrees) },
	{ "stand_still_speed", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(stand_still_speed) },
	{ "reverse_switch_speed", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(reverse_switch_speed) },
	{ "collision_steps", KeyKind::U32, 1, 8, BF3_VEHICLE_FIELD(collision_steps) },
	{ "velocity_steps", KeyKind::U32, 1, 128, BF3_VEHICLE_FIELD(velocity_steps) },
	{ "position_steps", KeyKind::U32, 0, 128, BF3_VEHICLE_FIELD(position_steps) },
};
static_assert(std::size(kKeys) == 30);

#undef BF3_VEHICLE_FIELD

std::string vehicle_fault(const core::KeyFaultInfo &p_info) {
	switch (p_info.fault) {
		case core::KeyFault::TooManyLines:
			return std::string(p_info.what) + " line " + std::to_string(p_info.limit + 1) + ": more than " +
					std::to_string(p_info.limit) + " lines";
		case core::KeyFault::NotAChoice:
			return core::key_fault_where(p_info) + ": `" + std::string(p_info.key) + "` must be " +
					core::choice_list(p_info.choices, ", ", " or ") + ", got `" + core::shown_text(p_info.value) + "`";
		default:
			return core::key_fault_message(p_info);
	}
}

const core::KeyValueGrammar<VehicleTuning> kGrammar{ "vehicle tuning", kKeys,
	kMaxVehicleTuningLines * (kMaxVehicleTuningLineBytes + 1), kMaxVehicleTuningLines, kMaxVehicleTuningLineBytes, true,
	'\n', true, true, vehicle_fault };
}

core::Result<VehicleTuning> parse_vehicle_tuning(std::string_view p_text) {
	return core::parse_key_values(p_text, kGrammar);
}

std::string format_vehicle_tuning(const VehicleTuning &p_tuning) {
	return core::format_key_values(p_tuning, kKeys);
}

uint64_t vehicle_tuning_hash(const VehicleTuning &p_tuning) {
	const std::string text = format_vehicle_tuning(p_tuning);
	uint64_t h = 0xCBF29CE484222325ull;
	for (const char c : text) {
		h ^= uint8_t(c);
		h *= 0x100000001B3ull;
	}
	return h;
}

}

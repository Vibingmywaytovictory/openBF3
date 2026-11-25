#include "bf3/sim/vehicle_model.h"

#include "bf3/core/util/key_values.h"

#include <string>

namespace bf3::sim {

namespace {

constexpr std::string_view kTesters[] = { "cylinder", "sphere", "ray" };
static_assert(std::size(kTesters) == size_t(WheelCollisionTester::Ray) + 1 && sizeof(WheelCollisionTester) == 1);

using Key = core::KeyDef<VehicleTuning>;
using core::KeyKind;

#define BF3_VEHICLE_FIELD(f) [](VehicleTuning &t) -> void * { return &t.f; }

const Key kKeys[] = {
	{ "gravity", KeyKind::Float, 0.01f, 100.0f, BF3_VEHICLE_FIELD(gravity) },
	{ "ground_friction", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(ground_friction) },
	{ "suspension_min_fraction", KeyKind::Float, 0.0f, 0.9f, BF3_VEHICLE_FIELD(suspension_min_fraction) },
	{ "wheel_angular_damping", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(wheel_angular_damping) },
	{ "longitudinal_grip", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(longitudinal_grip) },
	{ "lateral_grip", KeyKind::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(lateral_grip) },
	{ "tester", KeyKind::Choice, 0, 0, BF3_VEHICLE_FIELD(tester), kTesters },
	{ "max_pitch_roll_degrees", KeyKind::Float, 1.0f, 180.0f, BF3_VEHICLE_FIELD(max_pitch_roll_degrees) },
	{ "wheel_torque_scale", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(wheel_torque_scale) },
	{ "track_skid_steer_degrees", KeyKind::Float, 0.0f, 89.0f, BF3_VEHICLE_FIELD(track_skid_steer_degrees) },
	{ "stand_still_speed", KeyKind::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(stand_still_speed) },
	{ "collision_steps", KeyKind::U32, 1, 8, BF3_VEHICLE_FIELD(collision_steps) },
	{ "velocity_steps", KeyKind::U32, 1, 128, BF3_VEHICLE_FIELD(velocity_steps) },
	{ "position_steps", KeyKind::U32, 0, 128, BF3_VEHICLE_FIELD(position_steps) },
};
static_assert(std::size(kKeys) == 14);

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

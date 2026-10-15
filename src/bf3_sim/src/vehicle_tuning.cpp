// VehicleTuning file format (docs/design/s2.md §5.3; vehicle_model.h): "key = value" lines like motor_tuning.cfg
// (motor_tuning.cpp, docs/design/m1.md §6.11). '#' starts a comment (whole line or after the value), blank lines are
// skipped, enums by name, numbers as C locale decimals. An unknown key, a repeated key, a bad or non-finite value, a
// value out of range, a line over 256 bytes or more than 512 lines fails with the line number. Keys left out keep
// the defaults. format_vehicle_tuning writes every key with the shortest round-trip number text, so
// parse(format(t)) == t bit for bit.
#include "bf3/sim/vehicle_model.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace bf3::sim {

namespace {

enum class KeyType : uint8_t { Float, U32, Tester, SteerModel };

struct KeyDef {
	const char *name;
	KeyType type;
	float min;
	float max;
	void *(*field)(VehicleTuning &);
};

#define BF3_VEHICLE_FIELD(f) [](VehicleTuning &t) -> void * { return &t.f; }

// Declaration order of VehicleTuning. Ranges keep a hostile or mistyped file from making the model divide by zero
// or the step loop for long; every default and every plausible measured value lies well inside them.
const KeyDef kKeys[] = {
	{ "gravity", KeyType::Float, 0.01f, 100.0f, BF3_VEHICLE_FIELD(gravity) },
	{ "ground_friction", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(ground_friction) },
	{ "static_sag_fraction", KeyType::Float, 0.01f, 0.95f, BF3_VEHICLE_FIELD(static_sag_fraction) },
	{ "damping_ratio", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(damping_ratio) },
	{ "suspension_min_fraction", KeyType::Float, 0.0f, 0.9f, BF3_VEHICLE_FIELD(suspension_min_fraction) },
	{ "wheel_angular_damping", KeyType::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(wheel_angular_damping) },
	{ "brake_torque_per_force", KeyType::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(brake_torque_per_force) },
	{ "hand_brake_torque", KeyType::Float, 0.0f, 1e6f, BF3_VEHICLE_FIELD(hand_brake_torque) },
	{ "longitudinal_grip", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(longitudinal_grip) },
	{ "lateral_grip", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(lateral_grip) },
	{ "tester", KeyType::Tester, 0, 0, BF3_VEHICLE_FIELD(tester) },
	{ "steer_model", KeyType::SteerModel, 0, 0, BF3_VEHICLE_FIELD(steer_model) },
	{ "steer_max_degrees", KeyType::Float, 0.0f, 89.0f, BF3_VEHICLE_FIELD(steer_max_degrees) },
	{ "engine_inertia", KeyType::Float, 0.001f, 1000.0f, BF3_VEHICLE_FIELD(engine_inertia) },
	{ "engine_angular_damping", KeyType::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(engine_angular_damping) },
	{ "shift_up_rpm", KeyType::Float, 1.0f, 100000.0f, BF3_VEHICLE_FIELD(shift_up_rpm) },
	{ "shift_down_rpm", KeyType::Float, 0.0f, 100000.0f, BF3_VEHICLE_FIELD(shift_down_rpm) },
	{ "switch_time", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(switch_time) },
	{ "clutch_release_time", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(clutch_release_time) },
	{ "switch_latency", KeyType::Float, 0.0f, 10.0f, BF3_VEHICLE_FIELD(switch_latency) },
	{ "clutch_strength", KeyType::Float, 0.0f, 10000.0f, BF3_VEHICLE_FIELD(clutch_strength) },
	{ "differential_limited_slip", KeyType::Float, 1.0f, 1e30f, BF3_VEHICLE_FIELD(differential_limited_slip) },
	{ "anti_roll_scale", KeyType::Float, 0.0f, 1e9f, BF3_VEHICLE_FIELD(anti_roll_scale) },
	{ "drag_scale", KeyType::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(drag_scale) },
	{ "max_pitch_roll_degrees", KeyType::Float, 1.0f, 180.0f, BF3_VEHICLE_FIELD(max_pitch_roll_degrees) },
	{ "stand_still_speed", KeyType::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(stand_still_speed) },
	{ "reverse_switch_speed", KeyType::Float, 0.0f, 100.0f, BF3_VEHICLE_FIELD(reverse_switch_speed) },
	{ "collision_steps", KeyType::U32, 1, 8, BF3_VEHICLE_FIELD(collision_steps) },
	{ "velocity_steps", KeyType::U32, 1, 128, BF3_VEHICLE_FIELD(velocity_steps) },
	{ "position_steps", KeyType::U32, 0, 128, BF3_VEHICLE_FIELD(position_steps) },
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
static_assert(kKeyCount == 30); // s2.contract checks the shipped file against the same 30 names

#undef BF3_VEHICLE_FIELD

constexpr size_t kMaxLines = 512;
constexpr size_t kMaxLineBytes = 256;

const char *tester_name(WheelCollisionTester p_tester) {
	switch (p_tester) {
		case WheelCollisionTester::Cylinder:
			return "cylinder";
		case WheelCollisionTester::Sphere:
			return "sphere";
		case WheelCollisionTester::Ray:
			return "ray";
	}
	return "?";
}

const char *steer_model_name(SteerAngleModel p_model) {
	switch (p_model) {
		case SteerAngleModel::ScaledLock:
			return "scaled_lock";
		case SteerAngleModel::SensitivityRadians:
			return "sensitivity_radians";
	}
	return "?";
}

std::string_view trim(std::string_view p_text) {
	while (!p_text.empty() && (p_text.front() == ' ' || p_text.front() == '\t' || p_text.front() == '\r')) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && (p_text.back() == ' ' || p_text.back() == '\t' || p_text.back() == '\r')) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

std::string shown(std::string_view p_text) {
	std::string out;
	for (size_t i = 0; i < p_text.size() && i < 40; ++i) {
		const char c = p_text[i];
		out += (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	if (p_text.size() > 40) {
		out += "...";
	}
	return out;
}

std::string number_text(float p_value) {
	char buffer[64];
	const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), p_value);
	return std::string(buffer, r.ptr);
}

} // namespace

core::Result<VehicleTuning> parse_vehicle_tuning(std::string_view p_text) {
	VehicleTuning t;
	bool seen[kKeyCount] = {};
	size_t line_no = 0;
	size_t at = 0;
	while (at <= p_text.size()) {
		const size_t end = std::min(p_text.find('\n', at), p_text.size());
		std::string_view line = p_text.substr(at, end - at);
		at = end + 1;
		++line_no;
		const std::string where = "vehicle tuning line " + std::to_string(line_no);
		if (line_no > kMaxLines) {
			return core::make_error(where + ": more than " + std::to_string(kMaxLines) + " lines");
		}
		if (line.size() > kMaxLineBytes) {
			return core::make_error(where + ": longer than " + std::to_string(kMaxLineBytes) + " bytes");
		}
		if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
			line = line.substr(0, hash);
		}
		line = trim(line);
		if (line.empty()) {
			if (end == p_text.size()) {
				break;
			}
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos) {
			return core::make_error(where + ": expected `key = value`, got `" + shown(line) + "`");
		}
		const std::string_view key = trim(line.substr(0, eq));
		const std::string_view value = trim(line.substr(eq + 1));
		size_t index = kKeyCount;
		for (size_t k = 0; k < kKeyCount; ++k) {
			if (key == kKeys[k].name) {
				index = k;
				break;
			}
		}
		if (index == kKeyCount) {
			return core::make_error(where + ": unknown key `" + shown(key) + "`");
		}
		if (seen[index]) {
			return core::make_error(where + ": `" + std::string(key) + "` given twice");
		}
		seen[index] = true;
		const KeyDef &k = kKeys[index];
		void *field = k.field(t);
		switch (k.type) {
			case KeyType::Float: {
				float v = 0;
				const std::from_chars_result r = std::from_chars(value.data(), value.data() + value.size(), v);
				if (value.empty() || r.ec != std::errc() || r.ptr != value.data() + value.size() || !std::isfinite(v)) {
					return core::make_error(where + ": `" + std::string(key) + "` needs a number, got `" + shown(value) + "`");
				}
				if (!(v >= k.min && v <= k.max)) {
					return core::make_error(where + ": `" + std::string(key) + "` = " + shown(value) + " is outside " + number_text(k.min) + ".." +
							number_text(k.max));
				}
				*static_cast<float *>(field) = v;
				break;
			}
			case KeyType::U32: {
				uint32_t v = 0;
				const std::from_chars_result r = std::from_chars(value.data(), value.data() + value.size(), v);
				if (value.empty() || r.ec != std::errc() || r.ptr != value.data() + value.size()) {
					return core::make_error(where + ": `" + std::string(key) + "` needs a whole number, got `" + shown(value) + "`");
				}
				if (float(v) < k.min || float(v) > k.max) {
					return core::make_error(where + ": `" + std::string(key) + "` = " + shown(value) + " is outside " + number_text(k.min) + ".." +
							number_text(k.max));
				}
				*static_cast<uint32_t *>(field) = v;
				break;
			}
			case KeyType::Tester: {
				bool found = false;
				for (const WheelCollisionTester c : { WheelCollisionTester::Cylinder, WheelCollisionTester::Sphere, WheelCollisionTester::Ray }) {
					if (value == tester_name(c)) {
						*static_cast<WheelCollisionTester *>(field) = c;
						found = true;
					}
				}
				if (!found) {
					return core::make_error(where + ": `tester` must be cylinder, sphere or ray, got `" + shown(value) + "`");
				}
				break;
			}
			case KeyType::SteerModel: {
				bool found = false;
				for (const SteerAngleModel m : { SteerAngleModel::ScaledLock, SteerAngleModel::SensitivityRadians }) {
					if (value == steer_model_name(m)) {
						*static_cast<SteerAngleModel *>(field) = m;
						found = true;
					}
				}
				if (!found) {
					return core::make_error(where + ": `steer_model` must be scaled_lock or sensitivity_radians, got `" + shown(value) + "`");
				}
				break;
			}
		}
		if (end == p_text.size()) {
			break;
		}
	}
	return t;
}

std::string format_vehicle_tuning(const VehicleTuning &p_tuning) {
	VehicleTuning t = p_tuning;
	std::string out;
	for (const KeyDef &k : kKeys) {
		out += k.name;
		out += " = ";
		const void *field = k.field(t);
		switch (k.type) {
			case KeyType::Float:
				out += number_text(*static_cast<const float *>(field));
				break;
			case KeyType::U32:
				out += std::to_string(*static_cast<const uint32_t *>(field));
				break;
			case KeyType::Tester:
				out += tester_name(*static_cast<const WheelCollisionTester *>(field));
				break;
			case KeyType::SteerModel:
				out += steer_model_name(*static_cast<const SteerAngleModel *>(field));
				break;
		}
		out += '\n';
	}
	return out;
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

} // namespace bf3::sim

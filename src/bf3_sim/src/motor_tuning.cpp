#include "bf3/sim/soldier_motor.h"

#include "bf3/core/gameplay/soldier_def.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

namespace bf3::sim {

static_assert(uint8_t(core::gameplay::SoldierPose::Stand) == uint8_t(Pose::Stand));
static_assert(uint8_t(core::gameplay::SoldierPose::Crouch) == uint8_t(Pose::Crouch));
static_assert(uint8_t(core::gameplay::SoldierPose::Prone) == uint8_t(Pose::Prone));
static_assert(core::gameplay::kSoldierPoseCount == kPoseCount);

namespace {

enum class KeyType : uint8_t { Float, Bool, Model, U32 };

struct KeyDef {
	const char *name;
	KeyType type;
	float min;
	float max;
	void *(*field)(MotorTuning &);
};

#define BF3_TUNING_FIELD(f) [](MotorTuning &t) -> void * { return &t.f; }

const KeyDef kKeys[] = {
	{ "gravity", KeyType::Float, 0.01f, 100.0f, BF3_TUNING_FIELD(gravity) },
	{ "reference_hz", KeyType::Float, 1.0f, 1000.0f, BF3_TUNING_FIELD(reference_hz) },
	{ "acceleration_model", KeyType::Model, 0, 0, BF3_TUNING_FIELD(acceleration_model) },
	{ "deceleration_model", KeyType::Model, 0, 0, BF3_TUNING_FIELD(deceleration_model) },
	{ "sprint_model", KeyType::Model, 0, 0, BF3_TUNING_FIELD(sprint_model) },
	{ "air_control_scale", KeyType::Float, 0.0f, 100.0f, BF3_TUNING_FIELD(air_control_scale) },
	{ "transition_uses_slower_pose", KeyType::Bool, 0, 0, BF3_TUNING_FIELD(transition_uses_slower_pose) },
	{ "jump_penalty_scales_height", KeyType::Bool, 0, 0, BF3_TUNING_FIELD(jump_penalty_scales_height) },
	{ "sprint_stands_up", KeyType::Bool, 0, 0, BF3_TUNING_FIELD(sprint_stands_up) },
	{ "prone_footprint_check", KeyType::Bool, 0, 0, BF3_TUNING_FIELD(prone_footprint_check) },
	{ "prone_footprint_height", KeyType::Float, 0.01f, 2.0f, BF3_TUNING_FIELD(prone_footprint_height) },
	{ "character_padding", KeyType::Float, 0.001f, 0.2f, BF3_TUNING_FIELD(character_padding) },
	{ "predictive_contact_distance", KeyType::Float, 0.001f, 1.0f, BF3_TUNING_FIELD(predictive_contact_distance) },
	{ "penetration_recovery_speed", KeyType::Float, 0.0f, 1.0f, BF3_TUNING_FIELD(penetration_recovery_speed) },
	{ "pose_change_max_penetration", KeyType::Float, 0.0f, 0.5f, BF3_TUNING_FIELD(pose_change_max_penetration) },
	{ "max_collision_iterations", KeyType::U32, 1, 64, BF3_TUNING_FIELD(max_collision_iterations) },
	{ "max_constraint_iterations", KeyType::U32, 1, 256, BF3_TUNING_FIELD(max_constraint_iterations) },
	{ "enhanced_internal_edge_removal", KeyType::Bool, 0, 0, BF3_TUNING_FIELD(enhanced_internal_edge_removal) },
	{ "min_landing_speed", KeyType::Float, 0.0f, 100.0f, BF3_TUNING_FIELD(min_landing_speed) },
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef BF3_TUNING_FIELD

const char *model_name(GainModel p_model) {
	switch (p_model) {
		case GainModel::FractionPerReferenceTick:
			return "fraction_per_reference_tick";
		case GainModel::Linear:
			return "linear";
		case GainModel::Instant:
			return "instant";
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

void hash_bytes(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const unsigned char *b = static_cast<const unsigned char *>(p_data);
	for (size_t i = 0; i < p_size; ++i) {
		r_hash ^= b[i];
		r_hash *= 0x100000001B3ull;
	}
}

template <typename T>
void hash_value(uint64_t &r_hash, const T &p_value) {
	static_assert(std::is_trivially_copyable_v<T>);
	unsigned char bytes[sizeof(T)];
	std::memcpy(bytes, &p_value, sizeof(T));
	hash_bytes(r_hash, bytes, sizeof(T));
}

void hash_curve(uint64_t &r_hash, const Curve &p_curve) {
	for (const CurvePoint &p : p_curve.points) {
		hash_value(r_hash, p.x);
		hash_value(r_hash, p.y);
	}
	hash_value(r_hash, p_curve.count);
}

void hash_speed(uint64_t &r_hash, const StateSpeed &p_s) {
	for (const float f : { p_s.velocity, p_s.forward, p_s.backward, p_s.left, p_s.right, p_s.acceleration_gain, p_s.deceleration_gain, p_s.sprint_gain,
				 p_s.sprint_multiplier, p_s.acceleration_factor, p_s.deceleration_factor, p_s.sprint_factor }) {
		hash_value(r_hash, f);
	}
}

void hash_tuning(uint64_t &r_hash, const MotorTuning &p_t) {
	hash_value(r_hash, p_t.gravity);
	hash_value(r_hash, p_t.reference_hz);
	hash_value(r_hash, p_t.acceleration_model);
	hash_value(r_hash, p_t.deceleration_model);
	hash_value(r_hash, p_t.sprint_model);
	hash_value(r_hash, p_t.air_control_scale);
	hash_value(r_hash, p_t.transition_uses_slower_pose);
	hash_value(r_hash, p_t.jump_penalty_scales_height);
	hash_value(r_hash, p_t.sprint_stands_up);
	hash_value(r_hash, p_t.prone_footprint_check);
	hash_value(r_hash, p_t.prone_footprint_height);
	hash_value(r_hash, p_t.character_padding);
	hash_value(r_hash, p_t.predictive_contact_distance);
	hash_value(r_hash, p_t.penetration_recovery_speed);
	hash_value(r_hash, p_t.pose_change_max_penetration);
	hash_value(r_hash, p_t.max_collision_iterations);
	hash_value(r_hash, p_t.max_constraint_iterations);
	hash_value(r_hash, p_t.enhanced_internal_edge_removal);
	hash_value(r_hash, p_t.min_landing_speed);
}

core::Status validate_tuning(const MotorTuning &p_tuning) {
	MotorTuning t = p_tuning;
	for (const KeyDef &k : kKeys) {
		void *field = k.field(t);
		if (k.type == KeyType::Float) {
			const float v = *static_cast<float *>(field);
			if (!(v >= k.min && v <= k.max)) {
				return core::make_error(std::string("motor tuning: ") + k.name + " = " + number_text(v) + " is outside " + number_text(k.min) + ".." +
						number_text(k.max));
			}
		} else if (k.type == KeyType::U32) {
			const uint32_t v = *static_cast<uint32_t *>(field);
			if (float(v) < k.min || float(v) > k.max) {
				return core::make_error(std::string("motor tuning: ") + k.name + " = " + std::to_string(v) + " is outside " + number_text(k.min) + ".." +
						number_text(k.max));
			}
		} else if (k.type == KeyType::Model) {
			const uint8_t v = uint8_t(*static_cast<GainModel *>(field));
			if (v > uint8_t(GainModel::Instant)) {
				return core::make_error(std::string("motor tuning: ") + k.name + " has no model " + std::to_string(v));
			}
		}
	}
	return core::Status::success();
}

float gain_factor(GainModel p_model, float p_gain, float p_reference_hz) {
	switch (p_model) {
		case GainModel::FractionPerReferenceTick: {
			const float g = std::fmin(std::fmax(std::fabs(p_gain), 0.0f), 1.0f);
			return float(1.0 - std::pow(1.0 - double(g), double(p_reference_hz) * double(kTickSeconds)));
		}
		case GainModel::Linear:
			return std::fabs(p_gain) * kTickSeconds;
		case GainModel::Instant:
			return 1.0f;
	}
	return 1.0f;
}

core::Result<Curve> to_curve(const std::vector<core::gameplay::CurvePoint> &p_points, const char *p_what) {
	Curve c;
	if (p_points.size() > kMaxCurvePoints) {
		return core::make_error(std::string("motor params: ") + p_what + " has " + std::to_string(p_points.size()) + " points (at most " +
				std::to_string(kMaxCurvePoints) + ")");
	}
	for (size_t i = 0; i < p_points.size(); ++i) {
		if (!std::isfinite(p_points[i].x) || !std::isfinite(p_points[i].y)) {
			return core::make_error(std::string("motor params: ") + p_what + " has a non-finite point");
		}
		c.points[i] = { p_points[i].x, p_points[i].y };
	}
	c.count = uint8_t(p_points.size());
	return c;
}

StateSpeed to_speed(const core::gameplay::StatePoseSpeed &p_s) {
	StateSpeed s;
	s.velocity = p_s.velocity;
	s.forward = p_s.forward;
	s.backward = p_s.backward;
	s.left = p_s.left;
	s.right = p_s.right;
	s.acceleration_gain = p_s.acceleration_gain;
	s.deceleration_gain = p_s.deceleration_gain;
	s.sprint_gain = p_s.sprint_gain;
	s.sprint_multiplier = p_s.sprint_multiplier;
	return s;
}

bool speed_finite(const StateSpeed &p_s) {
	for (const float f : { p_s.velocity, p_s.forward, p_s.backward, p_s.left, p_s.right, p_s.acceleration_gain, p_s.deceleration_gain, p_s.sprint_gain,
				 p_s.sprint_multiplier }) {
		if (!std::isfinite(f) || std::fabs(f) > 1000.0f) {
			return false;
		}
	}
	return p_s.velocity >= 0.0f;
}

void set_factors(StateSpeed &r_s, const MotorTuning &p_t, float p_accel_scale) {
	r_s.acceleration_factor = gain_factor(p_t.acceleration_model, r_s.acceleration_gain * p_accel_scale, p_t.reference_hz);
	r_s.deceleration_factor = gain_factor(p_t.deceleration_model, r_s.deceleration_gain, p_t.reference_hz);
	r_s.sprint_factor = gain_factor(p_t.sprint_model, r_s.sprint_gain * p_accel_scale, p_t.reference_hz);
}

std::optional<float> scalar_of(const core::gameplay::SoldierDef &p_def, std::string_view p_state, std::string_view p_name) {
	const core::gameplay::MovementStateDef *s = p_def.state(p_state);
	const std::optional<double> v = s ? s->scalar(p_name) : std::nullopt;
	if (!v || !std::isfinite(*v)) {
		return std::nullopt;
	}
	return float(*v);
}

}

core::Result<MotorTuning> parse_motor_tuning(std::string_view p_text) {
	MotorTuning t;
	bool seen[kKeyCount] = {};
	size_t line_no = 0;
	size_t at = 0;
	while (at <= p_text.size()) {
		const size_t end = std::min(p_text.find('\n', at), p_text.size());
		std::string_view line = p_text.substr(at, end - at);
		at = end + 1;
		++line_no;
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
		const std::string where = "motor tuning line " + std::to_string(line_no);
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
			case KeyType::Bool:
				if (value == "true") {
					*static_cast<bool *>(field) = true;
				} else if (value == "false") {
					*static_cast<bool *>(field) = false;
				} else {
					return core::make_error(where + ": `" + std::string(key) + "` needs true or false, got `" + shown(value) + "`");
				}
				break;
			case KeyType::Model: {
				GainModel m = GainModel::Instant;
				bool found = false;
				for (const GainModel candidate : { GainModel::FractionPerReferenceTick, GainModel::Linear, GainModel::Instant }) {
					if (value == model_name(candidate)) {
						m = candidate;
						found = true;
					}
				}
				if (!found) {
					return core::make_error(where + ": `" + std::string(key) + "` needs fraction_per_reference_tick, linear or instant, got `" + shown(value) +
							"`");
				}
				*static_cast<GainModel *>(field) = m;
				break;
			}
		}
		if (end == p_text.size()) {
			break;
		}
	}
	return t;
}

std::string format_motor_tuning(const MotorTuning &p_tuning) {
	MotorTuning t = p_tuning;
	std::string out;
	for (const KeyDef &k : kKeys) {
		out += k.name;
		out += " = ";
		void *field = k.field(t);
		switch (k.type) {
			case KeyType::Float:
				out += number_text(*static_cast<float *>(field));
				break;
			case KeyType::U32:
				out += std::to_string(*static_cast<uint32_t *>(field));
				break;
			case KeyType::Bool:
				out += *static_cast<bool *>(field) ? "true" : "false";
				break;
			case KeyType::Model:
				out += model_name(*static_cast<GainModel *>(field));
				break;
		}
		out += '\n';
	}
	return out;
}

float eval(const Curve &p_curve, float p_x) {
	const size_t n = std::min<size_t>(p_curve.count, kMaxCurvePoints);
	if (n == 0) {
		return 0;
	}
	if (p_x <= p_curve.points[0].x) {
		return p_curve.points[0].y;
	}
	for (size_t i = 1; i < n; ++i) {
		const CurvePoint &a = p_curve.points[i - 1];
		const CurvePoint &b = p_curve.points[i];
		if (p_x <= b.x) {
			const float span = b.x - a.x;
			if (!(span > 0)) {
				return b.y;
			}
			return a.y + (b.y - a.y) * ((p_x - a.x) / span);
		}
	}
	return p_curve.points[n - 1].y;
}

core::Result<MotorParams> make_motor_params(const core::gameplay::SoldierDef &p_def, const MotorTuning &p_tuning) {
	using core::gameplay::SoldierPose;
	BF3_TRY(validate_tuning(p_tuning));
	MotorParams p;
	p.tuning = p_tuning;
	auto finite_positive = [](float p_v) { return std::isfinite(p_v) && p_v > 0.0f && p_v < 1000.0f; };
	if (!finite_positive(p_def.radius)) {
		return core::make_error("motor params: PhysicalRadius " + number_text(p_def.radius) + " is not a usable radius");
	}
	p.radius = p_def.radius;
	p.mass = finite_positive(p_def.mass) ? p_def.mass : 100.0f;
	if (!(std::isfinite(p_def.max_ascend_angle) && p_def.max_ascend_angle > 0.0f && p_def.max_ascend_angle < 90.0f)) {
		return core::make_error("motor params: MaxAscendAngle " + number_text(p_def.max_ascend_angle) + " is outside 0..90");
	}
	p.max_ascend_degrees = p_def.max_ascend_angle;
	if (const std::optional<float> d = scalar_of(p_def, "OnGroundStateData", "AllowedDistanceFromGround"); d && *d >= 0.0f && *d <= 10.0f) {
		p.allowed_distance_from_ground = *d;
	}
	if (const std::optional<float> h = scalar_of(p_def, "OnGroundStateData", "GroundHugging")) {
		p.ground_hugging = *h != 0.0f;
	}
	const std::optional<float> jump = p_def.jump_height();
	if (!jump || !(*jump >= 0.0f && *jump <= 100.0f)) {
		return core::make_error("motor params: no JumpStateData.JumpHeight");
	}
	p.jump_height = *jump;
	if (std::isfinite(p_def.jump_penalty_time) && p_def.jump_penalty_time >= 0.0f && p_def.jump_penalty_time <= 60.0f) {
		p.jump_penalty_ticks = uint16_t(seconds_to_ticks(p_def.jump_penalty_time));
	}
	if (std::isfinite(p_def.jump_penalty_factor) && p_def.jump_penalty_factor >= 0.0f && p_def.jump_penalty_factor <= 1.0f) {
		p.jump_penalty_factor = p_def.jump_penalty_factor;
	}
	if (const std::optional<float> f = scalar_of(p_def, "InAirStateData", "FreeFallVelocity"); f && *f > 0.0f && *f < 1000.0f) {
		p.free_fall_velocity = *f;
	}
	if (finite_positive(p_def.body_sprint_multiplier)) {
		p.body_sprint_multiplier = p_def.body_sprint_multiplier;
	}
	if (finite_positive(p_def.max_health)) {
		p.max_health = p_def.max_health;
	}
	if (std::isfinite(p_def.immortal_time_after_spawn) && p_def.immortal_time_after_spawn >= 0.0f && p_def.immortal_time_after_spawn <= 600.0f) {
		p.immortal_ticks = uint16_t(seconds_to_ticks(p_def.immortal_time_after_spawn));
	}
	if (p_def.fall_damage_vertical.empty()) {
		return core::make_error("motor params: no CollisionData fall-damage curve");
	}
	core::Result<Curve> vertical = to_curve(p_def.fall_damage_vertical, "the vertical fall-damage curve");
	BF3_TRY(vertical);
	p.fall_damage_vertical = *vertical;
	core::Result<Curve> horizontal = to_curve(p_def.fall_damage_horizontal, "the horizontal fall-damage curve");
	BF3_TRY(horizontal);
	p.fall_damage_horizontal = *horizontal;

	const core::gameplay::MovementStateDef *on_ground = p_def.state("OnGroundStateData");
	const core::gameplay::StatePoseSpeed *stand_ground = on_ground ? on_ground->pose(SoldierPose::Stand) : nullptr;
	if (stand_ground == nullptr) {
		return core::make_error("motor params: no OnGroundStateData speed for Stand");
	}
	const core::gameplay::MovementStateDef *in_air = p_def.state("InAirStateData");
	const core::gameplay::MovementStateDef *falling = p_def.state("FallingStateData");
	const core::gameplay::MovementStateDef *jumping = p_def.state("JumpStateData");
	for (size_t i = 0; i < kPoseCount; ++i) {
		const SoldierPose pose = SoldierPose(i);
		const core::gameplay::PoseDef *def = p_def.pose(pose);
		if (def == nullptr) {
			return core::make_error(std::string("motor params: no CharacterPoseData for ") + core::gameplay::soldier_pose_name(pose));
		}
		PoseParams &pp = p.poses[i];
		if (!(std::isfinite(def->height) && def->height > 0.0f && def->height < 10.0f)) {
			return core::make_error(std::string("motor params: pose ") + core::gameplay::soldier_pose_name(pose) + " height " + number_text(def->height) +
					" is unusable");
		}
		pp.height = def->height;
		pp.eye_height = std::isfinite(def->eye_height) ? def->eye_height : def->height;
		pp.step_height = std::isfinite(def->step_height) && def->step_height >= 0.0f && def->step_height <= 2.0f ? def->step_height : 0.0f;
		pp.box_min_expand = def->box_min_expand;
		pp.box_max_expand = def->box_max_expand;
		for (float &f : pp.box_min_expand) {
			f = std::isfinite(f) ? std::fmin(std::fabs(f), 10.0f) : 0.0f;
		}
		for (float &f : pp.box_max_expand) {
			f = std::isfinite(f) ? std::fmin(std::fabs(f), 10.0f) : 0.0f;
		}
		pp.min_look_pitch = def->min_look_pitch;
		pp.max_look_pitch = def->max_look_pitch;
		if (def->throttle_curve.empty()) {
			pp.throttle.points[0] = { 0.0f, 0.0f };
			pp.throttle.points[1] = { 1.0f, 1.0f };
			pp.throttle.count = 2;
		} else {
			core::Result<Curve> throttle = to_curve(def->throttle_curve, "a throttle curve");
			BF3_TRY(throttle);
			pp.throttle = *throttle;
		}
		for (size_t q = 0; q < kPoseCount; ++q) {
			const std::optional<float> &seconds = def->transition_to[q];
			if (q != i && seconds && std::isfinite(*seconds) && *seconds >= 0.0f && *seconds <= 60.0f) {
				pp.transition_ticks[q] = uint16_t(seconds_to_ticks(*seconds));
			}
		}
		const core::gameplay::StatePoseSpeed *ground = on_ground->pose(pose);
		pp.on_ground = to_speed(ground != nullptr ? *ground : *stand_ground);

		auto state_speed = [&](const core::gameplay::MovementStateDef *p_state) {
			const core::gameplay::StatePoseSpeed *s = p_state ? p_state->pose(pose) : nullptr;
			if (s == nullptr && p_state != nullptr) {
				s = p_state->pose(SoldierPose::Stand);
			}
			if (s != nullptr) {
				return to_speed(*s);
			}
			StateSpeed fallback = pp.on_ground;
			fallback.acceleration_gain = 0;
			fallback.deceleration_gain = 0;
			fallback.sprint_gain = 0;
			fallback.sprint_multiplier = 0;
			return fallback;
		};
		pp.in_air = state_speed(in_air);
		pp.falling = state_speed(falling);
		pp.jumping = state_speed(jumping);
		for (const StateSpeed *s : { &pp.on_ground, &pp.in_air, &pp.falling, &pp.jumping }) {
			if (!speed_finite(*s)) {
				return core::make_error(std::string("motor params: a speed of pose ") + core::gameplay::soldier_pose_name(pose) + " is not finite");
			}
		}
		set_factors(pp.on_ground, p.tuning, 1.0f);
		set_factors(pp.in_air, p.tuning, p.tuning.air_control_scale);
		set_factors(pp.falling, p.tuning, p.tuning.air_control_scale);
		set_factors(pp.jumping, p.tuning, p.tuning.air_control_scale);
	}
	if (p.poses[size_t(Pose::Stand)].height < 2.0f * p.radius || p.poses[size_t(Pose::Crouch)].height < 2.0f * p.radius) {
		return core::make_error("motor params: the stand or crouch height is below the capsule diameter");
	}
	return p;
}

uint64_t motor_params_hash(const MotorParams &p_params) {
	uint64_t h = 0xCBF29CE484222325ull;
	hash_value(h, p_params.radius);
	hash_value(h, p_params.mass);
	hash_value(h, p_params.max_ascend_degrees);
	hash_value(h, p_params.allowed_distance_from_ground);
	hash_value(h, p_params.ground_hugging);
	hash_value(h, p_params.jump_height);
	hash_value(h, p_params.jump_penalty_ticks);
	hash_value(h, p_params.jump_penalty_factor);
	hash_value(h, p_params.free_fall_velocity);
	hash_value(h, p_params.body_sprint_multiplier);
	hash_value(h, p_params.max_health);
	hash_value(h, p_params.immortal_ticks);
	hash_curve(h, p_params.fall_damage_vertical);
	hash_curve(h, p_params.fall_damage_horizontal);
	for (const PoseParams &pp : p_params.poses) {
		hash_value(h, pp.height);
		hash_value(h, pp.eye_height);
		hash_value(h, pp.step_height);
		for (const float f : pp.box_min_expand) {
			hash_value(h, f);
		}
		for (const float f : pp.box_max_expand) {
			hash_value(h, f);
		}
		hash_value(h, pp.min_look_pitch);
		hash_value(h, pp.max_look_pitch);
		hash_curve(h, pp.throttle);
		for (const uint16_t t : pp.transition_ticks) {
			hash_value(h, t);
		}
		hash_speed(h, pp.on_ground);
		hash_speed(h, pp.in_air);
		hash_speed(h, pp.falling);
		hash_speed(h, pp.jumping);
	}
	hash_tuning(h, p_params.tuning);

	if (p_params.protect_abort_buttons != 0 || p_params.protect_abort_move) {
		hash_value(h, p_params.protect_abort_buttons);
		hash_value(h, uint8_t(p_params.protect_abort_move ? 1 : 0));
	}
	return h;
}

}

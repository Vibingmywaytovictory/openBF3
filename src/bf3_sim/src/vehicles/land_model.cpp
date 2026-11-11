#include "land_model.h"

#include "bf3/core/util/finite.h"

#include <cmath>
#include <string>

namespace bf3::sim {

namespace {

using bf3::core::finite;

core::Status axis_rule(const core::gameplay::InputAxisDef &p_def, const char *p_what, vehicles::InputAxisRule &r_axis) {
	const float durations[] = { p_def.inertia_in, p_def.inertia_out };
	for (const float d : durations) {
		if (!finite(d) || d < 0.0f || d > 60.0f) {
			return core::make_error(std::string(p_what) + " inertia duration outside 0..60 s");
		}
	}
	if (!finite(p_def.deadzone) || p_def.deadzone < 0.0f || p_def.deadzone > 1.0f || !finite(p_def.inertia_min_ratio) ||
			p_def.inertia_min_ratio < 0.0f || p_def.inertia_min_ratio > 1.0f) {
		return core::make_error(std::string(p_what) + " dead zone or inertia min ratio outside 0..1");
	}
	r_axis = { p_def.deadzone, p_def.inertia_in, p_def.inertia_out, p_def.inertia_min_ratio };
	return core::Status::success();
}

core::Status gear_list(const std::vector<float> &p_values, bool p_reverse, bool p_ratios,
		std::array<float, kMaxModelGears> &r_out, uint8_t &r_count) {
	const char *what = p_ratios ? (p_reverse ? "reverse gear ratio" : "forward gear ratio")
								: (p_reverse ? "reverse gear speed" : "forward gear speed");
	if (p_values.size() > kMaxModelGears) {
		return core::make_error(
				std::string("more than ") + std::to_string(kMaxModelGears) + " entries in " + what + "s");
	}
	for (size_t i = 0; i < p_values.size(); ++i) {
		const float v = p_values[i];
		const bool ok = p_ratios ? (p_reverse ? (v < 0.0f && v > -1000.0f) : (v > 0.0f && v < 1000.0f))
								 : (finite(v) && v >= 0.0f && v < 10000.0f);
		if (!ok) {
			return core::make_error(std::string(what) + " out of range");
		}
		r_out[i] = v;
	}
	r_count = uint8_t(p_values.size());
	return core::Status::success();
}

}

vehicles::GearboxRule land_gearbox_rule(const LandDriveModel &p_land) {
	vehicles::GearboxRule r;
	r.forward_speeds = std::span<const float>(p_land.forward_speeds.data(), p_land.forward_speed_count);
	r.reverse_speeds = std::span<const float>(p_land.reverse_speeds.data(), p_land.reverse_speed_count);
	r.forward_gears = p_land.forward_gears;
	r.reverse_gears = p_land.reverse_gears;
	r.gearbox_type = p_land.gearbox_type;
	r.gearbox_mode = p_land.gearbox_mode;
	r.gear_change_time = p_land.gear_change_time;
	r.gear_down_speed_factor = p_land.gear_down_speed_factor;
	r.opposite_dir_gear_change_max_speed = p_land.opposite_dir_gear_change_max_speed;
	r.opposite_dir_gear_change_time = p_land.opposite_dir_gear_change_time;
	r.clutch_speed_factor = p_land.clutch_speed_factor;
	r.use_neutral_gear = p_land.use_neutral_gear;
	r.use_classic_auto_clutch = p_land.use_classic_auto_clutch;
	return r;
}

float land_gear_ratio(const LandDriveModel &p_land, int32_t p_gear) {
	if (p_gear > 0 && p_gear <= int32_t(p_land.forward_gears)) {
		return p_land.forward_ratios[size_t(p_gear) - 1];
	}
	if (p_gear < 0 && -p_gear <= int32_t(p_land.reverse_gears)) {
		return p_land.reverse_ratios[size_t(-p_gear) - 1];
	}
	return 0.0f;
}

core::Status build_land_drive(const core::gameplay::VehicleDriveDef &p_def, LandDriveModel &r_land) {
	const core::gameplay::ChassisConfigDef &ch = p_def.chassis;
	BF3_TRY(axis_rule(ch.throttle, "throttle", r_land.throttle));
	BF3_TRY(axis_rule(ch.brake, "brake", r_land.brake));
	BF3_TRY(axis_rule(ch.yaw, "yaw", r_land.yaw));

	const core::gameplay::GearboxDef &gb = p_def.gearbox;
	if (!gb.present || gb.forward_ratios.empty()) {
		return core::make_error("no gearbox");
	}
	BF3_TRY(gear_list(gb.forward_ratios, false, true, r_land.forward_ratios, r_land.forward_gears));
	BF3_TRY(gear_list(gb.reverse_ratios, true, true, r_land.reverse_ratios, r_land.reverse_gears));
	BF3_TRY(gear_list(gb.forward_speeds, false, false, r_land.forward_speeds, r_land.forward_speed_count));
	BF3_TRY(gear_list(gb.reverse_speeds, true, false, r_land.reverse_speeds, r_land.reverse_speed_count));
	const float times[] = { gb.gear_change_time, gb.opposite_dir_gear_change_time };
	for (const float t : times) {
		if (!finite(t) || t < 0.0f || t > 60.0f) {
			return core::make_error("gear change time outside 0..60 s");
		}
	}
	const float factors[] = { gb.gear_down_speed_factor, gb.clutch_speed_factor, gb.opposite_dir_gear_change_max_speed,
		gb.backward_throttle_limit };
	for (const float f : factors) {
		if (!finite(f) || f < 0.0f || f > 10000.0f) {
			return core::make_error("gearbox factor outside 0..10000");
		}
	}
	if (!finite(gb.transmission_efficiency) || gb.transmission_efficiency < 0.0f ||
			gb.transmission_efficiency > 10.0f) {
		return core::make_error("transmission efficiency outside 0..10");
	}
	r_land.gearbox_type = gb.gearbox_type;
	r_land.gearbox_mode = gb.gearbox_mode;
	r_land.gear_change_time = gb.gear_change_time;
	r_land.gear_down_speed_factor = gb.gear_down_speed_factor;
	r_land.clutch_speed_factor = gb.clutch_speed_factor;
	r_land.opposite_dir_gear_change_max_speed = gb.opposite_dir_gear_change_max_speed;
	r_land.opposite_dir_gear_change_time = gb.opposite_dir_gear_change_time;
	r_land.transmission_efficiency = gb.transmission_efficiency;
	r_land.use_neutral_gear = gb.use_neutral_gear;
	r_land.use_classic_auto_clutch = gb.use_classic_auto_clutch;
	r_land.limit_backward_throttle = gb.limit_backward_throttle;
	r_land.backward_throttle_limit = gb.backward_throttle_limit;

	const core::gameplay::EngineDef *engine = nullptr;
	for (const core::gameplay::EngineDef &e : p_def.engines) {
		if (!e.rpm_points.empty()) {
			engine = &e;
			break;
		}
	}
	if (engine == nullptr) {
		return core::make_error("no combustion engine with a torque curve");
	}
	if (engine->rpm_points.size() != engine->torque_points.size() || engine->rpm_points.size() > kMaxModelCurvePoints) {
		return core::make_error("engine curve has " + std::to_string(engine->rpm_points.size()) + " points (at most " +
				std::to_string(kMaxModelCurvePoints) + ", equal rpm and torque counts)");
	}
	for (size_t i = 0; i < engine->rpm_points.size(); ++i) {
		const float rpm = engine->rpm_points[i];
		const float torque = engine->torque_points[i];
		if (!finite(rpm) || rpm < 0.0f || (i > 0 && !(rpm > engine->rpm_points[i - 1]))) {
			return core::make_error("engine rpm points not strictly ascending");
		}
		if (!finite(torque) || torque < 0.0f || torque > 1e6f) {
			return core::make_error("engine torque point out of range");
		}
		r_land.rpm_points[i] = rpm;
		r_land.torque_points[i] = torque;
	}
	r_land.curve_points = uint8_t(engine->rpm_points.size());
	if (!(engine->rpm_min >= 0.0f && engine->rpm_max > engine->rpm_min && engine->rpm_max < 100000.0f)) {
		return core::make_error("engine rpm limits out of range");
	}
	const float rates[] = { engine->internal_acceleration, engine->internal_deceleration, engine->power_multiplier };
	for (const float r : rates) {
		if (!finite(r) || r < 0.0f || r > 1e4f) {
			return core::make_error("engine acceleration, deceleration or power multiplier outside 0..1e4");
		}
	}
	r_land.engine = { engine->rpm_min, engine->rpm_max, engine->internal_acceleration, engine->internal_deceleration,
		gb.gearbox_mode };
	r_land.power_multiplier = engine->power_multiplier;
	return core::Status::success();
}

bool land_driver_state_valid(const VehicleModel &p_model, const VehicleDriverState &p_driver) {
	const vehicles::GearboxState &g = p_driver.gearbox;
	const uint8_t phase = uint8_t(g.phase);
	const bool phase_ok =
			phase <= uint8_t(vehicles::GearPhase::Reengage) || phase == uint8_t(vehicles::GearPhase::OppositeDirection);
	return std::isfinite(p_driver.engine_rpm) && p_driver.engine_rpm >= 0.0f &&
			p_driver.engine_rpm <= p_model.land.engine.rpm_max && phase_ok &&
			g.gear >= -int32_t(p_model.land.reverse_gears) && g.gear <= int32_t(p_model.land.forward_gears) &&
			g.pending >= -1 && g.pending <= 1 && g.reverse_direction <= 1 && std::isfinite(g.clutch) &&
			g.clutch >= 0.0f && g.clutch <= 1.0f && std::isfinite(g.timer) && g.timer >= -1.0f && g.timer <= 60.0f;
}

}

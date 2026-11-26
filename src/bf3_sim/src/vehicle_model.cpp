#include "bf3/sim/vehicle_model.h"

#include "bf3/core/gameplay/vehicle_drive.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/vehicles/tracked_rules.h"
#include "vehicles/land_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>

namespace bf3::sim {

namespace {

using bf3::core::finite;

class Digest {
public:
	template <typename T>
	void add(const T &p_value) {
		static_assert(std::is_trivially_copyable_v<T>);
		unsigned char bytes[sizeof(T)];
		std::memcpy(bytes, &p_value, sizeof(T));
		bytes_(bytes, sizeof(T));
	}
	void add_text(const std::string &p_text) {
		add(uint64_t(p_text.size()));
		bytes_(p_text.data(), p_text.size());
	}
	uint64_t value() const { return hash_; }

private:
	void bytes_(const void *p_data, size_t p_size) {
		const unsigned char *b = static_cast<const unsigned char *>(p_data);
		for (size_t i = 0; i < p_size; ++i) {
			hash_ ^= b[i];
			hash_ *= 0x100000001B3ull;
		}
	}
	uint64_t hash_ = 0xCBF29CE484222325ull;
};

uint64_t digest_of(const VehicleModel &p_m) {
	Digest d;
	d.add_text(p_m.blueprint);
	d.add(p_m.mass);
	d.add(p_m.gravity_factor);
	d.add(p_m.center_of_mass);
	d.add(p_m.inertia_modifier);
	d.add(p_m.center_of_mass_handling_offset);
	d.add(p_m.friction);
	d.add(p_m.restitution);
	d.add(p_m.linear_damping);
	d.add(p_m.angular_damping);
	d.add(uint64_t(p_m.chassis.size()));
	for (const ChassisPiece &p : p_m.chassis) {
		d.add(uint64_t(p.points.size()));
		for (const Vec3 &v : p.points) {
			d.add(v);
		}
		d.add(p.convex_radius);
	}
	d.add(p_m.wheel_count);
	d.add(p_m.wheel_list);
	for (size_t i = 0; i < p_m.wheel_count; ++i) {
		const VehicleWheelModel &w = p_m.wheels[i];
		d.add(w.attachment);
		d.add(w.data_centre);
		for (const float f : { w.radius, w.width, w.suspension_min, w.suspension_max, w.spring_constant,
					 w.spring_damping, w.progressive_start, w.progressive_exponent, w.rest_compression, w.inertia,
					 w.mass, w.brake_factor, w.hand_brake_factor, w.diff_gear_ratio, w.rolling_base,
					 w.rolling_velocity_kmh, w.fx_torque_multiplier, w.longitudinal_force_max, w.lateral_force_max,
					 w.lattitude_brake_scale, w.constant_spring_force_friction_scale, w.slope_grip_min,
					 w.slope_grip_max, w.grip.angular_velocity_min_deg, w.grip.side_slip_angle_max_deg,
					 w.grip.wheel_velocity_x_min_deg, w.grip.slip_ratio_max, w.tyre.lateral_pos_k, w.tyre.lateral_neg_k,
					 w.tyre.longitudinal_pos_k, w.tyre.longitudinal_neg_k }) {
			d.add(f);
		}
		for (const uint8_t b : { w.steers, w.driven, w.axle, w.side, w.track_steer, w.rolling_resistance,
					 w.engine_brake, w.low_speed_auto_brake, uint8_t(w.grip.always_grip),
					 uint8_t(w.tyre.allowed_to_spin), w.longitude_scale_points, w.lattitude_scale_points }) {
			d.add(b);
		}
		d.add(w.slope_grip_exponent);
		for (size_t k = 0; k < w.longitude_scale_points; ++k) {
			d.add(w.longitude_scale[k]);
		}
		for (size_t k = 0; k < w.lattitude_scale_points; ++k) {
			d.add(w.lattitude_scale[k]);
		}
	}
	const LandDriveModel &l = p_m.land;
	for (const vehicles::InputAxisRule &a : { l.throttle, l.brake, l.yaw }) {
		for (const float f : { a.deadzone, a.inertia_in, a.inertia_out, a.inertia_min_ratio }) {
			d.add(f);
		}
	}
	d.add(l.forward_ratios);
	d.add(l.reverse_ratios);
	d.add(l.forward_speeds);
	d.add(l.reverse_speeds);
	for (const uint8_t b : { l.forward_gears, l.reverse_gears, l.forward_speed_count, l.reverse_speed_count,
				 uint8_t(l.use_neutral_gear), uint8_t(l.use_classic_auto_clutch), uint8_t(l.limit_backward_throttle),
				 l.curve_points }) {
		d.add(b);
	}
	d.add(l.gearbox_type);
	d.add(l.gearbox_mode);
	d.add(l.engine.gearbox_mode);
	for (const float f : { l.gear_change_time, l.gear_down_speed_factor, l.clutch_speed_factor,
				 l.opposite_dir_gear_change_max_speed, l.opposite_dir_gear_change_time, l.transmission_efficiency,
				 l.backward_throttle_limit, l.engine.rpm_min, l.engine.rpm_max, l.engine.internal_acceleration,
				 l.engine.internal_deceleration, l.power_multiplier }) {
		d.add(f);
	}
	d.add(l.rpm_points);
	d.add(l.torque_points);
	d.add(l.track_forward_speed_limit);
	d.add(l.track_turn_speed_limit);
	d.add(l.steer_inertia_points);
	for (size_t i = 0; i < l.steer_inertia_points; ++i) {
		d.add(l.steer_inertia[i]);
	}
	d.add(l.transmission_loss_points);
	for (size_t i = 0; i < l.transmission_loss_points; ++i) {
		d.add(l.transmission_loss[i]);
	}
	d.add(p_m.steer_points);
	for (size_t i = 0; i < p_m.steer_points; ++i) {
		d.add(p_m.steer_sensitivity[i]);
	}
	d.add(p_m.steer_inverted);
	const vehicles::MotionDampingRule &md = p_m.motion_damping;
	for (const float f : { md.pitch, md.yaw, md.roll, md.linear }) {
		d.add(f);
	}
	d.add(md.linear_modifier);
	d.add(p_m.wind_resistance);
	const vehicles::WindRule &wr = p_m.wind;
	for (const float f : { wr.coefficient_of_air_friction, wr.air_drag_area, wr.air_density, wr.base_factor,
				 wr.velocity_factor_kmh, wr.velocity_factor_min, wr.velocity_factor_max }) {
		d.add(f);
	}
	d.add(p_m.down_force);
	d.add(p_m.down_force_wheel_factor);
	d.add(p_m.down_force_base);
	d.add(p_m.down_force_per_wheel);
	d.add(p_m.anti_roll_front);
	d.add(p_m.anti_roll_rear);
	d.add(p_m.stand_still_brake);
	d.add(p_m.stand_still_ticks);
	d.add(p_m.stand_still_speed);
	d.add(p_m.tester);
	d.add(p_m.max_pitch_roll_angle);
	d.add_text(format_vehicle_tuning(p_m.tuning));
	return d.value();
}

}

float land_balance_speed_kmh(const VehicleModel &p_model) {
	const LandDriveModel &land = p_model.land;
	const VehicleWheelModel *w = nullptr;
	for (size_t i = 0; i < p_model.wheel_count && w == nullptr; ++i) {
		w = p_model.wheels[i].driven != 0 ? &p_model.wheels[i] : nullptr;
	}
	if (w == nullptr || land.forward_gears == 0) {
		return 0.0f;
	}
	const std::span<const float> rpm_points(land.rpm_points.data(), land.curve_points);
	const std::span<const float> torque_points(land.torque_points.data(), land.curve_points);
	const std::span<const vehicles::CurvePoint> loss(land.transmission_loss.data(), land.transmission_loss_points);
	const auto torque_from = [&](float p_rpm) {
		float most = std::max(vehicles::torque_curve_at(p_rpm, rpm_points, torque_points),
				vehicles::torque_curve_at(land.engine.rpm_max, rpm_points, torque_points));
		for (size_t i = 0; i < rpm_points.size(); ++i) {
			if (rpm_points[i] >= p_rpm && rpm_points[i] <= land.engine.rpm_max) {
				most = std::max(most, torque_points[i]);
			}
		}
		return most * land.power_multiplier;
	};
	for (uint32_t step = 1; step < kBalanceSpeedSteps; ++step) {
		const float kmh = float(step) * kBalanceSpeedStepKmh;
		int32_t gear = land.forward_gears;
		for (int32_t g = 1; g < int32_t(land.forward_gears); ++g) {
			if (kmh <= land.forward_speeds[size_t(g) - 1]) {
				gear = g;
				break;
			}
		}
		const float ratio = land_gear_ratio(land, gear);
		const float v = kmh / vehicles::kKmhPerMs;
		const float spin = v / w->radius;
		const float torque = torque_from(std::min(vehicles::wheel_rpm(std::span<const float>(&spin, 1), ratio,
														  w->diff_gear_ratio, land.transmission_efficiency),
				land.engine.rpm_max));
		const float drive = w->track_steer != 0
				? vehicles::track_forward_term(torque, ratio)
				: vehicles::wheel_drive_torque(torque, ratio, w->diff_gear_ratio, land.transmission_efficiency, 1.0f);
		const float rolling = w->rolling_resistance != 0
				? vehicles::rolling_resistance(
						  spin, w->rolling_base, w->rolling_velocity_kmh, w->width, w->mass, p_model.wheel_list)
				: 0.0f;
		if (vehicles::transmission_loss_resistance(loss, v, 1.0f, p_model.mass, p_model.wheel_list) + rolling >=
				drive) {
			return kmh;
		}
	}
	return INFINITY;
}

float vehicle_steer_sensitivity(const VehicleModel &p_model, float p_speed) {
	return vehicles::steer_sensitivity(
			std::span<const vehicles::CurvePoint>(p_model.steer_sensitivity.data(), p_model.steer_points), p_speed,
			p_model.steer_inverted);
}

core::Result<std::shared_ptr<const VehicleModel>> build_vehicle_model(const core::gameplay::VehicleDriveDef &p_def,
		std::span<const ChassisPieceSource> p_chassis, const VehicleTuning &p_tuning) {
	const std::string where = "vehicle model " + p_def.blueprint;
	const auto fail = [&](const std::string &p_what) { return core::make_error(where + ": " + p_what); };
	auto m = std::make_shared<VehicleModel>();
	m->blueprint = p_def.blueprint;
	m->tuning = p_tuning;

	const core::gameplay::ChassisConfigDef &ch = p_def.chassis;
	const core::gameplay::DriveRigidBodyDef *collision = nullptr;
	for (const core::gameplay::DriveRigidBodyDef &b : p_def.rigid_bodies) {
		if (b.body_type == "RBTypeCollision") {
			collision = &b;
			break;
		}
	}
	m->mass = ch.body_mass > 0.0f ? ch.body_mass : (collision != nullptr ? collision->mass : 0.0f);
	if (!finite(m->mass) || m->mass < 1.0f || m->mass > 1e6f) {
		return fail("mass " + std::to_string(m->mass) + " outside 1..1e6 kg");
	}
	m->gravity_factor = ch.gravity_modifier;
	if (!finite(m->gravity_factor) || m->gravity_factor < 0.0f || m->gravity_factor > 10.0f) {
		return fail("gravity modifier outside 0..10");
	}
	m->center_of_mass = { ch.center_of_mass[0], ch.center_of_mass[1], ch.center_of_mass[2] };
	m->inertia_modifier = { ch.inertia_modifier[0], ch.inertia_modifier[1], ch.inertia_modifier[2] };
	m->center_of_mass_handling_offset = { ch.center_of_mass_handling_offset[0], ch.center_of_mass_handling_offset[1],
		ch.center_of_mass_handling_offset[2] };
	if (!finite(m->center_of_mass) || !finite(m->inertia_modifier) || !finite(m->center_of_mass_handling_offset)) {
		return fail("non-finite centre of mass, handling offset or inertia modifier");
	}
	for (const float f : m->center_of_mass_handling_offset) {
		if (std::fabs(f) > 50.0f) {
			return fail("centre of mass handling offset outside 50 m");
		}
	}
	for (const float f : m->inertia_modifier) {
		if (!(f > 0.01f && f < 100.0f)) {
			return fail("inertia modifier outside 0.01..100");
		}
	}
	if (collision != nullptr) {
		m->friction = collision->friction;
		m->restitution = collision->restitution;
		m->linear_damping = std::max(collision->linear_damping, 0.0f);
		m->angular_damping = std::max(collision->angular_damping, 0.0f);
	}
	if (!finite(m->linear_damping) || m->linear_damping > 100.0f || !finite(m->angular_damping) ||
			m->angular_damping > 100.0f) {
		return fail("chassis damping outside 0..100");
	}
	if (!finite(m->friction) || m->friction < 0.0f || m->friction > 10.0f || !finite(m->restitution) || m->restitution < 0.0f || m->restitution > 1.0f) {
		return fail("chassis friction or restitution out of range");
	}
	if (p_chassis.empty()) {
		return fail("no chassis pieces");
	}
	if (p_chassis.size() > kMaxChassisPieces) {
		return fail(std::to_string(p_chassis.size()) + " chassis pieces (at most " + std::to_string(kMaxChassisPieces) + ")");
	}
	for (size_t i = 0; i < p_chassis.size(); ++i) {
		const ChassisPieceSource &src = p_chassis[i];
		if (src.points.size() < 4 || src.points.size() > kMaxChassisPiecePoints) {
			return fail("chassis piece " + std::to_string(i) + " has " + std::to_string(src.points.size()) + " points (4.." +
					std::to_string(kMaxChassisPiecePoints) + ")");
		}
		if (!finite(src.radius) || src.radius < 0.0f || src.radius > 1.0f) {
			return fail("chassis piece " + std::to_string(i) + " convex radius out of range");
		}
		ChassisPiece piece;
		piece.convex_radius = src.radius;
		for (const Vec3 &p : src.points) {
			if (!finite(p) || std::fabs(p[0]) > 100.0f || std::fabs(p[1]) > 100.0f || std::fabs(p[2]) > 100.0f) {
				return fail("chassis piece " + std::to_string(i) + " has a point outside 100 m");
			}
			piece.points.push_back(p);
		}
		m->chassis.push_back(std::move(piece));
	}

	std::vector<size_t> physics_wheels;
	for (size_t i = 0; i < p_def.wheels.size(); ++i) {
		if (p_def.wheels[i].physics_type == "wptNormal") {
			physics_wheels.push_back(i);
		}
	}
	if (physics_wheels.empty()) {
		return fail("no wptNormal wheels");
	}
	if (physics_wheels.size() > kMaxVehicleWheels) {
		return fail(std::to_string(physics_wheels.size()) + " physics wheels (at most " + std::to_string(kMaxVehicleWheels) + ")");
	}
	m->wheel_count = uint8_t(physics_wheels.size());
	m->wheel_list = uint8_t(std::min<size_t>(p_def.wheels.size(), 255));
	const uint32_t wheel_count = m->wheel_count;
	const float standing_substep_factor = vehicles::spring_substep_factor(vehicles::chassis_substeps(0.0f, true));
	const core::gameplay::WheelConfigDef *steering_config = nullptr;
	for (size_t k = 0; k < physics_wheels.size(); ++k) {
		const core::gameplay::VehicleWheelDef &wd = p_def.wheels[physics_wheels[k]];
		if (wd.config >= p_def.wheel_configs.size()) {
			return fail("wheel " + std::to_string(k) + " names config " + std::to_string(wd.config));
		}
		const core::gameplay::WheelConfigDef &c = p_def.wheel_configs[wd.config];
		VehicleWheelModel &w = m->wheels[k];
		w.data_centre = { wd.transform.trans[0], wd.transform.trans[1], wd.transform.trans[2] };
		if (!finite(w.data_centre) || std::fabs(w.data_centre[0]) > 50.0f || std::fabs(w.data_centre[1]) > 50.0f ||
				std::fabs(w.data_centre[2]) > 50.0f) {
			return fail("wheel " + std::to_string(k) + " position out of range");
		}
		if (!(c.radius > 0.05f && c.radius < 5.0f) || !(c.width > 0.01f && c.width < 5.0f) || !(c.spring_length > 0.01f && c.spring_length < 5.0f)) {
			return fail("wheel " + std::to_string(k) + " radius, width or spring length out of range");
		}
		w.radius = c.radius;
		w.width = c.width;
		w.suspension_max = c.spring_length;
		w.suspension_min = c.spring_length * p_tuning.suspension_min_fraction;
		if (!finite(c.spring_stiffness) || !(c.spring_stiffness >= 0.0f && c.spring_stiffness < 1.0f) ||
				!finite(c.spring_damping) || c.spring_damping < 0.0f || c.spring_damping > 100.0f ||
				!finite(c.spring_progressive_start_ratio) || c.spring_progressive_start_ratio < 0.0f ||
				!finite(c.spring_progressive_exponent) || c.spring_progressive_exponent < 0.0f ||
				c.spring_progressive_exponent > 100.0f) {
			return fail("wheel " + std::to_string(k) + " spring stiffness, damping or progression out of range");
		}
		w.spring_constant = vehicles::spring_constant(m->mass, wheel_count, c.spring_stiffness, c.spring_length);
		w.spring_damping = c.spring_damping;
		w.progressive_start = c.spring_progressive_start_ratio;
		w.progressive_exponent = c.spring_progressive_exponent;
		w.rest_compression = std::min(m->mass * p_tuning.gravity * m->gravity_factor /
						(float(wheel_count) * standing_substep_factor * w.spring_constant),
				c.spring_length - w.suspension_min);
		w.attachment = { w.data_centre[0], w.data_centre[1] + (c.spring_length - w.rest_compression),
			w.data_centre[2] };
		w.mass = c.mass;
		if (c.rotation_direction_index < 0 || c.rotation_direction_index > 2) {
			return fail("wheel " + std::to_string(k) + " rotation direction index outside 0..2");
		}
		w.inertia = c.inertia[size_t(c.rotation_direction_index)];
		if (!finite(w.inertia) || !(w.inertia > 0.0f && w.inertia < 1000.0f)) {
			return fail("wheel " + std::to_string(k) + " inertia outside 0..1000");
		}
		if (c.friction_method != 0) {
			return fail("wheel " + std::to_string(k) + " friction method " + std::to_string(c.friction_method) +
					" (only method 0 is read)");
		}
		w.fx_torque_multiplier = c.fx_torque_radius_multiplier;
		w.longitudinal_force_max = c.longitudinal_friction_force_max_factor;
		w.lateral_force_max = c.lateral_friction_force_max_factor;
		w.lattitude_brake_scale = c.wheel_friction_lattitude_brake_scale;
		w.constant_spring_force_friction_scale = c.constant_spring_force_friction_scale;
		w.slope_grip_min = c.slope_grip_min_angle;
		w.slope_grip_max = c.slope_grip_max_angle;
		w.slope_grip_exponent = c.slope_grip_exponent;
		w.grip = { c.angular_velocity_min_slip_condition, c.side_slip_angle_max_slip_condition,
			c.wheel_velocity_x_min_slip_condition, c.wheel_slip_ratio_max_slip_condition, c.always_grip };
		w.tyre = { c.lateral_pos_k, c.lateral_neg_k, c.longitudinal_pos_k, c.longitudinal_neg_k, c.is_allowed_to_spin };
		const float tyre_numbers[] = { w.fx_torque_multiplier, w.longitudinal_force_max, w.lateral_force_max,
			w.lattitude_brake_scale, w.constant_spring_force_friction_scale, w.slope_grip_min, w.slope_grip_max,
			w.grip.angular_velocity_min_deg, w.grip.side_slip_angle_max_deg, w.grip.wheel_velocity_x_min_deg,
			w.grip.slip_ratio_max, w.tyre.lateral_pos_k, w.tyre.lateral_neg_k, w.tyre.longitudinal_pos_k,
			w.tyre.longitudinal_neg_k };
		for (const float f : tyre_numbers) {
			if (!finite(f) || std::fabs(f) > 1e7f) {
				return fail("wheel " + std::to_string(k) + " tyre data outside 1e7");
			}
		}
		if (w.slope_grip_exponent > 64) {
			return fail("wheel " + std::to_string(k) + " slope grip exponent above 64");
		}
		for (const auto &[source, target, count] :
				{ std::tuple{ &c.longitude_friction_scale, &w.longitude_scale, &w.longitude_scale_points },
						std::tuple{ &c.lattitude_friction_scale, &w.lattitude_scale, &w.lattitude_scale_points } }) {
			if (source->size() > kMaxModelCurvePoints) {
				return fail("wheel " + std::to_string(k) + " friction scale curve has " +
						std::to_string(source->size()) + " points (at most " + std::to_string(kMaxModelCurvePoints) +
						")");
			}
			for (size_t i = 0; i < source->size(); ++i) {
				if (!finite((*source)[i][0]) || !finite((*source)[i][1]) ||
						(i > 0 && !((*source)[i][0] > (*source)[i - 1][0]))) {
					return fail("wheel " + std::to_string(k) + " friction scale points not finite and ascending");
				}
				(*target)[i] = (*source)[i];
			}
			*count = uint8_t(source->size());
		}
		w.track_steer = vehicles::track_steer_wheel(c.steering_type, c.driving_type) ? 1 : 0;
		w.steers = c.steering_type != 0 ? 1 : 0;
		w.driven = c.driving_type != 0 ? 1 : 0;
		w.rolling_resistance = c.use_rolling_resistance_velocity_factor ? 1 : 0;
		w.engine_brake = c.use_engine_brake ? 1 : 0;
		w.low_speed_auto_brake = c.use_low_speed_auto_brake ? 1 : 0;
		w.rolling_base = c.rolling_resistance_base_factor;
		w.rolling_velocity_kmh = c.rolling_resistance_velocity_factor;
		if (!finite(w.rolling_base) || std::fabs(w.rolling_base) > 1e4f || !finite(w.rolling_velocity_kmh) ||
				w.rolling_velocity_kmh < 0.0f || w.rolling_velocity_kmh > 1e4f || !finite(w.mass) || w.mass < 0.0f ||
				w.mass > 1e5f) {
			return fail("wheel " + std::to_string(k) + " rolling resistance or mass out of range");
		}
		w.brake_factor = c.brake_factor;
		w.hand_brake_factor = c.hand_brake_factor;
		w.diff_gear_ratio = c.diff_gear_ratio;
		if (!finite(w.brake_factor) || w.brake_factor < 0.0f || w.brake_factor > 1000.0f ||
				!finite(w.hand_brake_factor) || w.hand_brake_factor < 0.0f || w.hand_brake_factor > 1000.0f) {
			return fail("wheel " + std::to_string(k) + " brake factors outside 0..1000");
		}
		if (w.driven != 0 && !(w.diff_gear_ratio > 0.01f && w.diff_gear_ratio < 1000.0f)) {
			return fail("wheel " + std::to_string(k) + " differential ratio out of range");
		}

		w.side = w.data_centre[0] > 0.0f ? 0 : 1;
		if (w.steers != 0 && steering_config == nullptr) {
			steering_config = &c;
		}
	}

	{
		std::vector<float> axle_z;
		for (size_t k = 0; k < m->wheel_count; ++k) {
			bool found = false;
			for (const float z : axle_z) {
				found |= std::fabs(z - m->wheels[k].data_centre[2]) < 0.05f;
			}
			if (!found) {
				axle_z.push_back(m->wheels[k].data_centre[2]);
			}
		}
		std::sort(axle_z.begin(), axle_z.end(), [](float p_a, float p_b) { return p_a > p_b; });
		for (size_t k = 0; k < m->wheel_count; ++k) {
			for (size_t a = 0; a < axle_z.size(); ++a) {
				if (std::fabs(axle_z[a] - m->wheels[k].data_centre[2]) < 0.05f) {
					m->wheels[k].axle = uint8_t(a);
					break;
				}
			}
		}

		bool any_driven = false;
		for (size_t k = 0; k < m->wheel_count; ++k) {
			any_driven |= m->wheels[k].driven != 0;
		}
		if (!any_driven) {
			return fail("no driven wheels");
		}
	}

	if (steering_config != nullptr) {
		std::vector<std::array<float, 2>> points;
		for (const core::gameplay::SteeringSensitivityPoint &p : steering_config->steering_sensitivity) {
			if (!finite(p.velocity) || !finite(p.sensitivity) || p.sensitivity < 0.0f || p.sensitivity > 10.0f) {
				return fail("steering sensitivity point out of range");
			}
			points.push_back({ p.velocity, p.sensitivity });
		}
		if (points.size() > kMaxModelCurvePoints) {
			return fail(std::to_string(points.size()) + " steering sensitivity points (at most " + std::to_string(kMaxModelCurvePoints) + ")");
		}
		std::stable_sort(points.begin(), points.end(), [](const std::array<float, 2> &p_a, const std::array<float, 2> &p_b) { return p_a[0] < p_b[0]; });
		for (size_t i = 0; i < points.size(); ++i) {
			m->steer_sensitivity[i] = points[i];
		}
		m->steer_points = uint8_t(points.size());
		m->steer_inverted = steering_config->has_steering_inverted;
	}
	for (size_t k = 0; k < m->wheel_count; ++k) {
		const core::gameplay::WheelConfigDef &c = p_def.wheel_configs[p_def.wheels[physics_wheels[k]].config];
		m->wheels[k].steers = c.steering_type == 1 ? 1 : 0;
	}
	core::Status land = build_land_drive(p_def, m->land);
	if (!land.ok()) {
		return fail(land.error().message);
	}
	for (size_t k = 0; k < m->wheel_count; ++k) {
		const core::gameplay::WheelConfigDef &c = p_def.wheel_configs[p_def.wheels[physics_wheels[k]].config];
		if (m->wheels[k].driven == 0 || c.transmission_loss.empty()) {
			continue;
		}
		if (c.transmission_loss.size() > kMaxModelCurvePoints) {
			return fail(std::to_string(c.transmission_loss.size()) + " transmission loss points (at most " +
					std::to_string(kMaxModelCurvePoints) + ")");
		}
		for (size_t i = 0; i < c.transmission_loss.size(); ++i) {
			if (!finite(c.transmission_loss[i][0]) || !finite(c.transmission_loss[i][1]) ||
					(i > 0 && !(c.transmission_loss[i][0] > c.transmission_loss[i - 1][0]))) {
				return fail("transmission loss points not finite and ascending");
			}
			m->land.transmission_loss[i] = c.transmission_loss[i];
		}
		m->land.transmission_loss_points = uint8_t(c.transmission_loss.size());
		break;
	}
	for (size_t k = 0; k < m->wheel_count; ++k) {
		if (m->wheels[k].track_steer == 0) {
			continue;
		}
		const core::gameplay::WheelConfigDef &c = p_def.wheel_configs[p_def.wheels[physics_wheels[k]].config];
		if (!finite(c.tracked_forward_speed_limit) || c.tracked_forward_speed_limit < 0.0f ||
				c.tracked_forward_speed_limit > 1000.0f || !finite(c.tracked_turn_speed_limit) ||
				c.tracked_turn_speed_limit < 0.0f || c.tracked_turn_speed_limit > 1000.0f) {
			return fail("tracked speed limits outside 0..1000");
		}
		if (c.steer_inertia.size() > kMaxModelCurvePoints) {
			return fail(std::to_string(c.steer_inertia.size()) + " steer inertia points (at most " +
					std::to_string(kMaxModelCurvePoints) + ")");
		}
		m->land.track_forward_speed_limit = c.tracked_forward_speed_limit;
		m->land.track_turn_speed_limit = c.tracked_turn_speed_limit;
		m->land.steer_inertia_points = uint8_t(c.steer_inertia.size());
		for (size_t i = 0; i < c.steer_inertia.size(); ++i) {
			if (!finite(c.steer_inertia[i][0]) || !finite(c.steer_inertia[i][1]) ||
					(i > 0 && !(c.steer_inertia[i][0] > c.steer_inertia[i - 1][0]))) {
				return fail("steer inertia points not finite and ascending");
			}
			m->land.steer_inertia[i] = c.steer_inertia[i];
		}
		break;
	}

	m->motion_damping = { ch.damping_pitch, ch.damping_yaw, ch.damping_roll, ch.damping_linear,
		{ ch.damping_linear_modifier[0], ch.damping_linear_modifier[1], ch.damping_linear_modifier[2] } };
	m->wind_resistance = ch.use_wind_resistance;
	m->wind = { ch.coefficient_of_air_friction, ch.air_drag_area, ch.air_density, ch.wind_resistance_base_factor,
		ch.wind_resistance_velocity_factor, ch.wind_resistance_velocity_factor_min,
		ch.wind_resistance_velocity_factor_max };
	m->down_force = ch.use_down_force;
	m->down_force_wheel_factor = ch.use_down_force_wheel_factor;
	m->down_force_base = ch.down_force_base_factor;
	m->down_force_per_wheel = ch.down_force_wheel_factor;
	m->anti_roll_front = ch.anti_roll_front;
	m->anti_roll_rear = ch.anti_roll_rear;
	m->stand_still_brake = ch.use_stand_still_brake;
	if (!finite(ch.stand_still_low_speed_time_limit) || ch.stand_still_low_speed_time_limit < 0.0f ||
			ch.stand_still_low_speed_time_limit > 600.0f) {
		return fail("stand-still time limit outside 0..600 s");
	}
	m->stand_still_ticks = uint16_t(seconds_to_ticks(ch.stand_still_low_speed_time_limit));
	m->stand_still_speed = p_tuning.stand_still_speed;
	const float chassis_numbers[] = { m->motion_damping.pitch, m->motion_damping.yaw, m->motion_damping.roll,
		m->motion_damping.linear, m->motion_damping.linear_modifier[0], m->motion_damping.linear_modifier[1],
		m->motion_damping.linear_modifier[2], m->wind.coefficient_of_air_friction, m->wind.air_drag_area,
		m->wind.air_density, m->wind.base_factor, m->wind.velocity_factor_kmh, m->wind.velocity_factor_min,
		m->wind.velocity_factor_max, m->down_force_base, m->down_force_per_wheel, m->anti_roll_front,
		m->anti_roll_rear };
	for (const float f : chassis_numbers) {
		if (!finite(f) || f < 0.0f || f > 1e4f) {
			return fail("motion damping, wind resistance or down force outside 0..1e4");
		}
	}
	m->tester = p_tuning.tester;
	m->max_pitch_roll_angle = p_tuning.max_pitch_roll_degrees * core::kDegToRadF;
	m->model_digest = digest_of(*m);
	return std::shared_ptr<const VehicleModel>(std::move(m));
}

namespace {

size_t collision_body_index(std::span<const core::gameplay::DriveRigidBodyDef> p_bodies) {
	for (size_t i = 0; i < p_bodies.size(); ++i) {
		if (p_bodies[i].body_type == "RBTypeCollision") {
			return i;
		}
	}
	return 0;
}

}

core::Result<std::shared_ptr<const VehicleModel>> load_vehicle_model(const core::EbxResolver &p_db, const core::LevelMount &p_mount, std::string_view p_blueprint,
		const VehicleTuning &p_tuning, core::gameplay::WorkBudget *p_budget) {
	core::Result<core::gameplay::VehicleDriveDef> def = core::gameplay::read_vehicle_drive_def(p_db, p_blueprint, p_budget);
	BF3_TRY(def);
	if (def->physics_asset.empty()) {
		return core::make_error("vehicle model " + def->blueprint + ": no physics asset");
	}
	const float scale = def->physics_asset_scale;
	if (!finite(scale) || !(scale > 0.01f && scale < 100.0f)) {
		return core::make_error("vehicle model " + def->blueprint + ": physics asset scale out of range");
	}
	core::Result<core::res::LoadedHavok> havok = core::res::load_havok(p_mount, def->physics_asset);
	BF3_TRY(havok);
	if (havok->shapes.empty()) {
		return core::make_error("vehicle model " + def->blueprint + ": " + def->physics_asset + " has no rigid body shape");
	}
	const size_t chassis = collision_body_index(def->rigid_bodies);
	if (chassis >= havok->shapes.size()) {
		return core::make_error("vehicle model " + def->blueprint + ": " + def->physics_asset + " has " +
				std::to_string(havok->shapes.size()) + " rigid body shapes, none for collision body " +
				std::to_string(chassis));
	}

	const std::vector<core::res::ConvexPiece> pieces =
			core::res::havok_convex_pieces(havok->shapes[chassis], core::res::Transform::identity());
	if (pieces.size() > kMaxChassisPieces) {
		return core::make_error("vehicle model " + def->blueprint + ": " + std::to_string(pieces.size()) + " chassis pieces (at most " +
				std::to_string(kMaxChassisPieces) + ")");
	}
	std::vector<std::vector<Vec3>> points(pieces.size());
	std::vector<ChassisPieceSource> sources;
	for (size_t i = 0; i < pieces.size(); ++i) {
		for (const core::res::Vec3 &p : pieces[i].points) {
			points[i].push_back({ p[0] * scale, p[1] * scale, p[2] * scale });
		}
		sources.push_back({ std::span<const Vec3>(points[i]), pieces[i].radius * scale });
	}
	return build_vehicle_model(*def, sources, p_tuning);
}

}

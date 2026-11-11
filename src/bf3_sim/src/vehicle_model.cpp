#include "bf3/sim/vehicle_model.h"

#include "bf3/core/gameplay/vehicle_drive.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/util/finite.h"
#include "vehicles/land_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
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
	for (size_t i = 0; i < p_m.wheel_count; ++i) {
		const VehicleWheelModel &w = p_m.wheels[i];
		d.add(w.attachment);
		d.add(w.data_centre);
		for (const float f : { w.radius, w.width, w.suspension_min, w.suspension_max, w.spring_stiffness,
					 w.spring_damping, w.inertia, w.angular_damping, w.max_steer_angle, w.max_brake_torque,
					 w.max_hand_brake_torque, w.brake_factor, w.hand_brake_factor, w.diff_gear_ratio }) {
			d.add(f);
		}
		for (const uint8_t b : { w.steers, w.driven, w.axle, w.side }) {
			d.add(b);
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
				 l.engine.internal_deceleration, l.power_multiplier, l.drive_share }) {
		d.add(f);
	}
	d.add(l.rpm_points);
	d.add(l.torque_points);
	d.add(p_m.anti_roll_bars);
	for (size_t i = 0; i < p_m.anti_roll_bars; ++i) {
		d.add(p_m.anti_roll_wheels[i]);
		d.add(p_m.anti_roll_stiffness[i]);
	}
	d.add(p_m.steer_points);
	for (size_t i = 0; i < p_m.steer_points; ++i) {
		d.add(p_m.steer_sensitivity[i]);
	}
	d.add(p_m.steer_model);
	d.add(p_m.drag_coefficient);
	d.add(p_m.stand_still_brake);
	d.add(p_m.stand_still_ticks);
	d.add(p_m.stand_still_speed);
	d.add(p_m.tester);
	d.add(p_m.max_pitch_roll_angle);
	d.add_text(format_vehicle_tuning(p_m.tuning));
	return d.value();
}

float sensitivity_at(const VehicleModel &p_model, float p_kmh) {
	if (p_model.steer_points == 0) {
		return 1.0f;
	}
	const auto &c = p_model.steer_sensitivity;
	if (p_kmh <= c[0][0]) {
		return c[0][1];
	}
	for (size_t i = 1; i < p_model.steer_points; ++i) {
		if (p_kmh <= c[i][0]) {
			const float span = c[i][0] - c[i - 1][0];
			const float t = span > 0.0f ? (p_kmh - c[i - 1][0]) / span : 1.0f;
			return c[i - 1][1] + (c[i][1] - c[i - 1][1]) * t;
		}
	}
	return c[p_model.steer_points - 1][1];
}

}

float vehicle_max_steer_angle(const VehicleModel &p_model, float p_speed) {
	const float kmh = std::fabs(p_speed) * 3.6f;
	const float lock = p_model.tuning.steer_max_degrees * core::kDegToRadF;
	if (p_model.steer_model == SteerAngleModel::SensitivityRadians) {
		return p_model.steer_points > 0 ? sensitivity_at(p_model, kmh) : lock;
	}
	const float at_rest = sensitivity_at(p_model, 0.0f);
	if (!(at_rest > 0.0f)) {
		return lock;
	}
	return lock * sensitivity_at(p_model, kmh) / at_rest;
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
	if (!finite(m->center_of_mass) || !finite(m->inertia_modifier)) {
		return fail("non-finite centre of mass or inertia modifier");
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
	const float g_eff = p_tuning.gravity * m->gravity_factor;
	const float wheels_f = float(m->wheel_count);
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

		const float sag = c.spring_length * p_tuning.static_sag_fraction;
		w.attachment = { w.data_centre[0], w.data_centre[1] + (c.spring_length - sag), w.data_centre[2] };
		w.spring_stiffness = m->mass * g_eff / (wheels_f * sag);
		w.spring_damping = 2.0f * p_tuning.damping_ratio * std::sqrt(w.spring_stiffness * m->mass / wheels_f);
		w.inertia = c.inertia[0] > 0.0f && finite(c.inertia[0]) && c.inertia[0] < 1000.0f ? c.inertia[0] : 0.5f * c.mass * c.radius * c.radius;
		if (!(w.inertia > 0.0f)) {
			w.inertia = 1.0f;
		}
		w.angular_damping = p_tuning.wheel_angular_damping;
		w.steers = c.steering_type != 0 ? 1 : 0;
		w.driven = c.driving_type != 0 ? 1 : 0;
		w.max_steer_angle = 0.0f;
		if (!finite(c.brake_force) || c.brake_force < 0.0f || c.brake_force > 1e6f) {
			return fail("wheel " + std::to_string(k) + " brake force out of range");
		}
		w.max_brake_torque = c.brake_force * c.radius * p_tuning.brake_torque_per_force;
		w.max_hand_brake_torque = w.steers != 0 ? 0.0f : p_tuning.hand_brake_torque;
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

		const float stiffness[2] = { ch.anti_roll_front * p_tuning.anti_roll_scale, ch.anti_roll_rear * p_tuning.anti_roll_scale };
		const size_t axles[2] = { 0, axle_z.size() - 1 };
		for (size_t b = 0; b < 2; ++b) {
			if (!(stiffness[b] > 0.0f) || !finite(stiffness[b]) || (b == 1 && axles[1] == axles[0])) {
				continue;
			}
			int8_t left = -1, right = -1;
			for (size_t k = 0; k < m->wheel_count; ++k) {
				const VehicleWheelModel &w = m->wheels[k];
				if (w.axle != axles[b]) {
					continue;
				}
				if (w.side == 0 && left < 0) {
					left = int8_t(k);
				} else if (w.side == 1 && right < 0) {
					right = int8_t(k);
				}
			}
			if (left >= 0 && right >= 0) {
				m->anti_roll_wheels[m->anti_roll_bars] = { left, right };
				m->anti_roll_stiffness[m->anti_roll_bars] = stiffness[b];
				++m->anti_roll_bars;
			}
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
	}
	m->steer_model = p_tuning.steer_model;
	const float lock = vehicle_max_steer_angle(*m, 0.0f);
	for (size_t k = 0; k < m->wheel_count; ++k) {
		m->wheels[k].max_steer_angle = m->wheels[k].steers != 0 ? lock : 0.0f;
	}
	core::Status land = build_land_drive(p_def, m->land);
	if (!land.ok()) {
		return fail(land.error().message);
	}
	m->land.drive_share = p_tuning.wheel_drive_share;

	const float drag = 0.5f * ch.air_density * ch.coefficient_of_air_friction * ch.air_drag_area * p_tuning.drag_scale;
	if (!finite(drag) || drag < 0.0f || drag > 1e4f) {
		return fail("air drag out of range");
	}
	m->drag_coefficient = drag;
	m->stand_still_brake = ch.use_stand_still_brake;
	if (!finite(ch.stand_still_low_speed_time_limit) || ch.stand_still_low_speed_time_limit < 0.0f || ch.stand_still_low_speed_time_limit > 600.0f) {
		return fail("stand-still time limit outside 0..600 s");
	}
	m->stand_still_ticks = uint16_t(seconds_to_ticks(ch.stand_still_low_speed_time_limit));
	m->stand_still_speed = p_tuning.stand_still_speed;
	m->tester = p_tuning.tester;
	m->max_pitch_roll_angle = p_tuning.max_pitch_roll_degrees * core::kDegToRadF;
	m->model_digest = digest_of(*m);
	return std::shared_ptr<const VehicleModel>(std::move(m));
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

	const std::vector<core::res::ConvexPiece> pieces = core::res::havok_convex_pieces(havok->shapes[0], core::res::Transform::identity());
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

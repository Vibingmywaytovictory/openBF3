#include "bf3/core/gameplay/vehicle_drive.h"

#include "bf3/core/ids.h"
#include "bf3/core/level/level_transform.h"
#include "gameplay_read.h"

#include <cmath>
#include <optional>
#include <string>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr size_t kMaxDepth = 64;
constexpr size_t kMaxComponents = 8192;

bool active(const EbxInstance &p_component) {
	if (boolean(p_component.data(), "Excluded").value_or(false)) {
		return false;
	}
	return !p_component.is_a("UnlockComponentData") || boolean(p_component.data(), "InvertUnlockTest").value_or(false);
}

DriveVec3 v3(const level::Vec3 &p_v) {
	return { p_v[0], p_v[1], p_v[2] };
}

DriveTransform drive_transform(const level::LevelTransform &p_t) {
	DriveTransform out;
	out.right = v3(p_t.right);
	out.up = v3(p_t.up);
	out.forward = v3(p_t.forward);
	out.trans = v3(p_t.trans);
	return out;
}

class Numbers {
public:
	explicit Numbers(std::string p_where) :
			where_(std::move(p_where)) {}

	float f(const EbxStruct &p_object, std::string_view p_path, float p_fallback) {
		const std::optional<double> v = number(p_object, p_path);
		if (!v) {
			return p_fallback;
		}
		if (!std::isfinite(*v) || std::fabs(*v) > 1e30) {
			fail(p_path);
			return p_fallback;
		}
		return float(*v);
	}
	int32_t i(const EbxStruct &p_object, std::string_view p_path, int32_t p_fallback) {
		const std::optional<EbxValue> v = p_object.get_path(p_path);
		if (!v) {
			return p_fallback;
		}
		if (const std::optional<int32_t> e = v->enum_value()) {
			return *e;
		}
		if (const std::optional<int64_t> n = v->as_i64(); n && *n >= INT32_MIN && *n <= INT32_MAX) {
			return int32_t(*n);
		}
		return p_fallback;
	}
	DriveVec3 vec(const EbxStruct &p_object, std::string_view p_path, const DriveVec3 &p_fallback) {
		const std::optional<std::array<float, 3>> v = vec3(p_object, p_path);
		if (!v) {
			return p_fallback;
		}
		for (const float c : *v) {
			if (!std::isfinite(c) || std::fabs(c) > 1e30f) {
				fail(p_path);
				return p_fallback;
			}
		}
		return *v;
	}
	bool b(const EbxStruct &p_object, std::string_view p_path, bool p_fallback) { return boolean(p_object, p_path).value_or(p_fallback); }
	void fail(std::string_view p_what) {
		if (!error_) {
			error_ = make_error(where_ + ": non-finite or out-of-range `" + std::string(p_what) + "`");
		}
	}
	void fail_message(std::string p_message) {
		if (!error_) {
			error_ = make_error(where_ + ": " + std::move(p_message));
		}
	}
	const std::optional<Error> &error() const { return error_; }

private:
	std::string where_;
	std::optional<Error> error_;
};

std::vector<float> float_array(WorkBudget &r_budget, Numbers &r_numbers, const EbxStruct &p_object, std::string_view p_field, size_t p_max) {
	std::vector<float> out;
	const OptionalEbxArray a = array_of(r_budget, p_object, p_field);
	if (!a) {
		return out;
	}
	if (a->size() > p_max) {
		r_numbers.fail_message("`" + std::string(p_field) + "` has " + std::to_string(a->size()) + " entries (at most " + std::to_string(p_max) + ")");
		return out;
	}
	for (const EbxValue e : *a) {
		const std::optional<double> v = number(e);
		if (!v || !std::isfinite(*v) || std::fabs(*v) > 1e30) {
			r_numbers.fail(p_field);
			return {};
		}
		out.push_back(float(*v));
	}
	return out;
}

InputAxisDef read_axis(Numbers &r_n, const EbxStruct &p_input, const char *p_axis) {
	const std::string a(p_axis);
	InputAxisDef out;
	out.deadzone = r_n.f(p_input, a + "Deadzone", 0);
	out.inertia_in = r_n.f(p_input, a + "InertiaInDuration", 0);
	out.inertia_out = r_n.f(p_input, a + "InertiaOutDuration", 0);
	out.inertia_min_ratio = r_n.f(p_input, a + "InertiaMinRatio", 0);
	return out;
}

class DriveReader {
public:
	DriveReader(WorkBudget &r_budget, const EbxResolver &p_db, std::string p_name) :
			budget_(r_budget), db_(p_db), numbers_("vehicle drive " + p_name) {}

	VehicleDriveDef out;

	void walk(const EbxInstance &p_container, const level::LevelTransform &p_frame, size_t p_depth) {
		if (p_depth > kMaxDepth) {
			numbers_.fail_message("component tree deeper than " + std::to_string(kMaxDepth));
			return;
		}
		for (const EbxInstance &component : follow_all(budget_, db_, p_container.data(), "Components")) {
			if (budget_.exhausted() || numbers_.error()) {
				return;
			}
			if (components_ >= kMaxComponents) {
				numbers_.fail_message("more than " + std::to_string(kMaxComponents) + " components");
				return;
			}
			if (!component.is_a("ComponentData") || !active(component)) {
				continue;
			}
			++components_;
			level::LevelTransform local;
			if (const std::optional<EbxValue> t = component.get("Transform")) {
				if (const std::optional<std::array<std::array<float, 3>, 4>> rows = t->as_transform()) {
					for (const std::array<float, 3> &row : *rows) {
						for (const float c : row) {
							if (!std::isfinite(c) || std::fabs(c) > 1e30f) {
								numbers_.fail("Transform");
								return;
							}
						}
					}
					local = level::LevelTransform::from_rows(*rows);
				}
			}
			const level::LevelTransform frame = local.in_parent(p_frame);
			read_component(component, frame);
			walk(component, frame, p_depth + 1);
		}
	}

	size_t components() const { return components_; }
	Numbers &numbers() { return numbers_; }

	void read_physics(const EbxStruct &p_entity) {
		const std::optional<EbxInstance> physics = db_.follow(p_entity, "PhysicsData");
		if (!physics || !budget_.take_type(physics->data())) {
			return;
		}
		const EbxStruct p = physics->data();
		const OptionalEbxArray bodies = array_of(budget_, p, "RigidBodies");
		if (bodies && bodies->size() > kMaxDriveRigidBodies) {
			numbers_.fail_message("RigidBodies has " + std::to_string(bodies->size()) + " entries (at most " + std::to_string(kMaxDriveRigidBodies) + ")");
			return;
		}
		if (bodies) {
			for (const EbxValue element : *bodies) {
				const std::optional<EbxRef> r = element.as_ref();
				const std::optional<EbxInstance> body = r ? db_.resolve(p.partition(), *r) : std::nullopt;
				if (!body || !budget_.take_type(body->data())) {
					continue;
				}
				const EbxStruct b = body->data();
				DriveRigidBodyDef d;
				d.body_type = text(budget_, b, "RigidBodyType");
				d.collision_layer = text(budget_, b, "CollisionLayer");
				d.motion_type = text(budget_, b, "MotionType");
				d.mass = numbers_.f(b, "Mass", 0);
				d.friction = numbers_.f(b, "Friction", 0);
				d.restitution = numbers_.f(b, "Restitution", 0);
				d.linear_damping = numbers_.f(b, "LinearVelocityDamping", -1);
				d.angular_damping = numbers_.f(b, "AngularVelocityDamping", -1);
				d.inertia_modifier = numbers_.vec(b, "InertiaModifier", { 1, 1, 1 });
				out.rigid_bodies.push_back(std::move(d));
			}
		}
		std::optional<EbxInstance> asset = db_.follow(p, "Asset");
		if (!asset) {
			const OptionalEbxArray scaled = array_of(budget_, p, "ScaledAssets");
			if (scaled && !scaled->empty()) {
				const std::optional<EbxValue> first = scaled->get(0);
				const std::optional<EbxRef> r = first ? first->as_ref() : std::nullopt;
				asset = r ? db_.resolve(p.partition(), *r) : std::nullopt;
			}
		}
		if (asset && budget_.take_type(asset->data())) {
			out.physics_asset = text(budget_, asset->data(), "Name");
			out.physics_asset_scale = numbers_.f(asset->data(), "Scale", 1);
		}
	}

private:
	void read_component(const EbxInstance &p_component, const level::LevelTransform &p_frame) {
		const EbxStruct c = p_component.data();
		if (p_component.get("VehicleConfig")) {
			read_chassis(c);
		} else if (p_component.is_a("WheelComponentData")) {
			if (out.wheels.size() >= kMaxDriveWheels) {
				numbers_.fail_message("more than " + std::to_string(kMaxDriveWheels) + " wheels");
				return;
			}
			VehicleWheelDef w;
			w.component = uint32_t(components_ - 1);
			w.physics_type = text(budget_, c, "PhysicsType");
			w.transform = drive_transform(p_frame);
			const std::optional<EbxInstance> config = db_.follow(c, "Config");
			if (!config || !budget_.take_type(config->data())) {
				numbers_.fail_message("wheel " + std::to_string(out.wheels.size()) + " has no WheelConfigData");
				return;
			}
			w.config = wheel_config(*config);
			out.wheels.push_back(std::move(w));
		} else if (p_component.is_a("EngineComponentData")) {
			if (out.engines.size() >= kMaxDriveEngines) {
				numbers_.fail_message("more than " + std::to_string(kMaxDriveEngines) + " engines");
				return;
			}
			EngineDef e;
			e.transform = drive_transform(p_frame);
			if (const std::optional<EbxInstance> config = db_.follow(c, "Config"); config && budget_.take_type(config->data())) {
				e.config_type = copy_text(budget_, config->type_name());
				if (config->is_a("CombustionEngineConfigData")) {
					const EbxStruct g = config->data();
					e.rpm_points = float_array(budget_, numbers_, g, "RpmCurvePoints", kMaxDriveCurvePoints);
					e.torque_points = float_array(budget_, numbers_, g, "TorqueCurvePoints", kMaxDriveCurvePoints);
					if (e.rpm_points.size() != e.torque_points.size()) {
						numbers_.fail_message("engine curve has " + std::to_string(e.rpm_points.size()) + " rpm and " + std::to_string(e.torque_points.size()) +
								" torque points");
					}
					e.rpm_min = numbers_.f(g, "RpmMin", 0);
					e.rpm_max = numbers_.f(g, "RpmMax", 0);
					e.rpm_cut = numbers_.f(g, "RpmCut", 0);
					e.power_multiplier = numbers_.f(g, "EnginePowerMultiplier", 1);
					e.internal_acceleration = numbers_.f(g, "InternalAccelerationFactor", 0);
					e.internal_deceleration = numbers_.f(g, "InternalDeaccelerationFactor", 0);
					e.boost_acceleration_scale = numbers_.f(g, "Boost.AccelerationScale", 1);
					e.boost_forward_strength = numbers_.f(g, "Boost.ForwardStrength", 1);
					e.boost_reverse_strength = numbers_.f(g, "Boost.ReverseStrength", 1);
					e.boost_crawl_strength = numbers_.f(g, "Boost.CrawlStrength", 1);
					e.boost_dissipation_time = numbers_.f(g, "Boost.DissipationTime", 1);
					e.boost_recovery_time = numbers_.f(g, "Boost.RecoveryTime", 1);
				}
			}
			out.engines.push_back(std::move(e));
		}
	}

	void read_chassis(const EbxStruct &p_chassis) {
		if (chassis_read_) {
			return;
		}
		chassis_read_ = true;
		ChassisConfigDef &ch = out.chassis;
		ch.allow_negative_throttle = numbers_.b(p_chassis, "AllowNegativeThrottle", false);
		ch.always_full_throttle = numbers_.b(p_chassis, "AlwaysFullThrottle", false);
		if (const std::optional<EbxInstance> config = db_.follow(p_chassis, "VehicleConfig"); config && budget_.take_type(config->data())) {
			const EbxStruct c = config->data();
			ch.body_mass = numbers_.f(c, "BodyMass", 0);
			ch.gravity_modifier = numbers_.f(c, "GravityModifier", 1);
			ch.center_of_mass = numbers_.vec(c, "CenterOfMass", {});
			ch.center_of_mass_handling_offset = numbers_.vec(c, "CenterOfMassHandlingOffset", {});
			ch.inertia_modifier = numbers_.vec(c, "InertiaModifier", { 1, 1, 1 });
			ch.air_density = numbers_.f(c, "AirDensity", 0);
			ch.air_drag_area = numbers_.f(c, "AirDragArea", 0);
			ch.coefficient_of_air_friction = numbers_.f(c, "CoefficientOfAirFriction", 0);
			if (const std::optional<EbxValue> bars = c.get("AntiRollBars")) {
				if (const std::optional<EbxStruct> s = bars->as_struct()) {
					if (const std::optional<EbxInstance> front = db_.follow(*s, "Front"); front && budget_.take_type(front->data())) {
						ch.anti_roll_front = numbers_.f(front->data(), "Stiffness", 0);
						ch.anti_roll_front_torque_limit = numbers_.f(front->data(), "TorqueActivationLimit", 0);
					}
					if (const std::optional<EbxInstance> rear = db_.follow(*s, "Rear"); rear && budget_.take_type(rear->data())) {
						ch.anti_roll_rear = numbers_.f(rear->data(), "Stiffness", 0);
						ch.anti_roll_rear_torque_limit = numbers_.f(rear->data(), "TorqueActivationLimit", 0);
					}
				}
			}
			if (const std::optional<EbxValue> input = c.get("Input")) {
				if (const std::optional<EbxStruct> s = input->as_struct()) {
					ch.throttle = read_axis(numbers_, *s, "Throttle");
					ch.brake = read_axis(numbers_, *s, "Brake");
					ch.yaw = read_axis(numbers_, *s, "Yaw");
					ch.pitch = read_axis(numbers_, *s, "Pitch");
					ch.roll = read_axis(numbers_, *s, "Roll");
				}
			}
			ch.yaw_min = numbers_.f(c, "YawMin", -1);
			ch.yaw_max = numbers_.f(c, "YawMax", 1);
			ch.use_gearbox = numbers_.b(c, "UseGearbox", false);
			ch.use_stand_still_brake = numbers_.b(c, "UseStandStillBrake", false);
			ch.use_stand_still_sleep = numbers_.b(c, "UseStandStillSleep", false);
			ch.use_turn_around_force = numbers_.b(c, "UseTurnAroundForce", false);
			ch.use_down_force = numbers_.b(c, "UseDownForce", false);
			ch.use_wind_resistance = numbers_.b(c, "UseWindResistance", false);
			ch.stand_still_low_speed_time_limit = numbers_.f(c, "StandStillLowSpeedTimeLimit", 0);
			ch.down_force_base_factor = numbers_.f(c, "DownForceBaseFactor", 0);
			ch.down_force_wheel_factor = numbers_.f(c, "DownForceWheelFactor", 0);
			if (const std::optional<EbxInstance> damping = db_.follow(c, "MotionDamping"); damping && budget_.take_type(damping->data())) {
				const EbxStruct d = damping->data();
				ch.damping_linear = numbers_.f(d, "Linear", 0);
				ch.damping_pitch = numbers_.f(d, "Pitch", 0);
				ch.damping_roll = numbers_.f(d, "Roll", 0);
				ch.damping_yaw = numbers_.f(d, "Yaw", 0);
				ch.damping_linear_modifier = numbers_.vec(d, "LinearModifier", { 1, 1, 1 });
			}
		}
		if (const std::optional<EbxInstance> gearbox = db_.follow(p_chassis, "GearboxConfig"); gearbox && budget_.take_type(gearbox->data())) {
			const EbxStruct g = gearbox->data();
			GearboxDef &gb = out.gearbox;
			gb.present = true;
			gb.forward_ratios = float_array(budget_, numbers_, g, "ForwardGearRatios", kMaxDriveGears);
			gb.forward_speeds = float_array(budget_, numbers_, g, "ForwardGearSpeeds", kMaxDriveGears);
			gb.reverse_ratios = float_array(budget_, numbers_, g, "ReverseGearRatios", kMaxDriveGears);
			gb.reverse_speeds = float_array(budget_, numbers_, g, "ReverseGearSpeeds", kMaxDriveGears);
			gb.gear_change_time = numbers_.f(g, "GearChangeTime", 0);
			gb.clutch_speed_factor = numbers_.f(g, "ClutchSpeedFactor", 0);
			gb.gear_down_speed_factor = numbers_.f(g, "GearDownSpeedFactor", 0);
			gb.opposite_dir_gear_change_max_speed = numbers_.f(g, "OppositeDirGearChangeMaxSpeed", 0);
			gb.opposite_dir_gear_change_time = numbers_.f(g, "OppositeDirGearChangeTime", 0);
			gb.backward_throttle_limit = numbers_.f(g, "BackwardThrottleLimit", 0);
			gb.transmission_efficiency = numbers_.f(g, "TransmissionEfficiency", 1);
			gb.gearbox_type = numbers_.i(g, "GearboxType", 0);
			gb.gearbox_mode = numbers_.i(g, "GearboxMode", 0);
			gb.use_auto_clutch = numbers_.b(g, "UseAutoClutch", false);
			gb.use_neutral_gear = numbers_.b(g, "UseNeutralGear", false);
			gb.limit_backward_throttle = numbers_.b(g, "LimitBackwardThrottle", false);
		}
	}

	uint32_t wheel_config(const EbxInstance &p_config) {
		for (size_t i = 0; i < config_instances_.size(); ++i) {
			if (config_instances_[i] == p_config) {
				return uint32_t(i);
			}
		}
		const EbxStruct c = p_config.data();
		WheelConfigDef w;
		w.radius = numbers_.f(c, "Radius", 0);
		w.width = numbers_.f(c, "Width", 0);
		w.mass = numbers_.f(c, "Mass", 0);
		w.inertia = numbers_.vec(c, "Inertia", {});
		w.offset = numbers_.vec(c, "Offset", {});
		w.spring_length = numbers_.f(c, "Spring.Length", 0);
		w.spring_stiffness = numbers_.f(c, "Spring.Stiffness", 0);
		w.spring_damping = numbers_.f(c, "Spring.Damping", 0);
		w.spring_attach_offset_y = numbers_.f(c, "Spring.AttachOffsetY", 0);
		w.spring_max_force = numbers_.f(c, "Spring.MaxSpringForce", 0);
		w.spring_progressive_exponent = numbers_.f(c, "Spring.ProgressiveExponent", 0);
		w.spring_progressive_start_ratio = numbers_.f(c, "Spring.ProgressiveStartRatio", 0);
		w.spring_disabled_strength_modifier = numbers_.f(c, "Spring.DisabledStrenghModifier", 0);
		w.brake_force = numbers_.f(c, "BrakeForce", 0);
		w.brake_factor = numbers_.f(c, "BrakeFactor", 0);
		w.hand_brake_force = numbers_.f(c, "HandBrakeForce", 0);
		w.hand_brake_factor = numbers_.f(c, "HandBrakeFactor", 0);
		w.diff_gear_ratio = numbers_.f(c, "DiffGearRatio", 0);
		w.driving_type = numbers_.i(c, "DrivingType", 0);
		w.steering_type = numbers_.i(c, "SteeringType", 0);
		w.engine_index = numbers_.i(c, "EngineIndex", 0);
		w.has_steering_inverted = numbers_.b(c, "HasSteeringInverted", false);
		w.lateral_friction_force_max_factor = numbers_.f(c, "LateralFrictionForceMaxFactor", 0);
		w.longitudinal_friction_force_max_factor = numbers_.f(c, "LongitudinalFrictionForceMaxFactor", 0);
		w.longitudinal_pos_k = numbers_.f(c, "LongitudinalPosK", 0);
		w.longitudinal_neg_k = numbers_.f(c, "LongitudinalNegK", 0);
		w.lateral_pos_k = numbers_.f(c, "LateralPosK", 0);
		w.lateral_neg_k = numbers_.f(c, "LateralNegK", 0);
		w.align_mom_scale = numbers_.f(c, "AlignMomScale", 0);
		w.rolling_resistance_base_factor = numbers_.f(c, "RollingResistanceBaseFactor", 0);
		w.rolling_resistance_velocity_factor = numbers_.f(c, "RollingResistanceVelocityFactor", 0);
		w.use_engine_brake = numbers_.b(c, "UseEngineBrake", false);
		w.engine_brake_min_factor = numbers_.f(c, "EngineBrakeMinFactor", 0);
		w.engine_brake_max_factor = numbers_.f(c, "EngineBrakeMaxFactor", 0);
		w.engine_brake_velocity_factor = numbers_.f(c, "EngineBrakeVelocityFactor", 0);
		if (const OptionalEbxArray s = array_of(budget_, c, "SteeringSensitivity")) {
			if (s->size() > kMaxDriveCurvePoints) {
				numbers_.fail_message("SteeringSensitivity has " + std::to_string(s->size()) + " points (at most " + std::to_string(kMaxDriveCurvePoints) + ")");
			} else {
				for (const EbxValue e : *s) {

					std::optional<EbxStruct> point = e.as_struct();
					std::optional<EbxInstance> resolved;
					if (!point) {
						const std::optional<EbxRef> r = e.as_ref();
						resolved = r ? db_.resolve(c.partition(), *r) : std::nullopt;
						if (resolved && budget_.take_type(resolved->data())) {
							point = resolved->data();
						}
					}
					if (!point) {
						continue;
					}
					SteeringSensitivityPoint p;
					p.velocity = numbers_.f(*point, "Velocity", 0);
					p.sensitivity = numbers_.f(*point, "SteeringSensitivity", 0);
					w.steering_sensitivity.push_back(p);
				}
			}
		}
		if (const OptionalEbxArray loss = array_of(budget_, c, "TransmissionLoss")) {
			if (loss->size() > kMaxDriveCurvePoints) {
				numbers_.fail_message("TransmissionLoss has " + std::to_string(loss->size()) + " points (at most " + std::to_string(kMaxDriveCurvePoints) + ")");
			} else {
				for (const EbxValue e : *loss) {
					const std::optional<EbxStruct> s = e.as_struct();
					if (!s) {
						continue;
					}
					w.transmission_loss.push_back({ numbers_.f(*s, "x", 0), numbers_.f(*s, "y", 0) });
				}
			}
		}
		config_instances_.push_back(p_config);
		out.wheel_configs.push_back(std::move(w));
		return uint32_t(out.wheel_configs.size() - 1);
	}

	WorkBudget &budget_;
	const EbxResolver &db_;
	Numbers numbers_;
	size_t components_ = 0;
	bool chassis_read_ = false;
	std::vector<EbxInstance> config_instances_;
};

}

Result<VehicleDriveDef> read_vehicle_drive_def(const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string name = to_lower_ascii(p_blueprint);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("vehicle drive: no partition `" + name + "`");
	}
	const EbxInstance blueprint = partition->primary();
	BF3_TRY(budget.charge_type(blueprint.data()));
	const std::optional<EbxInstance> entity = p_db.follow(blueprint.data(), "Object");
	if (!entity || !entity->is_a("VehicleEntityData")) {
		return make_error("vehicle drive: `" + name + "` has no VehicleEntityData (primary " + std::string(blueprint.type_name()) + ")");
	}
	BF3_TRY(budget.charge_type(entity->data()));
	DriveReader reader(budget, p_db, name);
	reader.out.blueprint = name;
	reader.walk(*entity, level::LevelTransform::identity(), 0);
	reader.read_physics(entity->data());
	reader.out.components = reader.components();
	if (const Status spent = budget.status(); !spent) {
		return error_at("vehicle drive " + name, spent.error());
	}
	if (reader.numbers().error()) {
		return *reader.numbers().error();
	}
	return std::move(reader.out);
}

}

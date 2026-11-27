#include "bf3/client_audio/vehicle_sounds.h"

#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/turret_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace ca = core::audio;
using P = VehicleSoundParam;

core::Result<VehicleAudioAssets> load_vehicle_audio(
		const core::EbxResolver &p_db, const host::LevelVehicles &p_level, std::vector<std::string> &r_report) {
	VehicleAudioAssets a;
	std::vector<std::string> names;
	size_t failures = 0;
	for (const host::LevelVehicleKind &k : p_level.kinds) {
		core::Result<core::gameplay::VehicleSoundDef> def = core::gameplay::read_vehicle_sounds(p_db, k.blueprint);
		if (!def) {
			return def.error();
		}
		const auto program_of = [&](const std::string &p_patch, const char *p_role) -> uint16_t {
			const auto at = std::find(names.begin(), names.end(), p_patch);
			if (at != names.end()) {
				return uint16_t(at - names.begin());
			}
			core::Result<std::shared_ptr<const ca::SoundProgram>> p = compile_patch_lenient(p_db, p_patch);
			if (!p) {
				++failures;
				r_report.push_back(std::string("vehicle ") + p_role + " " + p_patch + ": NOT COMPILED (" +
						p.error().message + ")");
				return kNoVehicleProgram;
			}
			names.push_back(p_patch);
			a.programs.push_back(*p);
			return uint16_t(names.size() - 1);
		};
		uint16_t program = kNoVehicleProgram;
		if (!def->engines.empty() && !def->engines.front().patch.empty()) {
			program = program_of(def->engines.front().patch, "engine");
		}
		std::vector<std::pair<int8_t, uint16_t>> turrets;
		for (const core::gameplay::VehicleTurretSound &c : def->turrets) {
			const uint16_t t = program_of(c.patch, "turret");
			if (t != kNoVehicleProgram) {
				turrets.emplace_back(c.turret, t);
			}
		}
		a.turret_programs.push_back(std::move(turrets));
		// SOURCE: notes R5-VS1 Rule 2 item 19 (DisabledDamageThreshold over the vehicle's maximum health).
		// INFERRED: the maximum health is the largest zone MaxHealth, as R5-VS1 infers. Settle: a note on that getter.
		float full = 0.0f;
		for (const sim::vehicles::HealthZoneRule &z : k.damage.zones) {
			full = std::max(full, z.max_health);
		}
		a.disabled_fraction.push_back(full > 0.0f ? k.damage.disabled_threshold / full : 0.0f);
		a.engine_program.push_back(program);
		a.defs.push_back(std::move(*def));
	}
	r_report.push_back("vehicle sounds: " + std::to_string(p_level.kinds.size()) + " kinds, " +
			std::to_string(a.programs.size()) + " engine and turret patches, " + std::to_string(failures) +
			" failures");
	return a;
}

namespace {

// SOURCE: notes R5-VS1 constants (GForce 9.82, 1 (m/s)^2 gate, 0.2 snap, 0.05 smoothing; Slip 1/36 m/s gate and 0.5
// weight; Load 0.1 per km/h), the 7 constants below.
constexpr float kGravity = 9.82f;
constexpr float kGForceMovingSpeedSquared = 1.0f;
constexpr float kGForceSnap = 0.2f;
constexpr float kGForceSmoothing = 0.05f;
constexpr float kSlipGateSpeed = 1.0f / 36.0f;
constexpr float kSecondContactSlipWeight = 0.5f;
constexpr float kLoadFadeKmh = 0.1f;

float spring_compression(const host::PresentedVehicle &p_v, const sim::VehicleModel &p_model) {
	float out = 0.0f;
	for (size_t i = 0; i < p_v.wheel_count && i < p_model.wheel_count; ++i) {
		const float length = p_model.wheels[i].suspension_max;
		if (length > 0.0f) {
			out = std::max(out, (length - p_v.suspension_length[i]) / length);
		}
	}
	return out;
}

// SOURCE: notes R5-VS1 Rule 2 item 15 (Load = clamp(km/h x 0.1, 0, 1) x the first engine's load).
// STAND-IN: the engine load is recomputed from the presented wheel spin, rpm and hull. Settle: replicate the load.
float engine_load_input(const host::PresentedVehicle &p_v, const sim::VehicleModel &p_model,
		const core::Transform &p_hull, float p_speed) {
	const sim::LandDriveModel &land = p_model.land;
	const float ratio = sim::land_gear_ratio(land, p_v.gear);
	float wheel_rpm_sum = 0.0f;
	uint32_t driven = 0;
	bool grounded = false;
	for (size_t i = 0; i < p_v.wheel_count && i < p_model.wheel_count; ++i) {
		const sim::VehicleWheelModel &w = p_model.wheels[i];
		if (w.driven == 0) {
			continue;
		}
		const float omega = p_v.wheel_spin[i];
		wheel_rpm_sum += sim::vehicles::wheel_rpm(
				std::span<const float>(&omega, 1), ratio, w.diff_gear_ratio, land.transmission_efficiency);
		grounded = grounded || ((p_v.wheel_contact >> i) & 1u) != 0;
		++driven;
	}
	if (!grounded || driven == 0) {
		return 0.0f;
	}
	const float load = sim::vehicles::engine_load(wheel_rpm_sum / float(driven), p_v.engine_rpm, p_hull.forward.y,
			core::vec::dot(p_v.linear_velocity, p_hull.forward));
	return std::clamp(p_speed * sim::vehicles::kKmhPerMs * kLoadFadeKmh, 0.0f, 1.0f) * load;
}

VehicleChassisMotion &chassis_of(VehicleSoundMemory &r_memory, uint32_t p_key, const host::PresentedVehicle &p_v) {
	for (VehicleChassisMotion &m : r_memory.chassis) {
		if (m.key == p_key) {
			return m;
		}
	}
	r_memory.chassis.push_back(VehicleChassisMotion{ p_key, p_v.tick, p_v.linear_velocity, kVehicleRestGForce, false });
	return r_memory.chassis.back();
}

VehicleTurretMotion &motion_of(VehicleSoundMemory &r_memory, uint32_t p_key, sim::Tick p_tick, float p_angle) {
	for (VehicleTurretMotion &m : r_memory.turrets) {
		if (m.key == p_key) {
			return m;
		}
	}
	r_memory.turrets.push_back(VehicleTurretMotion{ p_key, p_tick, p_angle, 0.0f, 0.0f, 0.0f, false });
	return r_memory.turrets.back();
}

bool first_person_view(const core::gameplay::VehicleSoundDef &p_def, uint32_t p_camera) {
	if (p_def.engines.empty() || p_camera == core::gameplay::kNoVehicleCamera) {
		return false;
	}
	const std::vector<uint32_t> &cameras = p_def.engines.front().first_person_cameras;
	return std::find(cameras.begin(), cameras.end(), p_camera) != cameras.end();
}

}

// SOURCE: notes R5-VS1 Rule 4 (specific force with gravity, signed by its up part, a step of 0.2 g or more taken at 5 %).
float vehicle_g_force_step(float p_g_force, const core::Vec3 &p_previous_velocity, const core::Vec3 &p_velocity,
		const core::Vec3 &p_up, float p_dt) {
	if (!(p_dt > 0.0f)) {
		return p_g_force;
	}
	float target = kVehicleRestGForce;
	if (core::vec::dot(p_previous_velocity, p_previous_velocity) > kGForceMovingSpeedSquared) {
		const core::Vec3 a =
				core::vec::add(core::vec::scale(core::vec::sub(p_velocity, p_previous_velocity), 1.0f / p_dt),
						core::Vec3{ 0.0f, kGravity, 0.0f });
		const float along = core::vec::dot(a, p_up);
		const float sign = along > 0.0f ? 1.0f : (along < 0.0f ? -1.0f : 0.0f);
		target = sign * core::vec::length(a) / kGravity;
	}
	float d = target - p_g_force;
	if (std::fabs(d) >= kGForceSnap) {
		d *= kGForceSmoothing;
	}
	return p_g_force + d;
}

bool own_view_first_person(std::span<const host::PresentedVehicle> p_vehicles, const host::LevelVehicles *p_level,
		uint8_t p_own_slot, uint32_t p_camera) {
	const std::optional<host::PresentedSeat> seat = host::vehicle_seat_of(p_vehicles, p_own_slot);
	if (!seat) {
		return true;
	}
	if (p_level == nullptr || seat->spawner >= p_level->spawners.size()) {
		return false;
	}
	const uint16_t kind = p_level->spawners[seat->spawner].kind;
	if (kind >= p_level->kinds.size() || seat->seat >= p_level->kinds[kind].seat_defs.size()) {
		return false;
	}
	for (const core::gameplay::VehicleCameraDef &c : p_level->kinds[kind].seat_defs[seat->seat].cameras) {
		if (c.instance == p_camera && p_camera != core::gameplay::kNoVehicleCamera) {
			return c.first_person;
		}
	}
	return false;
}

// SOURCE: notes R5-VS1 Rule 2 item 10 (the slip ratio sum, plus off-ground wheels and half the second-contact wheels
// above 1/36 m/s).
float vehicle_slip_input(
		float p_slip_ratio_sum, uint32_t p_wheels_off_ground, uint32_t p_wheels_in_second_contact, float p_speed) {
	if (!(p_speed > kSlipGateSpeed)) {
		return p_slip_ratio_sum;
	}
	return p_slip_ratio_sum + float(p_wheels_off_ground) + kSecondContactSlipWeight * float(p_wheels_in_second_contact);
}

void vehicle_sound_sources(std::span<const host::PresentedVehicle> p_vehicles, const host::LevelVehicles &p_level,
		const VehicleAudioAssets &p_assets, const VehicleListenerView &p_view, VehicleSoundMemory &r_memory,
		std::vector<VehicleSoundSource> &r_out) {
	r_out.clear();
	for (VehicleTurretMotion &m : r_memory.turrets) {
		m.seen = false;
	}
	for (VehicleChassisMotion &m : r_memory.chassis) {
		m.seen = false;
	}
	const std::optional<host::PresentedSeat> own = host::vehicle_seat_of(p_vehicles, p_view.own_slot);
	for (const host::PresentedVehicle &v : p_vehicles) {
		if (v.spawner >= p_level.spawners.size()) {
			continue;
		}
		const uint16_t kind = p_level.spawners[v.spawner].kind;
		if (kind >= p_level.kinds.size() || kind >= p_assets.engine_program.size() || !p_level.kinds[kind].model) {
			continue;
		}
		const core::gameplay::VehicleSoundDef &def = p_assets.defs[kind];
		const bool engine = p_assets.engine_program[kind] != kNoVehicleProgram && !def.engines.empty();
		const host::LevelVehicleKind &k = p_level.kinds[kind];
		const sim::VehicleModel &model = *k.model;
		const core::Transform rot = core::Transform::from_rotation_translation(v.rotation, Vec3{});
		core::Transform hull = rot;
		hull.trans = core::vec::sub(v.com_position, rot.transform_vector(model.center_of_mass));
		if (!core::finite(hull.trans)) {
			continue;
		}
		const core::Transform at = engine ? hull.then_inner(def.engines.front().transform) : hull;
		const uint32_t key = vehicle_sound_key(v.spawner, v.generation, kVehicleEngineSound);
		VehicleChassisMotion &chassis = chassis_of(r_memory, key, v);
		chassis.seen = true;
		// INFERRED: GForce steps once per presented tick (R5-VS1 did not trace its cadence). Settle: a note on the cadence.
		if (v.tick != chassis.tick) {
			const float seconds = float(v.tick - chassis.tick) / float(sim::kSimHz);
			chassis.g_force =
					vehicle_g_force_step(chassis.g_force, chassis.velocity, v.linear_velocity, hull.up, seconds);
			chassis.velocity = v.linear_velocity;
			chassis.tick = v.tick;
		}
		VehicleSoundSource s;
		s.key = key;
		s.program = p_assets.engine_program[kind];
		s.kind = VehicleSoundKind::Engine;
		s.running = host::vehicle_engine_running(v);
		s.emitter.position = at.trans;
		s.emitter.velocity = v.linear_velocity;
		s.emitter.forward = at.forward;
		const bool local = own && own->spawner == v.spawner;
		const bool first_person = local && first_person_view(def, p_view.own_camera);
		const float speed = core::vec::length(v.linear_velocity);
		bool any_contact = false;
		uint32_t off_ground = 0;
		for (size_t i = 0; i < v.wheel_count && i < model.wheel_count; ++i) {
			const bool contact = ((v.wheel_contact >> i) & 1u) != 0;
			any_contact = any_contact || contact;
			off_ground += contact ? 0u : 1u;
		}
		// SOURCE: notes R5-VS1 Rule 2 (AngularVelocity w.y, RelativeVelocity |listener - chassis|, Roll (1 - up.y) / 2,
		// Tilt forward.y, Velocity |v|, SpringCompression max (L - x) / L) and R5-VS2 3a (InteriorCam, #2045231802).
		// STAND-IN: Contact has no second contact state, Slip no tyre slip ratios, RPM no fake rpm and Boost is 0.
		// Settle: simulate those physics values.
		auto &p = s.params;
		p[size_t(P::AngularVelocity)] = v.angular_velocity.y;
		p[size_t(P::Contact)] = any_contact ? 1.0f : 0.0f;
		p[size_t(P::Gear)] = float(v.gear);
		p[size_t(P::InDriverPos)] = s.running ? 1.0f : 0.0f;
		p[size_t(P::InteriorCam)] = first_person ? 1.0f : 0.0f;
		p[size_t(P::LocalPlayerInVehicle)] = local ? 1.0f : 0.0f;
		p[size_t(P::RelativeVelocity)] = core::vec::length(core::vec::sub(p_view.listener.velocity, v.linear_velocity));
		p[size_t(P::Roll)] = 0.5f * (1.0f - at.up.y);
		p[size_t(P::Rpm)] = v.engine_rpm;
		p[size_t(P::Slip)] = vehicle_slip_input(0.0f, off_ground, 0, speed);
		p[size_t(P::SpringCompression)] = spring_compression(v, model);
		p[size_t(P::Throttle)] = v.throttle;
		p[size_t(P::Tilt)] = at.forward.y;
		p[size_t(P::Velocity)] = speed;
		p[size_t(P::Load)] = engine_load_input(v, model, hull, speed);
		p[size_t(P::Boost)] = 0.0f;
		p[size_t(P::GForce)] = chassis.g_force;
		p[size_t(P::Health)] = v.health;
		p[size_t(P::DisabledDamageThreshold)] = p_assets.disabled_fraction[kind];
		if (engine) {
			r_out.push_back(s);
		}
		for (const auto &[turret, program] : p_assets.turret_programs[kind]) {
			if (size_t(turret) >= v.turret_count) {
				continue;
			}
			const uint32_t turret_key = vehicle_sound_key(v.spawner, v.generation, 1u + uint32_t(turret));
			const float angle = v.turret_angle[size_t(turret)];
			VehicleTurretMotion &m = motion_of(r_memory, turret_key, v.tick, angle);
			m.seen = true;
			if (v.tick != m.tick) {
				const float seconds = float(v.tick - m.tick) / float(sim::kSimHz);
				m.speed = seconds > 0.0f ? std::fabs(sim::vehicles::wrap_angle(angle - m.angle)) / seconds : 0.0f;
				m.tick = v.tick;
				m.angle = angle;
			}
			m.previous_rpm = m.rpm;
			// SOURCE: notes R5-VS1 Rule 7 (rotation speed x 9.549296 as RPM).
			// INFERRED: the speed is a magnitude, as R5-VS1 infers. Settle: a note on the getter's sign.
			m.rpm = m.speed * sim::vehicles::kRadPerSecToRpm;
			VehicleSoundSource t;
			t.key = turret_key;
			t.program = program;
			t.kind = VehicleSoundKind::Spin;
			t.emitter = s.emitter;
			for (const core::gameplay::VehicleTurretSound &c : def.turrets) {
				if (c.turret == turret) {
					// STAND-IN: the spin sound sits at the turret's rest position, where notes R5-VS1 Rule 7 follow the
					// child part's world transform. Settle: the turned turret's transform.
					t.emitter.position = hull.transform_point(c.position);
					t.start_rpm = c.start_rpm;
					t.stop_rpm = c.stop_rpm;
				}
			}
			t.previous_rpm = m.previous_rpm;
			t.params[size_t(P::Rpm)] = m.rpm;
			r_out.push_back(t);
		}
	}
	std::erase_if(r_memory.turrets, [](const VehicleTurretMotion &m) { return !m.seen; });
	std::erase_if(r_memory.chassis, [](const VehicleChassisMotion &m) { return !m.seen; });
}

}

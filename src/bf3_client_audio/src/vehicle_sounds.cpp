#include "bf3/client_audio/vehicle_sounds.h"

#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/vehicles/turret_rules.h"

#include <algorithm>
#include <cmath>
#include <numbers>

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

float spring_compression(const host::PresentedVehicle &p_v, const sim::VehicleModel &p_model) {
	float out = 0.0f;
	for (size_t i = 0; i < p_v.wheel_count && i < p_model.wheel_count; ++i) {
		const sim::VehicleWheelModel &w = p_model.wheels[i];
		const float travel = w.suspension_max - w.suspension_min;
		if (travel > 0.0f) {
			out = std::max(out, std::clamp((w.suspension_max - p_v.suspension_length[i]) / travel, 0.0f, 1.0f));
		}
	}
	return out;
}

VehicleTurretMotion &motion_of(VehicleSoundMemory &r_memory, uint32_t p_key, sim::Tick p_tick, float p_angle) {
	for (VehicleTurretMotion &m : r_memory.turrets) {
		if (m.key == p_key) {
			return m;
		}
	}
	r_memory.turrets.push_back(VehicleTurretMotion{ p_key, p_tick, p_angle, 0.0f, false });
	return r_memory.turrets.back();
}

bool inside(const sim::vehicles::HullBox &p_box, const core::Transform &p_hull, const Vec3 &p_point) {
	const Vec3 d = core::vec::sub(p_point, p_hull.trans);
	const Vec3 local{ core::vec::dot(d, p_hull.right), core::vec::dot(d, p_hull.up),
		core::vec::dot(d, p_hull.forward) };
	return std::fabs(local.x - p_box.centre.x) <= p_box.half.x && std::fabs(local.y - p_box.centre.y) <= p_box.half.y &&
			std::fabs(local.z - p_box.centre.z) <= p_box.half.z;
}

}

void vehicle_sound_sources(std::span<const host::PresentedVehicle> p_vehicles, const host::LevelVehicles &p_level,
		const VehicleAudioAssets &p_assets, const VehicleListenerView &p_view, VehicleSoundMemory &r_memory,
		std::vector<VehicleSoundSource> &r_out) {
	r_out.clear();
	for (VehicleTurretMotion &m : r_memory.turrets) {
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
		const bool engine = p_assets.engine_program[kind] != kNoVehicleProgram && !p_assets.defs[kind].engines.empty();
		const host::LevelVehicleKind &k = p_level.kinds[kind];
		const sim::VehicleModel &model = *k.model;
		const core::Transform rot = core::Transform::from_rotation_translation(v.rotation, Vec3{});
		core::Transform hull = rot;
		hull.trans = core::vec::sub(v.com_position, rot.transform_vector(model.center_of_mass));
		if (!core::finite(hull.trans)) {
			continue;
		}
		VehicleSoundSource s;
		s.key = vehicle_sound_key(v.spawner, v.generation, kVehicleEngineSound);
		s.program = p_assets.engine_program[kind];
		s.running = v.seat_count > 0 && v.occupants[0] != net::kNoVehicleOccupant;
		s.emitter.position = engine ? hull.transform_point(p_assets.defs[kind].engines.front().position) : hull.trans;
		s.emitter.velocity = v.linear_velocity;
		s.emitter.forward = hull.forward;
		const bool local = own && own->spawner == v.spawner;
		const float speed = core::vec::length(v.linear_velocity);
		const float forward_speed = core::vec::dot(v.linear_velocity, hull.forward);
		uint32_t contacts = 0;
		float slip = 0.0f;
		uint32_t slipping = 0;
		for (size_t i = 0; i < v.wheel_count && i < model.wheel_count; ++i) {
			if ((v.wheel_contact >> i) & 1u) {
				++contacts;
				if (model.wheels[i].driven) {
					slip += v.wheel_spin[i] * model.wheels[i].radius - forward_speed;
					++slipping;
				}
			}
		}
		const float contact = v.wheel_count > 0 ? float(contacts) / float(v.wheel_count) : 0.0f;
		auto &p = s.params;
		p[size_t(P::Throttle)] = v.throttle;
		p[size_t(P::Rpm)] = v.engine_rpm;
		p[size_t(P::Gear)] = float(v.gear);
		p[size_t(P::Load)] = std::fabs(v.throttle) * contact;
		p[size_t(P::Velocity)] = speed;
		p[size_t(P::AngularVelocity)] = core::vec::dot(v.angular_velocity, hull.up);
		p[size_t(P::RelativeVelocity)] = core::vec::length(core::vec::sub(v.linear_velocity, p_view.listener.velocity));
		p[size_t(P::Slip)] = slipping > 0 ? slip / float(slipping) : 0.0f;
		p[size_t(P::Tilt)] = std::asin(std::clamp(hull.forward.y, -1.0f, 1.0f));
		p[size_t(P::Contact)] = contact;
		p[size_t(P::Boost)] = 0.0f;
		p[size_t(P::SpringCompression)] = spring_compression(v, model);
		p[size_t(P::InteriorCam)] = local && inside(k.box, hull, p_view.listener.position) ? 1.0f : 0.0f;
		p[size_t(P::LocalPlayerInVehicle)] = local ? 1.0f : 0.0f;
		p[size_t(P::Health)] = v.health;
		p[size_t(P::DisabledDamageThreshold)] = p_assets.disabled_fraction[kind];
		p[size_t(P::InDriverPos)] = local && own->seat == 0 ? 1.0f : 0.0f;
		if (engine) {
			r_out.push_back(s);
		}
		for (const auto &[turret, program] : p_assets.turret_programs[kind]) {
			if (size_t(turret) >= v.turret_count) {
				continue;
			}
			const uint32_t key = vehicle_sound_key(v.spawner, v.generation, 1u + uint32_t(turret));
			const float angle = v.turret_angle[size_t(turret)];
			VehicleTurretMotion &m = motion_of(r_memory, key, v.tick, angle);
			m.seen = true;
			if (v.tick != m.tick) {
				const float seconds = float(v.tick - m.tick) / float(sim::kSimHz);
				m.speed = seconds > 0.0f ? std::fabs(sim::vehicles::wrap_angle(angle - m.angle)) / seconds : 0.0f;
				m.tick = v.tick;
				m.angle = angle;
			}
			VehicleSoundSource t = s;
			for (const core::gameplay::VehicleTurretSound &c : p_assets.defs[kind].turrets) {
				if (c.turret == turret) {
					t.emitter.position = hull.transform_point(c.position);
				}
			}
			t.key = key;
			t.program = program;
			const int8_t seat = k.armament && size_t(turret) < k.armament->turrets.size()
					? k.armament->turrets[size_t(turret)].seat
					: int8_t(-1);
			t.running = seat >= 0 && seat < v.seat_count && v.occupants[size_t(seat)] != net::kNoVehicleOccupant &&
					m.speed > 0.0f;
			t.params[size_t(P::Rpm)] = m.speed * 60.0f / (2.0f * float(std::numbers::pi));
			r_out.push_back(t);
		}
	}
	std::erase_if(r_memory.turrets, [](const VehicleTurretMotion &m) { return !m.seen; });
}

}

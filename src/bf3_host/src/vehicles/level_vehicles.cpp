#include "bf3/host/level_vehicles.h"

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/vehicle_spawns.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/vehicles/vehicle_targets.h"
#include "bf3/sim/vehicles/vehicle_weapon_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

sim::VehiclePlacement vehicle_spawn_placement(const core::level::LevelTransform &p_transform, float p_lift) {
	sim::VehiclePlacement p;
	p.position = { p_transform.trans[0], p_transform.trans[1] + p_lift, p_transform.trans[2] };
	const double heading = std::atan2(double(p_transform.forward[0]), double(p_transform.forward[2]));
	p.rotation = { 0.0f, float(std::sin(heading / 2.0)), 0.0f, float(std::cos(heading / 2.0)) };
	return p;
}

bool land_jeep(const sim::VehicleModel &p_model) {
	return p_model.wheel_count == 4;
}

std::optional<core::Vec3> vehicle_exit_point(const LevelVehicleKind &p_kind, uint8_t p_seat) {
	if (p_kind.exit_points.empty() || p_seat >= p_kind.seat_defs.size()) {
		return std::nullopt;
	}
	const core::Vec3 &seat = p_kind.seat_defs[p_seat].transform.trans;
	const core::Transform *best = &p_kind.exit_points.front();
	float nearest = INFINITY;
	for (const core::Transform &e : p_kind.exit_points) {
		const float dx = e.trans[0] - seat[0], dz = e.trans[2] - seat[2];
		const float d = dx * dx + dz * dz;
		if (d < nearest) {
			nearest = d;
			best = &e;
		}
	}
	return best->trans;
}

sim::Vec3 vehicle_seat_point(const LevelVehicleKind &p_kind, uint8_t p_seat, const std::array<float, 4> &p_rotation,
		const sim::Vec3 &p_origin) {
	const core::Vec3 at = p_seat < p_kind.seat_defs.size() ? p_kind.seat_defs[p_seat].transform.trans : core::Vec3{};
	return sim::vehicles::hull_transform(p_rotation, p_origin).transform_point(at);
}

sim::Vec3 vehicle_body_origin(const sim::VehicleModel &p_model, const sim::VehicleBodyState &p_body) {
	const sim::Vec3 com = sim::vehicles::hull_transform(p_body.rotation, {}).transform_point(p_model.center_of_mass);
	return { p_body.com_position[0] - com[0], p_body.com_position[1] - com[1], p_body.com_position[2] - com[2] };
}

bool land_tracked(const sim::VehicleModel &p_model) {
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		if (p_model.wheels[i].track_steer != 0) {
			return true;
		}
	}
	return false;
}

namespace {

sim::vehicles::HealthZoneRule zone_rule(const core::gameplay::VehicleHealthZoneDef &p_zone) {
	sim::vehicles::HealthZoneRule z;
	z.max_health = std::max(p_zone.max_health, 0.0f);
	z.max_shield = std::max(p_zone.max_shield_health, 0.0f);
	z.min_damage_angle = p_zone.min_damage_angle;
	z.damage_angle_multiplier = p_zone.damage_angle_multiplier;
	z.use_damage_angle = p_zone.use_damage_angle;
	return z;
}

struct WeaponKey {
	std::string firing;
	float reload = 1;
	float overheat = 1;
	bool operator==(const WeaponKey &) const = default;
};

uint16_t vehicle_weapon(const LevelDatabase &p_db, const sim::vehicles::ArmamentWeapon &p_weapon,
		std::vector<WeaponKey> &r_keys, LevelVehicles &r_out) {
	const WeaponKey key{ p_weapon.firing, p_weapon.reload_time_multiplier, p_weapon.overheat_drop_multiplier };
	const auto known = std::find(r_keys.begin(), r_keys.end(), key);
	if (known != r_keys.end()) {
		return uint16_t(known - r_keys.begin());
	}
	core::Result<core::gameplay::WeaponDef> def =
			core::gameplay::resolve_vehicle_weapon(p_db.level->db(), p_weapon.firing);
	if (!def) {
		r_out.notes.push_back("vehicle weapon " + p_weapon.firing + " not read: " + def.error().message);
		return kNoVehicleWeapon;
	}
	core::gameplay::FireFunction &fire = def->fire;
	fire.reload_time = sim::vehicles::vehicle_reload_seconds(fire.reload_time, key.reload);
	fire.reload_time_bullets_left = sim::vehicles::vehicle_reload_seconds(fire.reload_time_bullets_left, key.reload);
	fire.heat_drop_per_second = sim::vehicles::vehicle_heat_drop_per_second(fire.heat_drop_per_second, key.overheat);
	r_keys.push_back(key);
	r_out.weapons.push_back(std::move(*def));
	return uint16_t(r_out.weapons.size() - 1);
}

}

sim::vehicles::VehicleDamageRule vehicle_damage_rule(const core::gameplay::VehicleDef &p_def) {
	sim::vehicles::VehicleDamageRule r;
	r.zones = { zone_rule(p_def.front), zone_rule(p_def.rear), zone_rule(p_def.left), zone_rule(p_def.right) };
	r.share_damage = p_def.health_zones_share_damage;
	r.angle_of_impact.enabled = p_def.angle_of_impact.enabled;
	r.angle_of_impact.zone12_degrees = p_def.angle_of_impact.zone12_delimiter;
	r.angle_of_impact.zone23_degrees = p_def.angle_of_impact.zone23_delimiter;
	r.angle_of_impact.multiplier = p_def.angle_of_impact.multiplier;
	r.armor_multiplier = p_def.armor_multiplier;
	r.disabled_threshold = p_def.disabled_damage_threshold;
	r.regeneration_delay = p_def.regeneration_delay * p_def.regeneration_delay_multiplier;
	r.regeneration_rate = p_def.regeneration_rate * p_def.regeneration_rate_multiplier;
	r.decay_delay = p_def.decay_delay;
	r.decay_rate = p_def.decay_rate;
	r.critical_damage_time = p_def.critical_damage_time;
	r.pre_explosion_time = p_def.pre_explosion_time;
	r.pre_destruction_threshold = p_def.pre_destruction_damage_threshold;
	r.upside_down_degrees = p_def.upside_down_angle;
	r.upside_down_damage = p_def.upside_down_damage;
	r.upside_down_delay = p_def.upside_down_damage_delay;
	return r;
}

core::Result<std::shared_ptr<const LevelVehicles>> load_level_vehicles(
		const LevelDatabase &p_db, const sim::VehicleTuning &p_tuning) {
	if (p_db.level == nullptr || p_db.world == nullptr) {
		return core::make_error("level vehicles: no level database");
	}
	core::Result<std::vector<core::level::VehicleSpawnerDef>> spawners =
			core::level::read_vehicle_spawners(*p_db.world);
	if (!spawners) {
		return core::error_at("level vehicles", spawners.error());
	}
	auto out = std::make_shared<LevelVehicles>();
	out->tuning = p_tuning;
	std::vector<std::string> refused;
	std::vector<WeaponKey> weapon_keys;
	for (const core::level::VehicleSpawnerDef &s : *spawners) {
		if (s.blueprint.empty() || std::find(refused.begin(), refused.end(), s.blueprint) != refused.end()) {
			continue;
		}
		auto kind = std::find_if(out->kinds.begin(), out->kinds.end(),
				[&](const LevelVehicleKind &k) { return k.blueprint == s.blueprint; });
		if (kind == out->kinds.end()) {
			core::Result<std::shared_ptr<const sim::VehicleModel>> model =
					sim::load_vehicle_model(p_db.level->db(), p_db.level->mount(), s.blueprint, p_tuning);
			if (!model) {
				out->notes.push_back("vehicle " + s.blueprint + " not drivable: " + model.error().message);
				refused.push_back(s.blueprint);
				continue;
			}
			if (!land_jeep(**model) && !land_tracked(**model)) {
				out->notes.push_back("vehicle " + s.blueprint + " has " + std::to_string((*model)->wheel_count) +
						" wheels, not a jeep or a tracked vehicle");
				refused.push_back(s.blueprint);
				continue;
			}
			core::Result<core::gameplay::VehicleDef> def =
					core::gameplay::read_vehicle_def(p_db.level->db(), s.blueprint);
			if (!def) {
				out->notes.push_back("vehicle " + s.blueprint + " has no seats: " + def.error().message);
				refused.push_back(s.blueprint);
				continue;
			}
			LevelVehicleKind k;
			k.blueprint = s.blueprint;
			k.mesh = def->mesh;
			k.model = *model;
			k.seats = uint8_t(std::min(def->seats.size(), net::kMaxVehicleSeats));
			k.seat_defs.assign(def->seats.begin(), def->seats.begin() + k.seats);
			k.exit_points = def->exit_points;
			k.component_meshes = def->component_meshes;
			for (size_t seat = 0; seat < k.seats; ++seat) {
				k.exposed[seat] = def->seats[seat].shows_soldier;
			}
			k.damage = vehicle_damage_rule(*def);
			k.box = sim::vehicles::chassis_box(**model);
			k.material = def->material;
			k.min_speed_for_mine = def->min_speed_for_mine_activation;
			k.run_over = { def->kill_soldier_collision_speed_threshold, def->ignore_soldier_collision_normal };
			k.explosion = def->explosion;
			k.explosion_effect = def->explosion_effect;
			k.disabled_input = def->disabled_input;
			k.damage_visuals = def->damage_visuals;
			for (const core::gameplay::VehicleSeatDef &seat : def->seats) {
				k.entry_radius = std::max(k.entry_radius, std::isfinite(seat.entry_radius) ? seat.entry_radius : 0.0f);
			}
			core::Result<sim::vehicles::VehicleArmament> armament = sim::vehicles::build_vehicle_armament(*def);
			if (!armament) {
				out->notes.push_back("vehicle " + s.blueprint + " unarmed: " + armament.error().message);
			} else {
				for (const sim::vehicles::ArmamentWeapon &w : armament->weapons) {
					k.weapon_defs.push_back(vehicle_weapon(p_db, w, weapon_keys, *out));
				}
				k.armament = std::make_shared<const sim::vehicles::VehicleArmament>(std::move(*armament));
			}
			if (k.seats == 0) {
				out->notes.push_back("vehicle " + s.blueprint + " has no seats");
				refused.push_back(s.blueprint);
				continue;
			}
			out->kinds.push_back(std::move(k));
			kind = out->kinds.end() - 1;
		}
		LevelVehicleSpawner sp;
		sp.object = s.object;
		sp.kind = uint16_t(kind - out->kinds.begin());
		sp.placement = vehicle_spawn_placement(s.transform, kVehicleSpawnLift);
		sp.team = uint8_t(std::clamp(s.team, 0, int(net::kMaxTeam)));
		sp.enabled = s.enabled;
		sp.initial_spawn_delay = std::max(s.initial_spawn_delay, 0.0f);
		sp.spawn_delay = std::max(s.spawn_delay, 0.0f);
		sp.apply_damage_to_abandoned = s.apply_damage_to_abandoned;
		sp.time_until_abandoned = std::max(s.time_until_abandoned, 0.0f);
		sp.time_until_abandoned_is_destroyed = std::max(s.time_until_abandoned_is_destroyed, 0.0f);
		sp.keep_alive_radius = std::max(s.keep_alive_radius, 0.0f);
		sp.respawn_range = std::max(s.respawn_range, 0.0f);
		out->spawners.push_back(sp);
	}
	return std::shared_ptr<const LevelVehicles>(std::move(out));
}

}

// read_vehicle_spawners (S2, include/bf3/core/level/vehicle_spawns.h; docs/design/s2.md §4.3): every
// VehicleSpawnReferenceObjectData placement of a traversed level x mode with its world transform and its fields.
// Every field is the effective value: a PropertyConnection of the object's scope that feeds it wins over the
// object's own field (Scopes::property), as spawn_points.cpp reads the soldier spawns. Charges the World's
// WorkBudget: one unit per examined object, the blueprint name by its length.
#include "bf3/core/level/vehicle_spawns.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/level/connections.h"

#include <cmath>
#include <optional>
#include <string>

namespace bf3::core::level {

namespace {

// The fed value of p_field (a PropertyConnection into it), else the object's own field.
struct SpawnerFields {
	const World &world;
	const PlacedObject &object;

	Result<std::optional<Property>> fed(std::string_view p_field) const {
		const EbxInstance &target = object.reference ? *object.reference : object.entity;
		Result<std::optional<Property>> r = world.scopes.property(object.scope, target, field_id(p_field));
		if (!r) {
			return r;
		}
		if (*r || !object.reference) {
			return r;
		}
		return world.scopes.property(object.scope, object.entity, field_id(p_field));
	}

	Result<bool> boolean(std::string_view p_field, bool p_fallback) const {
		Result<std::optional<Property>> f = fed(p_field);
		BF3_TRY(f);
		if (*f) {
			if (const std::optional<bool> b = (*f)->as_bool()) {
				return *b;
			}
		}
		const std::optional<EbxValue> own = object.entity.get(p_field);
		const std::optional<bool> b = own ? own->as_bool() : std::nullopt;
		return b.value_or(p_fallback);
	}

	// A finite number (fed or own); p_fallback otherwise.
	Result<float> number(std::string_view p_field, float p_fallback) const {
		Result<std::optional<Property>> f = fed(p_field);
		BF3_TRY(f);
		std::optional<double> v;
		if (*f) {
			if (const std::optional<float> x = (*f)->as_f32()) {
				v = double(*x);
			} else if (const std::optional<int64_t> i = (*f)->as_i64()) {
				v = double(*i);
			}
		}
		if (!v) {
			if (const std::optional<EbxValue> own = object.entity.get(p_field)) {
				if (const std::optional<double> x = own->as_f64()) {
					v = *x;
				} else if (const std::optional<int64_t> i = own->as_i64()) {
					v = double(*i);
				}
			}
		}
		if (!v || !std::isfinite(*v) || std::fabs(*v) > 1e9) {
			return p_fallback;
		}
		return float(*v);
	}

	// TeamId: an enum field (TeamNeutral 0, Team1 1, ...) or a fed `TeamId TeamN` constant.
	Result<int> team(std::string_view p_field) const {
		Result<std::optional<Property>> f = fed(p_field);
		BF3_TRY(f);
		if (*f) {
			if (const std::optional<uint32_t> t = (*f)->as_team(); t && *t < 1024u) {
				return int(*t);
			}
			if (const std::optional<int64_t> v = (*f)->as_i64(); v && *v >= 0 && *v < 1024) {
				return int(*v);
			}
		}
		const std::optional<EbxValue> own = object.entity.get(p_field);
		if (own) {
			if (const std::optional<int32_t> e = own->enum_value(); e && *e >= 0 && *e < 1024) {
				return int(*e);
			}
			if (const std::optional<int64_t> v = own->as_i64(); v && *v >= 0 && *v < 1024) {
				return int(*v);
			}
		}
		return 0;
	}
};

} // namespace

Result<std::vector<VehicleSpawnerDef>> read_vehicle_spawners(const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	const EbxResolver &db = p_world.scopes.db();
	std::vector<VehicleSpawnerDef> out;
	for (uint32_t i = 0; i < p_world.objects.size(); ++i) {
		const PlacedObject &object = p_world.objects[i];
		if (object.kind != ObjectKind::VehicleSpawnReference) {
			continue;
		}
		BF3_TRY(budget.charge(1));
		const SpawnerFields read{ p_world, object };
		VehicleSpawnerDef s;
		s.object = i;
		s.transform = object.transform;
		if (const std::optional<EbxInstance> blueprint = db.follow(object.entity.data(), "Blueprint")) {
			const std::optional<std::string_view> name = db.name_of(blueprint->partition().guid());
			if (name) {
				BF3_TRY(charge_text(budget, *name));
				s.blueprint.reserve(name->size());
				for (const char c : *name) {
					s.blueprint.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
				}
			}
		}
		Result<int> team = read.team("Team");
		BF3_TRY(team);
		s.team = *team;
		Result<bool> enabled = read.boolean("Enabled", true);
		BF3_TRY(enabled);
		s.enabled = *enabled;
		Result<bool> auto_spawn = read.boolean("AutoSpawn", true);
		BF3_TRY(auto_spawn);
		s.auto_spawn = *auto_spawn;
		Result<bool> initial_auto_spawn = read.boolean("InitialAutoSpawn", true);
		BF3_TRY(initial_auto_spawn);
		s.initial_auto_spawn = *initial_auto_spawn;
		const std::pair<const char *, float *> numbers[] = { { "SpawnDelay", &s.spawn_delay }, { "InitialSpawnDelay", &s.initial_spawn_delay },
			{ "SpawnAreaRadius", &s.spawn_area_radius }, { "RespawnRange", &s.respawn_range }, { "RotationYaw", &s.rotation_yaw },
			{ "RotationPitch", &s.rotation_pitch }, { "RotationRoll", &s.rotation_roll } };
		for (const auto &[field, value] : numbers) {
			Result<float> v = read.number(field, 0.0f);
			BF3_TRY(v);
			*value = *v;
		}
		out.push_back(std::move(s));
	}
	return out;
}

} // namespace bf3::core::level

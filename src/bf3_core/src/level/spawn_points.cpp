#include "bf3/core/level/spawn_points.h"

#include <cmath>
#include <optional>
#include <string>

namespace bf3::core::level {

namespace {

struct FieldReader {
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

	Result<std::optional<bool>> boolean(std::string_view p_field) const {
		Result<std::optional<Property>> f = fed(p_field);
		BF3_TRY(f);
		if (*f) {
			if (const std::optional<bool> b = (*f)->as_bool()) {
				return std::optional<bool>(*b);
			}
		}
		const std::optional<EbxValue> own = object.entity.get(p_field);
		return own ? own->as_bool() : std::nullopt;
	}

	Result<std::optional<double>> number(std::string_view p_field) const {
		Result<std::optional<Property>> f = fed(p_field);
		BF3_TRY(f);
		if (*f) {
			if (const std::optional<float> v = (*f)->as_f32()) {
				return std::optional<double>(*v);
			}
			if (const std::optional<int64_t> v = (*f)->as_i64()) {
				return std::optional<double>(double(*v));
			}
		}
		const std::optional<EbxValue> own = object.entity.get(p_field);
		if (!own) {
			return std::optional<double>();
		}
		if (const std::optional<double> v = own->as_f64()) {
			return std::optional<double>(*v);
		}
		if (const std::optional<int64_t> v = own->as_i64()) {
			return std::optional<double>(double(*v));
		}
		return std::optional<double>();
	}

	Result<std::optional<int>> team(std::string_view p_field) const {
		Result<std::optional<Property>> f = fed(p_field);
		BF3_TRY(f);
		if (*f) {
			if (const std::optional<uint32_t> t = (*f)->as_team(); t && *t < 1024u) {
				return std::optional<int>(int(*t));
			}
			if (const std::optional<int64_t> v = (*f)->as_i64(); v && *v >= 0 && *v < 1024) {
				return std::optional<int>(int(*v));
			}
		}
		const std::optional<EbxValue> own = object.entity.get(p_field);
		if (!own) {
			return std::optional<int>();
		}
		if (const std::optional<int32_t> e = own->enum_value(); e && *e >= 0 && *e < 1024) {
			return std::optional<int>(*e);
		}
		if (const std::optional<int64_t> v = own->as_i64(); v && *v >= 0 && *v < 1024) {
			return std::optional<int>(int(*v));
		}
		return std::optional<int>();
	}
};

}

Result<LevelSpawns> read_level_spawns(const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	LevelSpawns out;
	for (uint32_t i = 0; i < p_world.objects.size(); ++i) {
		const PlacedObject &object = p_world.objects[i];
		if (object.kind != ObjectKind::AlternateSpawn && object.kind != ObjectKind::CharacterSpawnReference) {
			continue;
		}
		BF3_TRY(budget.charge(1));
		const FieldReader read{ p_world, object };
		if (object.kind == ObjectKind::AlternateSpawn) {
			SpawnPointDef p;
			p.object = i;
			p.transform = object.transform;
			Result<std::optional<int>> team = read.team("Team");
			BF3_TRY(team);
			p.team = team->value_or(0);
			Result<std::optional<bool>> enabled = read.boolean("Enabled");
			BF3_TRY(enabled);
			p.enabled = enabled->value_or(true);
			Result<std::optional<double>> priority = read.number("Priority");
			BF3_TRY(priority);
			p.priority = *priority && std::isfinite(**priority) ? float(**priority) : 1.0f;
			out.points.push_back(p);
			continue;
		}
		CharacterSpawnerDef s;
		s.object = i;
		Result<std::optional<int>> team = read.team("Team");
		BF3_TRY(team);
		s.team = team->value_or(0);
		Result<std::optional<bool>> enabled = read.boolean("Enabled");
		BF3_TRY(enabled);
		s.enabled = enabled->value_or(true);
		Result<std::optional<double>> radius = read.number("SpawnProtectionRadius");
		BF3_TRY(radius);
		s.protection_radius = *radius && std::isfinite(**radius) ? float(**radius) : 0.0f;
		Result<std::optional<double>> kills = read.number("SpawnProtectionFriendlyKilledCount");
		BF3_TRY(kills);
		s.friendly_killed_count = *kills && std::isfinite(**kills) && std::fabs(**kills) < 1e15 ? int64_t(**kills) : 0;
		Result<std::optional<double>> kill_time = read.number("SpawnProtectionFriendlyKilledTime");
		BF3_TRY(kill_time);
		s.friendly_killed_time = *kill_time && std::isfinite(**kill_time) ? float(**kill_time) : 0.0f;
		Result<std::optional<bool>> all_teams = read.boolean("SpawnProtectionCheckAllTeams");
		BF3_TRY(all_teams);
		s.check_all_teams = all_teams->value_or(false);
		Result<std::optional<bool>> out_of_sight = read.boolean("TryToSpawnOutOfSight");
		BF3_TRY(out_of_sight);
		s.out_of_sight = out_of_sight->value_or(false);
		Result<std::optional<std::string_view>> sid = charged_str(object.entity.get("LocationTextSid"), budget);
		BF3_TRY(sid);
		if (*sid) {
			s.location_text_sid = std::string(**sid);
		}
		out.spawners.push_back(std::move(s));
	}
	return out;
}

}

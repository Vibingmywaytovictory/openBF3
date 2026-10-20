#include "bf3/core/level/mode_areas.h"

#include "bf3/core/install.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/spawn_points.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>

namespace bf3::core::level {

namespace {

constexpr uint32_t kMaxInterfaceHops = 16;

bool inside_any(std::span<const CombatAreaDef> p_areas, const Vec3 &p_point) {
	if (p_areas.empty()) {
		return true;
	}
	for (const CombatAreaDef &a : p_areas) {
		bool in = false;
		const size_t n = a.polygon.size();
		for (size_t i = 0, j = n - 1; n >= 3 && i < n; j = i++) {
			const float xi = a.polygon[i][0], zi = a.polygon[i][1];
			const float xj = a.polygon[j][0], zj = a.polygon[j][1];
			if ((zi > p_point[2]) != (zj > p_point[2]) && p_point[0] < (xj - xi) * (p_point[2] - zi) / (zj - zi) + xi) {
				in = !in;
			}
		}
		if (in) {
			return true;
		}
	}
	return false;
}

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

Result<std::optional<Property>> fed(const World &p_world, const PlacedObject &p_object, std::string_view p_field) {
	const EbxInstance &target = p_object.reference ? *p_object.reference : p_object.entity;
	Result<std::optional<Property>> r = p_world.scopes.property(p_object.scope, target, field_id(p_field));
	if (!r || *r || !p_object.reference) {
		return r;
	}
	return p_world.scopes.property(p_object.scope, p_object.entity, field_id(p_field));
}

Result<int> team_of(const World &p_world, const PlacedObject &p_object, std::string_view p_field) {
	Result<std::optional<Property>> f = fed(p_world, p_object, p_field);
	BF3_TRY(f);
	if (*f) {
		if (const std::optional<uint32_t> t = (*f)->as_team(); t && *t < 1024u) {
			return int(*t);
		}
		if (const std::optional<int64_t> v = (*f)->as_i64(); v && *v >= 0 && *v < 1024) {
			return int(*v);
		}
	}
	const std::optional<EbxValue> own = p_object.entity.get(p_field);
	if (own) {
		if (const std::optional<int32_t> e = own->enum_value(); e && *e >= 0 && *e < 1024) {
			return int(*e);
		}
	}
	return 0;
}

Result<std::string> enum_of(const World &p_world, const PlacedObject &p_object, std::string_view p_field) {
	WorkBudget &budget = *p_world.budget;
	Result<std::optional<Property>> f = fed(p_world, p_object, p_field);
	BF3_TRY(f);
	if (*f) {
		if (const std::optional<std::string_view> s = (*f)->as_str()) {
			BF3_TRY(charge_text(budget, *s));
			return std::string(*s);
		}
		if ((*f)->kind == Property::Kind::Constant) {
			BF3_TRY(charge_text(budget, (*f)->text));
			const std::string_view text = (*f)->text;
			const size_t space = text.rfind(' ');
			return std::string(space == std::string_view::npos ? text : text.substr(space + 1));
		}
	}
	Result<std::optional<std::string_view>> own = charged_enum_name(p_object.entity.get(p_field), budget);
	BF3_TRY(own);
	return *own ? std::string(**own) : std::string();
}

struct ShapeAt {
	EbxInstance shape;
	uint32_t scope = 0;
};
Result<std::optional<ShapeAt>> follow_area(const World &p_world, uint32_t p_scope, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth,
		bool &r_other) {
	WorkBudget &budget = *p_world.budget;
	if (p_depth > kMaxInterfaceHops || p_scope >= p_world.scopes.size()) {
		r_other = true;
		return std::optional<ShapeAt>();
	}
	const Connections &c = p_world.scopes.connections(p_scope);
	BF3_TRY(budget.charge(1 + c.link.size()));
	const Scope &scope = p_world.scopes.get(p_scope);
	for (const Connection &link : c.link) {
		if (!(link.source == p_source) || link.source_field != p_field) {
			continue;
		}
		BF3_TRY(charge_text(budget, link.target.type_name()));
		if (link.target.is_a("VolumeVectorShapeData")) {
			return std::optional<ShapeAt>(ShapeAt{ link.target, p_scope });
		}
		if (link.target.is_a("InterfaceDescriptorData") && scope.reference && scope.parent) {
			Result<std::optional<ShapeAt>> up = follow_area(p_world, *scope.parent, *scope.reference, link.target_field, p_depth + 1, r_other);
			BF3_TRY(up);
			if (*up) {
				return up;
			}
		} else {
			r_other = true;
		}
	}
	return std::optional<ShapeAt>();
}

}

Result<ModeAreas> read_mode_areas(const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	ModeAreas out;
	const int32_t geometry = field_id("Geometry");
	for (uint32_t i = 0; i < p_world.objects.size(); ++i) {
		const PlacedObject &object = p_world.objects[i];
		BF3_TRY(budget.charge(1));
		if (object.kind == ObjectKind::CombatArea) {
			if (out.combat_areas.size() >= kMaxCombatAreas) {
				return make_error("mode areas: more than " + std::to_string(kMaxCombatAreas) + " combat areas");
			}
			CombatAreaDef area;
			area.trigger = i;
			Result<int> team = team_of(p_world, object, "Team");
			BF3_TRY(team);
			area.team = *team;
			const EbxInstance &source = object.entity;
			bool other = false;
			Result<std::optional<ShapeAt>> shape = follow_area(p_world, object.scope, source, geometry, 0, other);
			BF3_TRY(shape);
			if (!*shape && !other) {

				out.unwired_triggers.push_back(i);
				continue;
			}
			if (!*shape) {
				return make_error("mode areas: combat area trigger " + std::to_string(i) + " (" + object.entity.guid().to_string() +
						") resolves to no VolumeVectorShapeData");
			}
			const PlacedObject *placed = p_world.placed((*shape)->scope, (*shape)->shape);
			const LevelTransform frame = placed != nullptr ? placed->transform : p_world.scopes.get((*shape)->scope).transform;
			if (placed != nullptr) {
				area.shape = uint32_t(placed - p_world.objects.data());
			}
			const std::optional<EbxValue> points = (*shape)->shape.get("Points");
			const OptionalEbxArray list = points ? points->as_array() : OptionalEbxArray();
			if (!list || list->size() < 3) {
				return make_error("mode areas: combat area shape of trigger " + std::to_string(i) + " has fewer than 3 points");
			}
			if (list->size() > kMaxCombatAreaPoints) {
				return make_error("mode areas: combat area shape of trigger " + std::to_string(i) + " has " + std::to_string(list->size()) + " points");
			}
			BF3_TRY(budget.charge(list->size()));
			for (const EbxValue &p : *list) {
				const std::optional<EbxStruct> s = p.as_struct();
				const std::optional<std::array<float, 3>> v = s ? s->vec3() : std::nullopt;
				if (!v || !std::isfinite((*v)[0]) || !std::isfinite((*v)[1]) || !std::isfinite((*v)[2])) {
					return make_error("mode areas: combat area shape of trigger " + std::to_string(i) + " has a bad point");
				}
				const Vec3 w = frame.transform_point(*v);
				if (!std::isfinite(w[0]) || !std::isfinite(w[2]) || std::fabs(w[0]) > 1e6f || std::fabs(w[2]) > 1e6f) {
					return make_error("mode areas: combat area shape of trigger " + std::to_string(i) + " has a point out of range");
				}
				area.polygon.push_back({ w[0], w[2] });
			}
			if (const std::optional<EbxValue> h = (*shape)->shape.get("Height")) {
				const std::optional<float> f = h->as_f32();
				area.height = f && std::isfinite(*f) ? *f : 0.0f;
			}
			out.combat_areas.push_back(std::move(area));
			continue;
		}
		const bool capture = object.kind == ObjectKind::CapturePoint;
		const bool interaction = object.kind == ObjectKind::Other && object.entity.is_a("GameInteractionEntityData");
		if (!capture && !interaction) {
			continue;
		}
		ModeObjectiveDef o;
		o.object = i;
		o.position = object.transform.trans;
		if (capture) {
			Result<std::string> type = enum_of(p_world, object, "CapturableType");
			BF3_TRY(type);
			o.kind = *type == "CTUnableToChangeTeam" ? ModeObjectiveDef::Kind::Hq : ModeObjectiveDef::Kind::Flag;
			Result<int> team = team_of(p_world, object, "InitialOwnerTeam");
			BF3_TRY(team);
			o.team = *team;
		} else {
			Result<std::string> type = enum_of(p_world, object, "InteractionEntityType");
			BF3_TRY(type);
			if (*type != "IET_RushCrateArm") {
				continue;
			}
			o.kind = ModeObjectiveDef::Kind::Mcom;
		}
		if (const std::optional<std::string_view> label = p_world.scopes.db().name_of(object.entity.partition().guid())) {
			BF3_TRY(charge_text(budget, *label));
			o.scope_label = std::string(*label);
		}
		out.objectives.push_back(std::move(o));
	}
	return out;
}

Result<std::vector<WaterPlane>> read_water_planes(const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	std::vector<WaterPlane> out;
	for (const PlacedObject &object : p_world.objects) {
		BF3_TRY(budget.charge(1));
		if (!object.entity.is_a("WaterEntityData")) {
			continue;
		}
		const std::optional<EbxInstance> asset = p_world.scopes.db().follow(object.entity.data(), "Asset");
		if (!asset) {
			continue;
		}
		const std::vector<EbxInstance> lakes = asset->partition().instances_of("LakeData");
		BF3_TRY(budget.charge(1 + lakes.size()));
		for (const EbxInstance &lake : lakes) {
			if (out.size() >= kMaxCombatAreas) {
				return make_error("water: more than " + std::to_string(kMaxCombatAreas) + " lakes");
			}
			const std::optional<EbxValue> points = lake.get("Points");
			const OptionalEbxArray list = points ? points->as_array() : OptionalEbxArray();
			if (!list || list->size() < 3) {
				continue;
			}
			if (list->size() > kMaxCombatAreaPoints) {
				return make_error("water: a lake has " + std::to_string(list->size()) + " points");
			}
			BF3_TRY(budget.charge(list->size()));
			WaterPlane plane;
			plane.y = -std::numeric_limits<float>::infinity();
			for (const EbxValue &p : *list) {
				const std::optional<EbxStruct> s = p.as_struct();
				const std::optional<std::array<float, 3>> v = s ? s->vec3() : std::nullopt;
				if (!v || !std::isfinite((*v)[0]) || !std::isfinite((*v)[1]) || !std::isfinite((*v)[2])) {
					return make_error("water: a lake has a bad point");
				}
				const Vec3 w = object.transform.transform_point(*v);
				if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2]) || std::fabs(w[0]) > 1e6f || std::fabs(w[1]) > 1e6f ||
						std::fabs(w[2]) > 1e6f) {
					return make_error("water: a lake point is out of range");
				}
				plane.y = std::max(plane.y, w[1]);
				plane.polygon.push_back({ w[0], w[2] });
			}
			out.push_back(std::move(plane));
		}
	}
	return out;
}

bool water_plane_covers(const WaterPlane &p_plane, float p_x, float p_z) {
	bool in = false;
	const size_t n = p_plane.polygon.size();
	for (size_t i = 0, j = n - 1; n >= 3 && i < n; j = i++) {
		const float xi = p_plane.polygon[i][0], zi = p_plane.polygon[i][1];
		const float xj = p_plane.polygon[j][0], zj = p_plane.polygon[j][1];
		if ((zi > p_z) != (zj > p_z) && p_x < (xj - xi) * (p_z - zi) / (zj - zi) + xi) {
			in = !in;
		}
	}
	return in;
}

Result<std::vector<std::array<int32_t, 2>>> combat_area_tiles(std::span<const CombatAreaDef> p_areas, float p_margin) {
	constexpr float kTile = 64.0f;
	std::set<std::array<int32_t, 2>> tiles;
	const float margin = std::isfinite(p_margin) ? std::clamp(p_margin, 0.0f, 1024.0f) : 0.0f;

	uint64_t work = 0;
	for (const CombatAreaDef &a : p_areas) {
		if (a.polygon.size() < 3) {
			continue;
		}
		float x0 = a.polygon[0][0], x1 = x0, z0 = a.polygon[0][1], z1 = z0;
		uint64_t samples = 0;
		for (size_t i = 0, j = a.polygon.size() - 1; i < a.polygon.size(); j = i++) {
			const std::array<float, 2> &p = a.polygon[i];
			if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || std::fabs(p[0]) > 1e6f || std::fabs(p[1]) > 1e6f) {
				return make_error("combat_area_tiles: a polygon point is not finite or beyond 10^6 m");
			}
			x0 = std::min(x0, p[0]);
			x1 = std::max(x1, p[0]);
			z0 = std::min(z0, p[1]);
			z1 = std::max(z1, p[1]);
			const float dx = p[0] - a.polygon[j][0], dz = p[1] - a.polygon[j][1];
			samples += 2 + uint64_t(std::sqrt(dx * dx + dz * dz) / 2.0f);
		}
		const uint64_t nx = uint64_t(std::floor((x1 + margin) / kTile) - std::floor((x0 - margin) / kTile)) + 1;
		const uint64_t nz = uint64_t(std::floor((z1 + margin) / kTile) - std::floor((z0 - margin) / kTile)) + 1;
		work += nx * nz * samples;
		if (work > kMaxCombatAreaTileWork) {
			return make_error("combat_area_tiles: tiles x edge samples over " + std::to_string(kMaxCombatAreaTileWork));
		}
	}
	for (const CombatAreaDef &a : p_areas) {
		if (a.polygon.size() < 3) {
			continue;
		}
		float x0 = a.polygon[0][0], x1 = x0, z0 = a.polygon[0][1], z1 = z0;
		for (const std::array<float, 2> &p : a.polygon) {
			x0 = std::min(x0, p[0]);
			x1 = std::max(x1, p[0]);
			z0 = std::min(z0, p[1]);
			z1 = std::max(z1, p[1]);
		}
		const int32_t tx0 = int32_t(std::floor((x0 - margin) / kTile));
		const int32_t tx1 = int32_t(std::floor((x1 + margin) / kTile));
		const int32_t tz0 = int32_t(std::floor((z0 - margin) / kTile));
		const int32_t tz1 = int32_t(std::floor((z1 + margin) / kTile));

		const auto inside = [&](float p_x, float p_z) {
			bool in = false;
			const size_t n = a.polygon.size();
			for (size_t i = 0, j = n - 1; i < n; j = i++) {
				const float xi = a.polygon[i][0], zi = a.polygon[i][1];
				const float xj = a.polygon[j][0], zj = a.polygon[j][1];
				if ((zi > p_z) != (zj > p_z) && p_x < (xj - xi) * (p_z - zi) / (zj - zi) + xi) {
					in = !in;
				}
			}
			return in;
		};
		const auto seg_box_distance2 = [](float ax, float az, float bx, float bz, float qx0, float qz0, float qx1, float qz1) {

			float best = 1e30f;
			const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
			const int steps = std::max(1, int(len / 2.0f));
			for (int s = 0; s <= steps; ++s) {
				const float t = float(s) / float(steps);
				const float px = ax + (bx - ax) * t, pz = az + (bz - az) * t;
				const float dx = std::max({ qx0 - px, 0.0f, px - qx1 });
				const float dz = std::max({ qz0 - pz, 0.0f, pz - qz1 });
				best = std::min(best, dx * dx + dz * dz);
			}
			return best;
		};
		if (int64_t(tx1 - tx0 + 1) * int64_t(tz1 - tz0 + 1) > (int64_t(1) << 20)) {
			continue;
		}
		for (int32_t tz = tz0; tz <= tz1; ++tz) {
			for (int32_t tx = tx0; tx <= tx1; ++tx) {
				const float qx0 = float(tx) * kTile, qz0 = float(tz) * kTile;
				const float qx1 = qx0 + kTile, qz1 = qz0 + kTile;
				bool touch = inside(qx0 + 0.5f * kTile, qz0 + 0.5f * kTile);
				for (size_t i = 0, j = a.polygon.size() - 1; !touch && i < a.polygon.size(); j = i++) {
					touch = seg_box_distance2(a.polygon[j][0], a.polygon[j][1], a.polygon[i][0], a.polygon[i][1], qx0, qz0, qx1, qz1) <= margin * margin;
				}
				if (touch) {
					tiles.insert({ tx, tz });
				}
			}
		}
	}
	return std::vector<std::array<int32_t, 2>>(tiles.begin(), tiles.end());
}

std::vector<std::array<int32_t, 2>> point_tiles(const Vec3 &p_point, float p_margin) {
	constexpr float kTile = 64.0f;
	std::vector<std::array<int32_t, 2>> out;
	if (!std::isfinite(p_point[0]) || !std::isfinite(p_point[2]) || std::fabs(p_point[0]) > 1e6f || std::fabs(p_point[2]) > 1e6f) {
		return out;
	}
	const float m = std::isfinite(p_margin) ? std::clamp(p_margin, 0.0f, 64.0f) : 0.0f;
	const int32_t tx0 = int32_t(std::floor((p_point[0] - m) / kTile)), tx1 = int32_t(std::floor((p_point[0] + m) / kTile));
	const int32_t tz0 = int32_t(std::floor((p_point[2] - m) / kTile)), tz1 = int32_t(std::floor((p_point[2] + m) / kTile));
	for (int32_t tz = tz0; tz <= tz1; ++tz) {
		for (int32_t tx = tx0; tx <= tx1; ++tx) {
			out.push_back({ tx, tz });
		}
	}
	return out;
}

Result<LevelCombatTiles> level_combat_tiles(std::shared_ptr<const Install> p_install, std::string_view p_level_id, float p_margin) {
	if (p_install == nullptr) {
		return make_error("level_combat_tiles: no install");
	}
	const auto start = std::chrono::steady_clock::now();
	Result<LevelInfo> info = level_info(*p_install, p_level_id);
	if (!info) {
		return error_at("level_combat_tiles " + std::string(p_level_id), info.error());
	}
	LevelCombatTiles out;
	std::set<std::array<int32_t, 2>> all;
	std::set<std::array<int32_t, 2>> point_set;
	for (const std::string &mode : info->modes) {
		Result<std::unique_ptr<LoadedLevel>> level = load_level(p_install, p_level_id, mode);
		if (!level) {
			return error_at("level_combat_tiles " + std::string(p_level_id) + " " + mode, level.error());
		}
		Result<std::unique_ptr<World>> world = World::traverse(**level);
		if (!world) {
			return error_at("level_combat_tiles " + std::string(p_level_id) + " " + mode + ": traverse", world.error());
		}
		Result<ModeAreas> areas = read_mode_areas(**world);
		if (!areas) {
			return error_at("level_combat_tiles " + std::string(p_level_id) + " " + mode, areas.error());
		}
		Result<std::vector<std::array<int32_t, 2>>> tiles = combat_area_tiles(areas->combat_areas, p_margin);
		if (!tiles) {
			return error_at("level_combat_tiles " + std::string(p_level_id) + " " + mode, tiles.error());
		}
		for (const std::array<int32_t, 2> &t : *tiles) {
			all.insert(t);
		}
		if (out.water.empty()) {
			Result<std::vector<WaterPlane>> water = read_water_planes(**world);
			if (!water) {
				return error_at("level_combat_tiles " + std::string(p_level_id) + " " + mode, water.error());
			}
			out.water = std::move(*water);
		}
		Result<LevelSpawns> spawns = read_level_spawns(**world);
		if (!spawns) {
			return error_at("level_combat_tiles " + std::string(p_level_id) + " " + mode + ": spawns", spawns.error());
		}
		std::vector<Vec3> points;
		for (const ModeObjectiveDef &o : areas->objectives) {
			points.push_back(o.position);
		}
		for (const SpawnPointDef &sp : spawns->points) {
			points.push_back(sp.transform.trans);
		}
		for (const Vec3 &pt : points) {
			if (!inside_any(areas->combat_areas, pt)) {
				++out.points_outside_areas;
			}
			for (const std::array<int32_t, 2> &t : point_tiles(pt, p_margin)) {
				point_set.insert(t);
			}
		}
		out.points += points.size();
		out.modes.push_back(mode);
		out.areas.push_back(std::move(*areas));
	}
	out.polygon_tiles = all.size();
	for (const std::array<int32_t, 2> &t : point_set) {
		out.point_only_tiles += all.insert(t).second ? 1 : 0;
	}
	out.tiles.assign(all.begin(), all.end());
	out.seconds = seconds_since(start);
	return out;
}

}

#include "bf3/sim/logic/conquest_data.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/logic/logic_native.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace bf3::sim::logic {

namespace gp = core::gameplay;
using T = gp::LogicTypeId;

namespace {

struct Reader {
	const gp::LogicWorld &world;
	const gp::LogicObject &object;

	std::optional<gp::PropertyValue> get(std::string_view p_field) const { return world.effective(object, p_field); }

	double number(std::string_view p_field, double p_default) const {
		const std::optional<gp::PropertyValue> v = get(p_field);
		const std::optional<double> d = v ? v->as_f64() : std::nullopt;
		return d && std::isfinite(*d) ? *d : p_default;
	}
	int64_t integer(std::string_view p_field, int64_t p_default) const {
		const std::optional<gp::PropertyValue> v = get(p_field);
		if (const std::optional<int64_t> i = v ? v->as_i64() : std::nullopt) {
			return *i;
		}
		const std::optional<double> d = v ? v->as_f64() : std::nullopt;
		return d && std::isfinite(*d) && std::fabs(*d) < 1e15 ? int64_t(*d) : p_default;
	}
	bool boolean(std::string_view p_field, bool p_default) const {
		const std::optional<gp::PropertyValue> v = get(p_field);
		const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
		return b.value_or(p_default);
	}
	uint8_t team(std::string_view p_field) const {
		const std::optional<gp::PropertyValue> v = get(p_field);
		if (!v) {
			return 0;
		}
		if (const std::optional<int> t = v->as_team(); t && *t >= 0 && size_t(*t) < kConquestTeams) {
			return uint8_t(*t);
		}
		const std::optional<int64_t> i = v->as_i64();
		return i && *i >= 0 && size_t(*i) < kConquestTeams ? uint8_t(*i) : 0;
	}
	std::string enumeration(std::string_view p_field) const {
		const std::optional<gp::PropertyValue> v = get(p_field);
		const std::optional<std::string_view> name = v ? v->enum_name() : std::nullopt;
		return name ? std::string(*name) : std::string();
	}
};

std::vector<core::Guid> graph_chain(const gp::LogicGraph &p_graph, uint32_t p_scope) {
	std::vector<core::Guid> out;
	for (uint32_t s = p_scope;
			s < p_graph.scopes().size() && p_graph.scopes()[s].parent != gp::kNoLogicIndex && out.size() < 64;
			s = p_graph.scopes()[s].parent) {
		out.push_back(p_graph.scopes()[s].reference);
	}
	return out;
}

std::vector<core::Guid> level_chain(const core::level::World &p_level, uint32_t p_scope) {
	std::vector<core::Guid> out;
	for (uint32_t s = p_scope; s < p_level.scopes.size() && p_level.scopes.get(s).parent && out.size() < 64;
			s = *p_level.scopes.get(s).parent) {
		const std::optional<core::EbxInstance> &r = p_level.scopes.get(s).reference;
		out.push_back(r ? r->guid() : core::Guid{});
	}
	return out;
}

std::optional<uint32_t> node_of(const gp::LogicGraph &p_graph, const core::Guid &p_guid, uint32_t p_scope, T p_type) {
	for (const uint32_t n : p_graph.nodes_with_guid(p_guid)) {
		if (p_graph.nodes()[n].scope == p_scope && p_graph.nodes()[n].type == p_type) {
			return n;
		}
	}
	return std::nullopt;
}

std::optional<uint32_t> node_of_placed(
		const gp::LogicGraph &p_graph, const core::level::World &p_level, uint32_t p_object, T p_type) {
	if (p_object >= p_level.objects.size()) {
		return std::nullopt;
	}
	const core::level::PlacedObject &placed = p_level.objects[p_object];
	const std::vector<core::Guid> chain = level_chain(p_level, placed.scope);
	for (const uint32_t n : p_graph.nodes_with_guid(placed.entity.guid())) {
		if (p_graph.nodes()[n].type == p_type && graph_chain(p_graph, p_graph.nodes()[n].scope) == chain) {
			return n;
		}
	}
	return std::nullopt;
}

CapturableType capturable(const std::string &p_name) {
	if (p_name == "CTUnableToChangeTeam") {
		return CapturableType::UnableToChangeTeam;
	}
	return p_name == "CTCaptureEnablable" ? CapturableType::CaptureEnablable : CapturableType::Default;
}

TicketDecrease decrease(const std::string &p_name) {
	if (p_name == "TDTOnKilled") {
		return TicketDecrease::OnKilled;
	}
	return p_name == "TDTNoDecreaseOnSoldierDeath" ? TicketDecrease::NoDecreaseOnSoldierDeath
												   : TicketDecrease::OnRespawned;
}

}

const ConquestSpawnerData *ConquestParams::spawner(uint32_t p_node) const {
	for (const ConquestSpawnerData &s : spawners) {
		if (s.node == p_node) {
			return &s;
		}
	}
	return nullptr;
}

core::Result<std::shared_ptr<const ConquestParams>> read_conquest_params(
		const gp::LogicWorld &p_world, const gp::LogicGraph &p_graph, const core::level::World *p_level) {
	auto out = std::make_shared<ConquestParams>();
	std::vector<std::pair<uint32_t, const gp::LogicObject *>> spawner_objects;
	for (const gp::LogicObject &ob : p_world.objects()) {
		const T type = gp::logic_type_of(ob.entity.type_name());
		if (type != T::CapturePoint && type != T::TicketCounter && type != T::CombatAreaTrigger &&
				type != T::CharacterSpawn && type != T::VehicleSpawn) {
			continue;
		}
		const std::optional<uint32_t> node = node_of(p_graph, ob.entity.guid(), ob.scope, type);
		if (!node) {
			continue;
		}
		if (out->points.size() + out->counters.size() + out->areas.size() + spawner_objects.size() >=
				kMaxConquestNodes) {
			return core::make_error("conquest: more than " + std::to_string(kMaxConquestNodes) + " conquest nodes");
		}
		const Reader r{ p_world, ob };
		if (type == T::CapturePoint) {
			ConquestPointData d;
			d.node = *node;
			CapturePointParams &p = d.params;
			p.initial_owner = r.team("InitialOwnerTeam");
			p.type = capturable(r.enumeration("CapturableType"));
			p.capture_radius = float(r.number("CaptureRadius", 0.0));
			p.upper_sphere = r.boolean("IsCapturedInUpperSphere", false);
			p.area_value = float(r.number("AreaValue", 0.0));
			if (r.boolean("UseAreaValuesPerTeam", false)) {
				out->notes.push_back("conquest: capture point node " + std::to_string(*node) +
						" sets UseAreaValuesPerTeam; AreaValues are not read (0)");
				p.area_values_per_team = true;
			}
			p.time_to_get_control = int32_t(std::clamp<int64_t>(r.integer("TimeToGetControl", 20), 1, 3600));
			p.time_to_lose_control = int32_t(std::clamp<int64_t>(r.integer("TimeToLoseControl", 20), 1, 3600));
			p.return_multiplier = float(r.number("ReturnMultiplier", 1.0));
			p.only_takeable_by_team = r.team("OnlyTakeableByTeam");
			p.min_nr_to_take_control = int32_t(std::clamp<int64_t>(r.integer("MinNrToTakeControl", 1), 0, 256));
			p.max_capture_multiplier = int32_t(std::clamp<int64_t>(r.integer("MaxCaptureMultiplier", 4), 0, 256));
			p.added_multiplier_per_player = float(r.number("AddedMultiplierPerPlayer", 0.0));
			p.seesaw = r.boolean("SeesawCapturing", false);
			p.start_at_bottom = r.boolean("StartAtBottom", true);
			p.lose_control_when_not_close = r.boolean("LoseControlWhenNotClose", false);
			p.disable_if_enemy_inside = r.boolean("DisableIfEnemyInside", false);
			p.intruder_warning = r.boolean("IntruderWarning", false);
			p.enabled = r.boolean("Enabled", true);
			p.spawn_menu_list_ordinal =
					int32_t(std::clamp<int64_t>(r.integer("SpawnMenuListOrdinal", 0), -1000000, 1000000));
			for (const gp::LogicEdge &e : p_graph.links_from(*node)) {
				const T target = p_graph.nodes()[e.target].type;
				if ((e.source_port == native_id::kCaptureCharacterSpawnLink && target == T::CharacterSpawn) ||
						(e.source_port == native_id::kCaptureVehicleSpawnLink && target == T::VehicleSpawn)) {
					if (std::find(d.spawners.begin(), d.spawners.end(), e.target) == d.spawners.end()) {
						d.spawners.push_back(e.target);
					}
				}
			}
			out->points.push_back(std::move(d));
		} else if (type == T::TicketCounter) {
			ConquestCounterData d;
			d.node = *node;
			TicketCounterParams &p = d.params;
			p.team = r.team("TeamId");
			p.initial_ticket_count = std::clamp<int64_t>(r.integer("InitialTicketCount", 0), 0, 1000000);
			p.decrease = decrease(r.enumeration("DecreaseTickets"));
			p.loss_per_min = float(r.number("TicketLossPerMin", 0.0));
			p.loss_start = float(r.number("TicketLossStart", 0.0));
			p.loss_when_lost_all = float(r.number("TicketLossWhenLostAll", 0.0));
			p.loss_when_lost_all_controls_point = float(r.number("TicketLossWhenLostAllControlsPoint", 0.0));
			p.halt_on_equal_point_count = r.boolean("HaltTicketLossOnEqualPointCount", false);
			p.reset_capture_points_on_reset = r.boolean("ResetCapturePointsOnReset", false);
			p.spawn_always_allowed = r.boolean("SpawnAlwaysAllowed", false);
			if (const std::optional<gp::PropertyValue> list = r.get("TicketPercentages");
					list && list->kind == gp::PropertyValue::Kind::Field) {
				if (const core::OptionalEbxArray array = list->field.as_array()) {
					for (const core::EbxValue element : *array) {
						if (p.percentages.size() >= 64) {
							break;
						}
						const std::optional<core::EbxValue> max = element.get("MaxPercentOfTicketsLeft");
						const std::optional<core::EbxValue> loss = element.get("TicketLossPerMin");
						TicketPercentage t;
						t.max_percent_of_tickets_left = max ? max->as_f32().value_or(0.0f) : 0.0f;
						t.ticket_loss_per_min = loss ? loss->as_f32().value_or(0.0f) : 0.0f;
						p.percentages.push_back(t);
					}
				}
			}
			out->counters.push_back(std::move(d));
		} else if (type == T::CombatAreaTrigger) {
			ConquestAreaData d;
			d.node = *node;
			d.params.team = r.team("Team");
			d.params.team_specific = r.boolean("IsTeamSpecific", false);
			d.params.ai_allowed_outside = r.boolean("IsAiAllowedOutside", true);
			d.params.time_to_return = int32_t(std::clamp<int64_t>(r.integer("TimeToReturn", 10), 0, 3600));
			out->areas.push_back(std::move(d));
		} else {
			spawner_objects.push_back({ *node, &ob });
		}
	}
	for (const auto &[node, ob] : spawner_objects) {
		bool linked = false;
		for (const ConquestPointData &p : out->points) {
			linked |= std::find(p.spawners.begin(), p.spawners.end(), node) != p.spawners.end();
		}
		if (!linked) {
			continue;
		}
		const Reader r{ p_world, *ob };
		ConquestSpawnerData s;
		s.node = node;
		s.vehicle = p_graph.nodes()[node].type == T::VehicleSpawn;
		s.gate.data_team = r.team("Team");
		s.gate.team = s.gate.data_team;
		s.gate.locked_team = r.boolean("LockedTeam", false);
		s.gate.enabled = r.boolean("Enabled", true);
		out->spawners.push_back(s);
	}
	if (p_level != nullptr) {
		core::Result<core::level::ModeAreas> areas = core::level::read_mode_areas(*p_level);
		if (!areas) {
			return core::error_at("conquest", areas.error());
		}
		for (const core::level::ModeObjectiveDef &o : areas->objectives) {
			if (o.kind == core::level::ModeObjectiveDef::Kind::Mcom) {
				continue;
			}
			const std::optional<uint32_t> node = node_of_placed(p_graph, *p_level, o.object, T::CapturePoint);
			for (ConquestPointData &p : out->points) {
				if (node && p.node == *node) {
					p.placed = true;
					p.params.position = o.position;
					if (!o.polygon.empty()) {
						p.params.volumes.push_back(CaptureVolume{ o.polygon });
					}
				}
			}
		}
		for (const core::level::CombatAreaDef &a : areas->combat_areas) {
			const std::optional<uint32_t> node = node_of_placed(p_graph, *p_level, a.trigger, T::CombatAreaTrigger);
			for (ConquestAreaData &d : out->areas) {
				if (node && d.node == *node && d.polygon.empty()) {
					d.polygon = a.polygon;
				}
			}
		}
		for (uint32_t i = 0; i < p_level->objects.size(); ++i) {
			if (p_level->objects[i].kind != core::level::ObjectKind::CharacterSpawnReference) {
				continue;
			}
			const std::optional<uint32_t> node = node_of_placed(p_graph, *p_level, i, T::CharacterSpawn);
			for (ConquestSpawnerData &s : out->spawners) {
				if (node && s.node == *node) {
					s.level_object = i;
				}
			}
		}
		for (const ConquestPointData &p : out->points) {
			if (!p.placed) {
				out->notes.push_back("conquest: capture point node " + std::to_string(p.node) +
						" has no placed object (no position, no volume)");
			}
		}
		for (const ConquestAreaData &d : out->areas) {
			if (d.polygon.empty()) {
				out->notes.push_back(
						"conquest: combat area node " + std::to_string(d.node) + " has no shape (it tracks nobody)");
			}
		}
	}
	std::sort(out->points.begin(), out->points.end(),
			[](const ConquestPointData &a, const ConquestPointData &b) { return a.node < b.node; });
	std::sort(out->counters.begin(), out->counters.end(),
			[](const ConquestCounterData &a, const ConquestCounterData &b) { return a.node < b.node; });
	std::sort(out->areas.begin(), out->areas.end(),
			[](const ConquestAreaData &a, const ConquestAreaData &b) { return a.node < b.node; });
	std::sort(out->spawners.begin(), out->spawners.end(),
			[](const ConquestSpawnerData &a, const ConquestSpawnerData &b) { return a.node < b.node; });
	return std::shared_ptr<const ConquestParams>(std::move(out));
}

}

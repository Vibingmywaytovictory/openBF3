#include "bf3/core/gameplay/vehicle_hud.h"

#include "bf3/core/gameplay/logic_world.h"
#include "gameplay_read.h"

#include <algorithm>
#include <memory>
#include <optional>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

struct Scope {
	const LogicConnections *connections = nullptr;
	std::optional<EbxInstance> reference;
	std::optional<EbxInstance> descriptor;
	std::string prefab;
};

struct Prefab {
	EbxInstance reference;
	EbxInstance descriptor;
	std::string name;
	LogicConnections connections;
};

class Walker {
public:
	Walker(const EbxResolver &p_db, WorkBudget &r_budget, const LogicConnections &p_vehicle) :
			db_(p_db), budget_(r_budget), vehicle_{ &p_vehicle, std::nullopt, std::nullopt, {} } {}

	Status walk(const EbxInstance &p_camera, int32_t p_output, const VehicleHudTerm &p_term) {
		return follow(vehicle_, p_camera, p_output, p_term, 0, -1);
	}

	VehicleHudDef take() {
		std::erase_if(
				out_.switches, [](const VehicleHudSwitch &p_s) { return p_s.on_true.empty() && p_s.on_false.empty(); });
		return std::move(out_);
	}

private:
	// SOURCE: notes R5-UI1 (camera outputs through Or and Or4 into a HUD prefab, BoolToEvent, then UIGraph events).
	// STAND-IN: a Delay on the path (helicopter, jet and MFD prefabs) is not followed. Settle: follow it with air vehicles.
	Status follow(const Scope &p_scope, const EbxInstance &p_source, int32_t p_field, const VehicleHudTerm &p_term,
			size_t p_depth, int32_t p_prefab_field) {
		if (p_depth > kMaxVehicleHudDepth) {
			return Status::success();
		}
		BF3_TRY(budget_.charge(p_scope.connections->property.size()));
		for (const LogicConnection &c : p_scope.connections->property) {
			if (!(c.source == p_source) || c.source_field != p_field) {
				continue;
			}
			if (c.target.is_a("OrEntityData") || c.target.is_a("Or4EntityData")) {
				BF3_TRY(follow(p_scope, c.target, field_id("Out"), p_term, p_depth + 1, p_prefab_field));
			} else if (c.target.is_a("BoolToEventEntityData") && c.target_field == field_id("Value")) {
				BF3_TRY(add_term(p_scope, c.target, p_term, p_prefab_field));
			} else if (!p_scope.reference && c.target.is_a("LogicReferenceObjectData")) {
				Result<const Prefab *> prefab = prefab_of(c.target);
				if (!prefab) {
					return prefab.error();
				}
				if (*prefab != nullptr) {
					const Scope inner{ &(*prefab)->connections, (*prefab)->reference, (*prefab)->descriptor,
						(*prefab)->name };
					BF3_TRY(follow(inner, (*prefab)->descriptor, c.target_field, p_term, p_depth + 1, c.target_field));
				}
			}
		}
		return Status::success();
	}

	Result<const Prefab *> prefab_of(const EbxInstance &p_reference) {
		for (const std::unique_ptr<Prefab> &p : prefabs_) {
			if (p->reference == p_reference) {
				return p.get();
			}
		}
		const std::optional<EbxInstance> blueprint = db_.follow(p_reference.data(), "Blueprint");
		if (!blueprint || !blueprint->is_a("LogicPrefabBlueprint")) {
			return static_cast<const Prefab *>(nullptr);
		}
		const std::optional<EbxInstance> descriptor = db_.follow(blueprint->data(), "Descriptor");
		if (!descriptor) {
			return static_cast<const Prefab *>(nullptr);
		}
		Result<LogicConnections> connections = LogicConnections::of(db_, *blueprint, budget_);
		if (!connections) {
			return connections.error();
		}
		prefabs_.push_back(std::make_unique<Prefab>(
				Prefab{ p_reference, *descriptor, partition_name(budget_, db_, *blueprint), std::move(*connections) }));
		return prefabs_.back().get();
	}

	Status add_term(
			const Scope &p_scope, const EbxInstance &p_switch, const VehicleHudTerm &p_term, int32_t p_prefab_field) {
		const auto key = std::make_pair(p_scope.reference, p_switch);
		size_t at = keys_.size();
		for (size_t i = 0; i < keys_.size(); ++i) {
			if (keys_[i] == key) {
				at = i;
			}
		}
		if (at == keys_.size()) {
			keys_.push_back(key);
			VehicleHudSwitch s;
			s.prefab = p_scope.prefab;
			s.prefab_field = p_prefab_field;
			s.initial = boolean(p_switch.data(), "Value").value_or(false);
			s.initial_event = boolean(p_switch.data(), "InitalEvent").value_or(false);
			BF3_TRY(graph_events(*p_scope.connections, p_switch, field_id("OnTrue"), s.on_true));
			BF3_TRY(graph_events(*p_scope.connections, p_switch, field_id("OnFalse"), s.on_false));
			out_.switches.push_back(std::move(s));
		}
		std::vector<VehicleHudTerm> &terms = out_.switches[at].terms;
		if (std::find(terms.begin(), terms.end(), p_term) == terms.end()) {
			terms.push_back(p_term);
		}
		return Status::success();
	}

	Status graph_events(const LogicConnections &p_connections, const EbxInstance &p_switch, int32_t p_port,
			std::vector<VehicleHudGraphEvent> &r_out) {
		BF3_TRY(budget_.charge(p_connections.event.size()));
		for (const LogicConnection &e : p_connections.event) {
			if (!(e.source == p_switch) || e.source_field != p_port || !e.target.is_a("UIGraphEntityData")) {
				continue;
			}
			const std::optional<EbxInstance> asset = db_.follow(e.target.data(), "GraphAsset");
			if (!asset) {
				continue;
			}
			VehicleHudGraphEvent g;
			g.graph = partition_name(budget_, db_, *asset);
			g.priority = text(budget_, e.target.data(), "GraphPriority");
			g.pop_previous = boolean(e.target.data(), "PopPreviousGraph").value_or(false);
			g.event = e.target_field;
			r_out.push_back(std::move(g));
		}
		return Status::success();
	}

	const EbxResolver &db_;
	WorkBudget &budget_;
	Scope vehicle_;
	std::vector<std::unique_ptr<Prefab>> prefabs_;
	std::vector<std::pair<std::optional<EbxInstance>, EbxInstance>> keys_;
	VehicleHudDef out_;
};

}

Result<VehicleHudDef> read_vehicle_hud(const EbxResolver &p_db, const EbxInstance &p_blueprint,
		std::span<const EbxInstance> p_cameras, WorkBudget &r_budget) {
	Result<LogicConnections> vehicle = LogicConnections::of(p_db, p_blueprint, r_budget);
	if (!vehicle) {
		return vehicle.error();
	}
	Walker walker(p_db, r_budget, *vehicle);
	for (size_t i = 0; i < p_cameras.size(); ++i) {
		for (const bool alternate : { false, true }) {
			const VehicleHudTerm term{ static_cast<uint32_t>(i), alternate };
			BF3_TRY(walker.walk(p_cameras[i], alternate ? kCameraAlternateViewOutput : kCameraActiveOutput, term));
		}
	}
	BF3_TRY(r_budget.status());
	return walker.take();
}

VehicleHudCameraFlags read_vehicle_hud_flags(const EbxResolver &p_db, const EbxStruct &p_hud_data) {
	VehicleHudCameraFlags out;
	out.use_weapon_orientations = boolean(p_hud_data, "UseWeaponOrientations").value_or(false);
	out.use_render_target = boolean(p_hud_data, "UseRenderTarget").value_or(false);
	out.use_aim_warning = boolean(p_hud_data, "UseAimWarning").value_or(false);
	const std::optional<EbxInstance> list = p_db.follow(p_hud_data, "HudPropertyList");
	if (!list) {
		return out;
	}
	const std::optional<EbxValue> entries = list->get("HudPropertyList");
	const OptionalEbxArray array = entries ? entries->as_array() : OptionalEbxArray();
	if (!array) {
		return out;
	}
	for (const EbxValue entry : *array) {
		const std::optional<EbxStruct> s = entry.as_struct();
		if (s && text_view(*s, "Identifier") == "HCI_RangeMeter") {
			out.range_meter = true;
		}
	}
	return out;
}

}

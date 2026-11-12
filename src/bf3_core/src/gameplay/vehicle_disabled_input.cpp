#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/gameplay/vehicle_def.h"
#include "gameplay_read.h"

#include <algorithm>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

bool entry_linked(const LogicConnections &p_connections, const EbxInstance &p_target, std::optional<int32_t> p_field) {
	return std::any_of(p_connections.link.begin(), p_connections.link.end(), [&](const LogicConnection &l) {
		return l.target == p_target && (!p_field || l.target_field == *p_field) &&
				l.source.is_a("PlayerEntryComponentData");
	});
}

void add_modifier(WorkBudget &r_budget, const EbxInstance &p_modifier, std::vector<EbxInstance> &r_seen,
		std::vector<VehicleInputModifierDef> &r_out) {
	if (std::find(r_seen.begin(), r_seen.end(), p_modifier) != r_seen.end()) {
		return;
	}
	r_seen.push_back(p_modifier);
	const EbxStruct m = p_modifier.data();
	VehicleInputModifierDef d;
	d.action = text(r_budget, m, "Action");
	d.scale = static_cast<float>(number_or(m, "Scale", 1));
	d.offset = static_cast<float>(number_or(m, "Offset", 0));
	r_out.push_back(std::move(d));
}

}

Result<std::vector<VehicleInputModifierDef>> read_disabled_input(
		const EbxResolver &p_db, const EbxInstance &p_blueprint, const EbxInstance &p_entity, WorkBudget &r_budget) {
	std::vector<VehicleInputModifierDef> out;
	std::vector<EbxInstance> seen;
	Result<LogicConnections> vehicle = LogicConnections::of(p_db, p_blueprint, r_budget);
	if (!vehicle) {
		return vehicle.error();
	}
	const int32_t on_disabled = field_id("OnDisabledDamage");
	const int32_t enabled = field_id("Enabled");
	const int32_t switched = field_id("Out");
	for (const LogicConnection &e : vehicle->event) {
		if (!(e.source == p_entity) || e.source_field != on_disabled || !e.target.is_a("LogicReferenceObjectData")) {
			continue;
		}
		const std::optional<EbxInstance> prefab = p_db.follow(e.target.data(), "Blueprint");
		const std::optional<EbxInstance> descriptor = prefab && prefab->is_a("LogicPrefabBlueprint")
				? p_db.follow(prefab->data(), "Descriptor")
				: std::nullopt;
		if (!descriptor) {
			continue;
		}
		Result<LogicConnections> inner = LogicConnections::of(p_db, *prefab, r_budget);
		if (!inner) {
			return inner.error();
		}
		for (const LogicConnection &in : inner->event) {
			if (!(in.source == *descriptor) || in.source_field != e.target_field) {
				continue;
			}
			for (const LogicConnection &p : inner->property) {
				if (!(p.source == in.target) || p.source_field != switched) {
					continue;
				}
				if (p.target.is_a("InputModifierEntityData") && p.target_field == enabled) {
					for (const LogicConnection &l : inner->link) {
						if (l.source == *descriptor && l.target == p.target &&
								entry_linked(*vehicle, e.target, l.source_field)) {
							add_modifier(r_budget, p.target, seen, out);
						}
					}
				} else if (p.target == *descriptor) {
					for (const LogicConnection &v : vehicle->property) {
						if (v.source == e.target && v.source_field == p.target_field &&
								v.target.is_a("InputModifierEntityData") && v.target_field == enabled &&
								entry_linked(*vehicle, v.target, std::nullopt)) {
							add_modifier(r_budget, v.target, seen, out);
						}
					}
				}
			}
		}
	}
	BF3_TRY(r_budget.status());
	return out;
}

float apply_input_modifiers(
		std::span<const VehicleInputModifierDef> p_modifiers, std::string_view p_action, float p_level) {
	float level = p_level;
	for (const VehicleInputModifierDef &m : p_modifiers) {
		if (m.action == p_action) {
			level = std::clamp(level * m.scale + m.offset, -1.0f, 1.0f);
		}
	}
	return level;
}

}

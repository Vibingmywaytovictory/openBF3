#include "bf3/core/gameplay/vehicle_damage_visuals.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/util/splitmix64.h"
#include "gameplay_read.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr size_t kMaxFloatHops = 8;
constexpr uint32_t kWholeObjectPart = 0xffffffffu;
constexpr int32_t kStart = logic_port_id("Start");
constexpr int32_t kStop = logic_port_id("Stop");
constexpr int32_t kFloatIn0 = logic_port_id("FloatIn0");
constexpr int32_t kFloatIn1 = logic_port_id("FloatIn1");
constexpr int32_t kHealthNormalized = logic_port_id("CurrentHealthNormalized");
constexpr int32_t kOnEnabled = logic_port_id("OnEnabled");
constexpr int32_t kOnSuccess = logic_port_id("OnSuccess");
constexpr int32_t kOnFail = logic_port_id("OnFail");

struct Framed {
	EbxInstance component;
	level::LevelTransform frame;
};

void frames(WorkBudget &r_budget, const EbxResolver &p_db, const EbxInstance &p_container,
		const level::LevelTransform &p_parent, size_t p_depth, std::vector<Framed> &r_out) {
	if (p_depth > kMaxVehicleComponentDepth) {
		return;
	}
	for (const EbxInstance &c : follow_all(r_budget, p_db, p_container.data(), "Components")) {
		if (r_out.size() >= kMaxVehicleComponents || !active_component(c)) {
			continue;
		}
		const Result<level::LevelTransform> local = component_transform(c);
		const level::LevelTransform here = (local ? *local : level::LevelTransform::identity()).in_parent(p_parent);
		r_out.push_back({ c, here });
		frames(r_budget, p_db, c, here, p_depth + 1, r_out);
	}
}

core::Transform model_transform(const level::LevelTransform &p_t) {
	return { p_t.right, p_t.up, p_t.forward, p_t.trans };
}

const Framed *framed(const std::vector<Framed> &p_all, const EbxInstance &p_component) {
	for (const Framed &f : p_all) {
		if (f.component == p_component) {
			return &f;
		}
	}
	return nullptr;
}

std::optional<HealthCompare> compare_of(std::string_view p_operator) {
	static constexpr std::pair<std::string_view, HealthCompare> kOps[] = {
		{ "CompareOp_Equal", HealthCompare::Equal },
		{ "CompareOp_NotEqual", HealthCompare::NotEqual },
		{ "CompareOp_Less", HealthCompare::Less },
		{ "CompareOp_LessOrEqual", HealthCompare::LessOrEqual },
		{ "CompareOp_Greater", HealthCompare::Greater },
		{ "CompareOp_GreaterOrEqual", HealthCompare::GreaterOrEqual },
	};
	for (const auto &[name, op] : kOps) {
		if (name == p_operator) {
			return op;
		}
	}
	return std::nullopt;
}

HealthCompare negated(HealthCompare p_op) {
	switch (p_op) {
		case HealthCompare::Equal:
			return HealthCompare::NotEqual;
		case HealthCompare::NotEqual:
			return HealthCompare::Equal;
		case HealthCompare::Less:
			return HealthCompare::GreaterOrEqual;
		case HealthCompare::LessOrEqual:
			return HealthCompare::Greater;
		case HealthCompare::Greater:
			return HealthCompare::LessOrEqual;
		case HealthCompare::GreaterOrEqual:
			return HealthCompare::Less;
	}
	return p_op;
}

std::optional<double> interface_default(WorkBudget &r_budget, const EbxInstance &p_descriptor, int32_t p_field) {
	const OptionalEbxArray fields = array_of(r_budget, p_descriptor.data(), "Fields");
	if (!fields) {
		return std::nullopt;
	}
	for (const EbxValue f : *fields) {
		const std::optional<EbxStruct> s = f.as_struct();
		if (!s || number(*s, "Id") != std::optional<double>(p_field)) {
			continue;
		}
		const std::optional<PropertyValue> v = PropertyValue::parse(text_view(*s, "Value"));
		return v ? v->as_f64() : std::nullopt;
	}
	return std::nullopt;
}

class Prefab {
public:
	Prefab(WorkBudget &r_budget, const LogicConnections &p_outer, const EbxInstance &p_reference,
			const EbxInstance &p_descriptor, LogicConnections p_inner) :
			budget_(r_budget), outer_(p_outer), reference_(p_reference), descriptor_(p_descriptor),
			inner_(std::move(p_inner)) {}

	const LogicConnection *into(const LogicConnections &p_set, const EbxInstance &p_target, int32_t p_field) const {
		for (const LogicConnection &c : p_set.property) {
			if (c.target == p_target && c.target_field == p_field) {
				return &c;
			}
		}
		return nullptr;
	}

	bool takes_health() const {
		return std::any_of(outer_.property.begin(), outer_.property.end(), [&](const LogicConnection &c) {
			return c.target == reference_ && c.source.is_a("VehicleEntityData") && c.source_field == kHealthNormalized;
		});
	}

	bool health_input(const EbxInstance &p_compare) const {
		const LogicConnection *in = into(inner_, p_compare, kFloatIn0);
		if (in == nullptr || !(in->source == descriptor_)) {
			return false;
		}
		const LogicConnection *out = into(outer_, reference_, in->source_field);
		return out != nullptr && out->source.is_a("VehicleEntityData") && out->source_field == kHealthNormalized;
	}

	std::optional<double> value(const EbxInstance &p_target, int32_t p_field, size_t p_hops) const {
		if (p_hops > kMaxFloatHops) {
			return std::nullopt;
		}
		const LogicConnection *in = into(inner_, p_target, p_field);
		if (in == nullptr) {
			return std::nullopt;
		}
		if (in->source.is_a("FloatEntityData") && in->source_field == logic_port::kValue) {
			return number(in->source.data(), "DefaultValue");
		}
		if (in->source.is_a("FloatHubEntityData") && in->source_field == logic_port::kOut) {
			const std::optional<double> select = number(in->source.data(), "InputSelect");
			if (!select || *select < 0) {
				return std::nullopt;
			}
			return value(in->source, logic_port_id("In" + std::to_string(static_cast<int>(*select))), p_hops + 1);
		}
		if (in->source == descriptor_) {
			if (const LogicConnection *out = into(outer_, reference_, in->source_field)) {
				if (out->source.is_a("InterfaceDescriptorData")) {
					return interface_default(budget_, out->source, out->source_field);
				}
				if (out->source.is_a("FloatEntityData") && out->source_field == logic_port::kValue) {
					return number(out->source.data(), "DefaultValue");
				}
				return std::nullopt;
			}
			return interface_default(budget_, descriptor_, in->source_field);
		}
		return std::nullopt;
	}

	std::optional<std::pair<HealthCompare, float>> condition(int32_t p_output) const {
		for (const LogicConnection &e : inner_.event) {
			if (!(e.target == descriptor_) || e.target_field != p_output || !e.source.is_a("BoolToEventEntityData")) {
				continue;
			}
			const bool on_true = e.source_field == logic_port::kOnTrue;
			if (!on_true && e.source_field != logic_port::kOnFalse) {
				continue;
			}
			const LogicConnection *b = into(inner_, e.source, logic_port::kValue);
			if (b == nullptr || !b->source.is_a("CompareEntityData") || b->source_field != logic_port::kOut ||
					!health_input(b->source)) {
				continue;
			}
			const std::optional<HealthCompare> op = compare_of(text_view(b->source.data(), "Operator"));
			std::optional<double> threshold = value(b->source, kFloatIn1, 0);
			if (!threshold && into(inner_, b->source, kFloatIn1) == nullptr) {
				threshold = number(b->source.data(), "FloatIn1");
			}
			if (!op || !threshold || !std::isfinite(*threshold)) {
				continue;
			}
			return std::make_pair(on_true ? *op : negated(*op), static_cast<float>(*threshold));
		}
		return std::nullopt;
	}

private:
	WorkBudget &budget_;
	const LogicConnections &outer_;
	EbxInstance reference_;
	EbxInstance descriptor_;
	LogicConnections inner_;
};

Status read_damage(const EbxResolver &p_db, const LogicConnections &p_vehicle, const std::vector<Framed> &p_frames,
		WorkBudget &r_budget, VehicleDamageVisualsDef &r_out) {
	std::vector<std::optional<Prefab>> prefabs;
	std::vector<EbxInstance> references;
	for (const LogicConnection &e : p_vehicle.event) {
		if (!e.source.is_a("LogicReferenceObjectData") || !e.target.is_a("EffectComponentData") ||
				e.target_field != kStart) {
			continue;
		}
		size_t at = 0;
		while (at < references.size() && !(references[at] == e.source)) {
			++at;
		}
		if (at == references.size()) {
			references.push_back(e.source);
			const std::optional<EbxInstance> prefab = p_db.follow(e.source.data(), "Blueprint");
			const std::optional<EbxInstance> descriptor = prefab && prefab->is_a("LogicPrefabBlueprint")
					? p_db.follow(prefab->data(), "Descriptor")
					: std::nullopt;
			if (!descriptor) {
				prefabs.emplace_back();
			} else {
				Result<LogicConnections> inner = LogicConnections::of(p_db, *prefab, r_budget);
				if (!inner) {
					return inner.error();
				}
				prefabs.emplace_back(std::in_place, r_budget, p_vehicle, e.source, *descriptor, std::move(*inner));
			}
		}
		if (!prefabs[at] || !prefabs[at]->takes_health()) {
			continue;
		}
		const std::optional<std::pair<HealthCompare, float>> c = prefabs[at]->condition(e.source_field);
		const Framed *where = framed(p_frames, e.target);
		const std::optional<EbxInstance> effect = p_db.follow(e.target.data(), "Effect");
		if (!c || where == nullptr || !effect) {
			++r_out.unresolved;
			continue;
		}
		VehicleDamageEffectDef d;
		d.effect = partition_name(r_budget, p_db, *effect);
		d.transform = model_transform(where->frame);
		d.compare = c->first;
		d.threshold = c->second;
		r_out.damage.push_back(std::move(d));
	}
	return r_budget.status();
}

std::optional<EbxInstance> wreck_state(
		WorkBudget &r_budget, const EbxResolver &p_db, const std::vector<Framed> &p_frames, const Framed **r_at) {
	for (const Framed &f : p_frames) {
		if (!f.component.get("VehicleConfig")) {
			continue;
		}
		for (const EbxInstance &s : follow_all(r_budget, p_db, f.component.data(), "HealthStates")) {
			const std::optional<double> part = number(s.data(), "PartIndex");
			if (s.is_a("HealthStateData") && part == std::optional<double>(kWholeObjectPart) &&
					number_or(s.data(), "Health", 1) <= 0 && p_db.follow(s.data(), "SpawnedBangerBlueprint")) {
				*r_at = &f;
				return s;
			}
		}
	}
	return std::nullopt;
}

Status read_wreck(const EbxResolver &p_db, const std::vector<Framed> &p_frames, WorkBudget &r_budget,
		VehicleDamageVisualsDef &r_out) {
	const Framed *chassis = nullptr;
	const std::optional<EbxInstance> state = wreck_state(r_budget, p_db, p_frames, &chassis);
	if (!state) {
		return r_budget.status();
	}
	const std::optional<EbxInstance> blueprint = p_db.follow(state->data(), "SpawnedBangerBlueprint");
	const std::optional<EbxInstance> banger = blueprint ? p_db.follow(blueprint->data(), "Object") : std::nullopt;
	if (!banger || !banger->is_a("BangerEntityData") || !r_budget.take_type(banger->data())) {
		++r_out.unresolved;
		return r_budget.status();
	}
	VehicleWreckDef w;
	w.blueprint = partition_name(r_budget, p_db, *blueprint);
	w.mesh = ref_name(r_budget, p_db, banger->data(), "Mesh");
	w.time_to_live = static_cast<float>(number_or(banger->data(), "TimeToLive", 0));
	const Result<level::LevelTransform> placed = component_transform(*banger);
	w.transform = model_transform((placed ? *placed : level::LevelTransform::identity()).in_parent(chassis->frame));
	std::vector<Framed> parts;
	frames(r_budget, p_db, *banger, level::LevelTransform::identity(), 0, parts);
	Result<LogicConnections> logic = LogicConnections::of(p_db, *blueprint, r_budget);
	if (!logic) {
		return logic.error();
	}
	std::vector<EbxInstance> gate_of;
	std::vector<std::pair<EbxInstance, int32_t>> frontier{ { *banger, kOnEnabled } };
	std::vector<int> frontier_gate{ -1 };
	for (size_t i = 0; i < frontier.size() && i < kMaxVehicleComponents; ++i) {
		const auto [source, port] = frontier[i];
		const int from = frontier_gate[i];
		for (const LogicConnection &e : logic->event) {
			if (!(e.source == source) || e.source_field != port) {
				continue;
			}
			const bool success = port != kOnFail;
			if (e.target.is_a("EffectComponentData") && e.target_field == kStart) {
				const Framed *where = framed(parts, e.target);
				const std::optional<EbxInstance> effect = p_db.follow(e.target.data(), "Effect");
				if (where == nullptr || !effect) {
					++r_out.unresolved;
					continue;
				}
				w.effects.push_back(
						{ partition_name(r_budget, p_db, *effect), model_transform(where->frame), from, success });
			} else if (e.target.is_a("RandomEventEntityData") && e.target_field == logic_port::kIn) {
				if (std::find(gate_of.begin(), gate_of.end(), e.target) != gate_of.end()) {
					continue;
				}
				gate_of.push_back(e.target);
				w.gates.push_back({ static_cast<float>(number_or(e.target.data(), "Probability", 0)), from, success });
				const int gate = static_cast<int>(w.gates.size() - 1);
				frontier.push_back({ e.target, kOnSuccess });
				frontier_gate.push_back(gate);
				frontier.push_back({ e.target, kOnFail });
				frontier_gate.push_back(gate);
			} else {
				++r_out.unresolved;
			}
		}
	}
	r_out.wreck = std::move(w);
	return r_budget.status();
}

}

bool health_compare(HealthCompare p_op, float p_health, float p_threshold) {
	switch (p_op) {
		case HealthCompare::Equal:
			return p_health == p_threshold;
		case HealthCompare::NotEqual:
			return p_health != p_threshold;
		case HealthCompare::Less:
			return p_health < p_threshold;
		case HealthCompare::LessOrEqual:
			return p_health <= p_threshold;
		case HealthCompare::Greater:
			return p_health > p_threshold;
		case HealthCompare::GreaterOrEqual:
			return p_health >= p_threshold;
	}
	return false;
}

Result<VehicleDamageVisualsDef> read_vehicle_damage_visuals(
		const EbxResolver &p_db, const EbxInstance &p_blueprint, const EbxInstance &p_entity, WorkBudget &r_budget) {
	VehicleDamageVisualsDef out;
	std::vector<Framed> all;
	frames(r_budget, p_db, p_entity, level::LevelTransform::identity(), 0, all);
	Result<LogicConnections> vehicle = LogicConnections::of(p_db, p_blueprint, r_budget);
	if (!vehicle) {
		return vehicle.error();
	}
	BF3_TRY(read_damage(p_db, *vehicle, all, r_budget, out));
	BF3_TRY(read_wreck(p_db, all, r_budget, out));
	return out;
}

std::vector<bool> roll_wreck_effects(const VehicleWreckDef &p_wreck, uint64_t p_seed) {
	uint64_t state = p_seed;
	std::vector<int8_t> gate(p_wreck.gates.size(), -1);
	for (size_t g = 0; g < p_wreck.gates.size(); ++g) {
		const VehicleWreckGateDef &d = p_wreck.gates[g];
		const bool reached = d.parent < 0 ||
				(size_t(d.parent) < g && gate[size_t(d.parent)] >= 0 && (gate[size_t(d.parent)] == 1) == d.on_success);
		if (reached) {
			gate[g] = static_cast<int8_t>(static_cast<float>(splitmix64_next(state) % 100u) < d.probability ? 1 : 0);
		}
	}
	std::vector<bool> out(p_wreck.effects.size(), false);
	for (size_t e = 0; e < p_wreck.effects.size(); ++e) {
		const VehicleWreckEffectDef &d = p_wreck.effects[e];
		out[e] = d.gate < 0 ||
				(size_t(d.gate) < gate.size() && gate[size_t(d.gate)] >= 0 &&
						(gate[size_t(d.gate)] == 1) == d.on_success);
	}
	return out;
}

}

#include "bf3/core/gameplay/soldier_sound.h"

#include "bf3/core/gameplay/logic_world.h"
#include "ebx_fields.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace bf3::core::gameplay {

using detail::Fields;

namespace {

constexpr int32_t kOut = logic_port_id("Out");
constexpr int32_t kValue = logic_port_id("Value");
constexpr int32_t kIn0 = logic_port_id("In0");
constexpr int32_t kIn1 = logic_port_id("In1");
constexpr int32_t kAGreaterB = logic_port_id("A>B");
constexpr int32_t kALessOrEqualB = logic_port_id("A<=B");
constexpr int32_t kHasLocalPlayer = logic_port_id("HasLocalPlayer");
constexpr int32_t kOnFootPlantLeft = logic_port_id("OnFootPlantLeft");
constexpr int32_t kOnFootPlantRight = logic_port_id("OnFootPlantRight");
constexpr int64_t kMathMultiply = 2;

struct Reader {
	Fields &f;
	const LogicConnections &links;

	const LogicConnection *property_into(const EbxInstance &p_target, int32_t p_field) const {
		const std::span<const uint32_t> at = links.property_into(p_target, p_field);
		return at.empty() ? nullptr : &links.property[at.front()];
	}

	std::optional<float> compare_threshold(const EbxInstance &p_sound, int32_t p_event, int32_t p_compare_event) {
		for (const LogicConnection &l : links.event) {
			if (!(l.target == p_sound) || l.target_field != p_event) {
				continue;
			}
			if (!l.source.is_a("CompareFloatEntityData") || l.source_field != p_compare_event) {
				f.fail(std::string(kMpSoldier) + ": the footsteps event " + std::to_string(p_event) + " comes from a " +
						std::string(l.source.type_name()));
				return std::nullopt;
			}
			const LogicConnection *a = property_into(l.source, logic_port_id("A"));
			if (a == nullptr || a->source_field != soldier_sound_port::kCurrentVelocityNormalized) {
				f.fail(std::string(kMpSoldier) +
						": the footsteps start and stop compare something other than CurrentVelocityNormalized");
				return std::nullopt;
			}
			return f.f32(l.source.data(), "B");
		}
		f.fail(std::string(kMpSoldier) + ": no event connection into the footsteps event " + std::to_string(p_event));
		return std::nullopt;
	}

	bool closed_by_local_player(const EbxInstance &p_gate) const {
		for (const LogicConnection &g : links.event) {
			if (!(g.target == p_gate) || g.target_field != logic_port::kClose ||
					!g.source.is_a("BoolToEventEntityData") || g.source_field != logic_port::kOnTrue) {
				continue;
			}
			const LogicConnection *value = property_into(g.source, kValue);
			if (value != nullptr && value->source.is_a("SoldierEntityData") && value->source_field == kHasLocalPlayer) {
				return true;
			}
		}
		return false;
	}

	bool period_gated(const EbxInstance &p_sound, const EbxInstance &p_component, int32_t p_plant,
			int32_t p_component_event) const {
		for (const LogicConnection &l : links.event) {
			if (!(l.target == p_sound) || l.target_field != p_plant || !l.source.is_a("EventGateEntityData") ||
					l.source_field != kOut) {
				continue;
			}
			const bool fed = std::any_of(links.event.begin(), links.event.end(), [&](const LogicConnection &g) {
				return g.target == l.source && g.target_field == logic_port::kIn && g.source == p_component &&
						g.source_field == p_component_event;
			});
			return fed && closed_by_local_player(l.source);
		}
		return false;
	}

	std::string sound_of(const EbxStruct &p_s, std::string_view p_field) {
		const std::optional<EbxRef> r = f.ref(p_s, p_field);
		if (!r || r->is_null()) {
			return {};
		}
		return f.target_partition(p_s.partition(), *r);
	}
};

}

Result<SoldierSoundDef> read_soldier_sounds(const EbxResolver &p_db, std::string_view p_soldier) {
	Fields f(p_db);
	const EbxPartition *part = p_db.partition_by_name(p_soldier);
	if (part == nullptr) {
		return make_error(std::string(p_soldier) + ": partition not found");
	}
	const std::vector<EbxInstance> components = part->instances_of("SoldierSoundComponentData");
	const std::vector<EbxInstance> sounds = part->instances_of("SoldierSoundData");
	if (components.size() != 1 || sounds.size() != 1) {
		return make_error(std::string(p_soldier) + ": " + std::to_string(components.size()) +
				" SoldierSoundComponentData and " + std::to_string(sounds.size()) +
				" SoldierSoundData, expected one each");
	}
	WorkBudget budget;
	Result<LogicConnections> wiring = LogicConnections::of(p_db, part->primary(), budget);
	if (!wiring) {
		return wiring.error();
	}
	if (wiring->property.empty() || wiring->event.empty()) {
		return make_error(std::string(p_soldier) + ": no connections");
	}
	Reader r{ f, *wiring };
	SoldierSoundDef out;
	const EbxStruct c = components.front().data();
	out.walk_velocity_threshold = f.f32(c, "WalkVelocityThreshold");
	out.run_velocity_threshold = f.f32(c, "RunVelocityThreshold");
	out.walk_period = f.f32(c, "WalkPeriod");
	out.run_period = f.f32(c, "RunPeriod");
	const EbxStruct s = sounds.front().data();
	out.movement = r.sound_of(s, "Movement");
	out.death = r.sound_of(s, "Death");
	out.bullet_impact = r.sound_of(s, "BulletImpact");
	out.breath_control = r.sound_of(s, "BreathControl");
	if (!f.ok()) {
		return f.error();
	}

	std::optional<EbxInstance> footsteps;
	for (const LogicConnection &l : wiring->property) {
		if (!(l.source == components.front()) || l.source_field != soldier_sound_port::kSoundMaterialId ||
				!l.target.is_a("SoundEntityData")) {
			continue;
		}
		const LogicConnection *pose = r.property_into(l.target, soldier_sound_port::kPose);
		if (pose == nullptr || !(pose->source == components.front())) {
			continue;
		}
		if (footsteps && !(*footsteps == l.target)) {
			return make_error(std::string(p_soldier) + ": SoundMaterialId and Pose feed two sound entities");
		}
		footsteps = l.target;
	}
	if (!footsteps) {
		return make_error(std::string(p_soldier) +
				": no sound entity takes the SoldierSoundComponentData's SoundMaterialId and Pose");
	}
	out.footsteps = r.sound_of(footsteps->data(), "Sound");

	const std::optional<float> start = r.compare_threshold(*footsteps, soldier_sound_port::kOnStart, kAGreaterB);
	const std::optional<float> stop = r.compare_threshold(*footsteps, soldier_sound_port::kOnStop, kALessOrEqualB);
	if (!start || !stop) {
		return f.error();
	}
	out.start_above = *start;
	out.stop_at_or_below = *stop;
	out.local_player_closes_period =
			r.period_gated(*footsteps, components.front(), soldier_sound_port::kFootPlantLeft, kOnFootPlantLeft) &&
			r.period_gated(*footsteps, components.front(), soldier_sound_port::kFootPlantRight, kOnFootPlantRight);

	for (const EbxInstance &ant : part->instances_of("AntEventEntityData")) {
		const OptionalEbxArray enters = f.array(ant.data(), "OnEnterEvents");
		if (!enters) {
			return f.error();
		}
		for (size_t i = 0; i < enters->size(); ++i) {
			const std::optional<EbxValue> v = enters->get(i);
			const std::optional<EbxStruct> entry = v ? v->as_struct() : std::nullopt;
			const std::optional<EbxStruct> spec = entry ? f.sub(*entry, "Event") : std::nullopt;
			if (!spec) {
				return f.ok() ? make_error(std::string(p_soldier) +
										": an AntEventEntityData OnEnterEvents entry has no Event")
							  : f.error();
			}
			const int32_t event = f.i32(*spec, "Id");
			const int32_t tag = f.i32(*entry, "TagId");
			for (const LogicConnection &l : wiring->event) {
				if (!(l.source == ant) || l.source_field != event || !(l.target == *footsteps)) {
					continue;
				}
				if (out.ant_steps.size() >= kMaxSoldierAntSteps) {
					return make_error(std::string(p_soldier) + ": more than " + std::to_string(kMaxSoldierAntSteps) +
							" animation events reach the footsteps sound");
				}
				out.ant_steps.push_back(SoldierAntStep{ tag, uint32_t(l.target_field) });
			}
		}
	}
	if (!f.ok()) {
		return f.error();
	}

	for (const LogicConnection &l : wiring->property) {
		if (!(l.target == *footsteps) || !l.source.is_a("MathOpEntityData") || l.source_field != kOut) {
			continue;
		}
		const OptionalEbxArray ops = f.array(l.source.data(), "Operators");
		const LogicConnection *velocity = r.property_into(l.source, kIn0);
		const LogicConnection *scale = r.property_into(l.source, kIn1);
		if (!ops || ops->size() != 1 || ops->get(0)->as_i64().value_or(-1) != kMathMultiply || velocity == nullptr ||
				scale == nullptr || velocity->source_field != soldier_sound_port::kCurrentVelocityNormalized ||
				!scale->source.is_a("FloatEntityData") || scale->source_field != kValue) {
			return f.ok()
					? make_error(std::string(p_soldier) +
							  ": the footsteps speed input is not CurrentVelocityNormalized times a FloatEntityData")
					: f.error();
		}
		out.speed_input = uint32_t(l.target_field);
		out.speed_scale = f.f32(scale->source.data(), "DefaultValue");
	}
	if (!f.ok()) {
		return f.error();
	}
	if (out.speed_input == 0 || !(out.speed_scale > 0.0f)) {
		return make_error(std::string(p_soldier) + ": no speed input into the footsteps sound");
	}
	for (const float v : { out.walk_velocity_threshold, out.run_velocity_threshold, out.walk_period, out.run_period }) {
		if (!std::isfinite(v) || !(v > 0.0f)) {
			return make_error(
					std::string(p_soldier) + ": a SoldierSoundComponentData threshold or period is not positive");
		}
	}
	return out;
}

}

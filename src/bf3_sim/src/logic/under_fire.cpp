#include "bf3/sim/logic/under_fire.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/level/connections.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/logic/conquest_data.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace bf3::sim::logic {

namespace gp = core::gameplay;

namespace {

float field_f32(const core::EbxInstance &p_entity, std::string_view p_field, float p_default) {
	const std::optional<core::EbxValue> v = p_entity.get(p_field);
	const std::optional<float> f = v ? v->as_f32() : std::nullopt;
	return f && std::isfinite(*f) ? *f : p_default;
}

bool field_bool(const core::EbxInstance &p_entity, std::string_view p_field, bool p_default) {
	const std::optional<core::EbxValue> v = p_entity.get(p_field);
	const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
	return b.value_or(p_default);
}

std::optional<core::level::AreaVolume> linked_box(
		const core::level::World &p_level, const core::level::PlacedObject &p_trigger) {
	for (const core::level::Connection &link : p_level.scopes.connections(p_trigger.scope).link) {
		if (!(link.source == p_trigger.entity) || !link.target.is_a("OBBData")) {
			continue;
		}
		const core::level::PlacedObject *placed = p_level.placed(p_trigger.scope, link.target);
		core::Result<core::level::AreaVolume> v = core::level::read_area_volume(
				link.target, placed != nullptr ? placed->transform : core::level::LevelTransform::identity());
		if (v) {
			return std::move(*v);
		}
	}
	return std::nullopt;
}

bool value_true(const LogicValue &p_value) {
	switch (p_value.kind) {
		case LogicValue::Kind::Bool:
			return p_value.b;
		case LogicValue::Kind::Int:
			return p_value.i != 0;
		case LogicValue::Kind::Float:
			return p_value.f != 0.0;
		default:
			return false;
	}
}

}

float UnderFireTriggerData::distance(const core::Vec3 &p_point) const {
	if (volume) {
		return volume->distance(p_point);
	}
	const float dx = p_point[0] - position[0], dy = p_point[1] - position[1], dz = p_point[2] - position[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

core::Result<std::shared_ptr<const UnderFireParams>> read_under_fire_params(
		const gp::LogicGraph &p_graph, const core::level::World &p_level) {
	auto out = std::make_shared<UnderFireParams>();
	for (uint32_t i = 0; i < p_level.objects.size(); ++i) {
		const core::level::PlacedObject &o = p_level.objects[i];
		if (o.entity.type_name() != "UnderFireTriggerEntityData") {
			continue;
		}
		const std::optional<uint32_t> node = placed_logic_node(p_graph, p_level, i, gp::LogicTypeId::UnderFireTrigger);
		if (!node) {
			out->notes.push_back("under fire: placed trigger " + o.entity.guid().to_string() + " has no logic node");
			continue;
		}
		if (out->triggers.size() >= kMaxUnderFireTriggers) {
			return core::make_error("under fire: more than " + std::to_string(kMaxUnderFireTriggers) + " triggers");
		}
		UnderFireTriggerData t;
		t.node = *node;
		t.level_object = i;
		t.volume = linked_box(p_level, o);
		t.position = t.volume ? t.volume->centre() : o.transform.trans;
		t.radius = std::max(0.0f, field_f32(o.entity, "Radius", 0.0f));
		t.cool_down_s = std::max(0.0f, field_f32(o.entity, "CoolDownTime", 0.0f));
		t.delay_s = std::max(0.0f, field_f32(o.entity, "Delay", 0.0f));
		t.run_once = field_bool(o.entity, "RunOnce", false);
		t.enabled = field_bool(o.entity, "Enabled", true);
		out->triggers.push_back(t);
	}
	std::sort(out->triggers.begin(), out->triggers.end(),
			[](const UnderFireTriggerData &a, const UnderFireTriggerData &b) { return a.node < b.node; });
	return std::shared_ptr<const UnderFireParams>(std::move(out));
}

UnderFireNatives::UnderFireNatives(std::shared_ptr<const UnderFireParams> p_params) :
		params_(p_params ? std::move(p_params) : std::make_shared<const UnderFireParams>()) {
	reset();
}

void UnderFireNatives::reset() {
	triggers_.assign(params_->triggers.size(), Trigger{});
	for (size_t i = 0; i < triggers_.size(); ++i) {
		triggers_[i].enabled = params_->triggers[i].enabled;
	}
}

size_t UnderFireNatives::index_of(uint32_t p_node) const {
	const auto it = std::lower_bound(params_->triggers.begin(), params_->triggers.end(), p_node,
			[](const UnderFireTriggerData &t, uint32_t n) { return t.node < n; });
	return it == params_->triggers.end() || it->node != p_node ? triggers_.size()
															   : size_t(it - params_->triggers.begin());
}

bool UnderFireNatives::enabled(uint32_t p_node) const {
	const size_t i = index_of(p_node);
	return i < triggers_.size() && triggers_[i].enabled;
}

void UnderFireNatives::apply(Tick p_tick, std::span<const LogicEffect> p_effects) {
	for (const LogicEffect &e : p_effects) {
		if (e.apply_tick != p_tick) {
			continue;
		}
		const size_t i = index_of(e.node);
		if (i >= triggers_.size()) {
			continue;
		}
		Trigger &t = triggers_[i];
		if (e.kind == LogicEffectKind::Event && e.port == gp::logic_port::kEnable) {
			t.enabled = true;
			t.spent = false;
		} else if (e.kind == LogicEffectKind::Event && e.port == gp::logic_port::kDisable) {
			t.enabled = false;
		} else if (e.kind == LogicEffectKind::Property && e.port == gp::logic_port::kEnabled) {
			t.enabled = value_true(e.value);
		}
	}
}

// INFERRED: a hit within Radius of the linked OBBData box fires while Enabled, after Delay, once per CoolDownTime,
// RunOnce until the next Enable (the data's own Delays suggest it). Settle: a note on UnderFireTrigger.
core::Status UnderFireNatives::impacts(Tick p_tick, std::span<const core::Vec3> p_points, LogicRuntime &r_runtime) {
	stats_.impacts += p_points.size();
	for (size_t i = 0; i < triggers_.size(); ++i) {
		const UnderFireTriggerData &d = params_->triggers[i];
		Trigger &t = triggers_[i];
		bool inside = false;
		for (const core::Vec3 &p : p_points) {
			if (d.distance(p) <= d.radius) {
				inside = true;
				break;
			}
		}
		if (inside) {
			++stats_.inside;
			if (!t.enabled) {
				++stats_.disabled;
			} else if (t.spent) {
				++stats_.spent;
			} else if (t.fired_once && p_tick < t.ready) {
				++stats_.cooling;
			} else {
				t.fired_once = true;
				t.ready = p_tick + seconds_to_ticks(d.cool_down_s);
				t.spent = d.run_once;
				t.pending.push_back(p_tick + seconds_to_ticks(d.delay_s));
			}
		}
		for (auto it = t.pending.begin(); it != t.pending.end();) {
			if (*it > p_tick) {
				++it;
				continue;
			}
			BF3_TRY(r_runtime.force_event(d.node, kUnderFireOut, LogicContext{}));
			++stats_.fired;
			it = t.pending.erase(it);
		}
	}
	return core::Status::success();
}

}

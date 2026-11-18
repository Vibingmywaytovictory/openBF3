#include "bf3/render/level_lights.h"

#include "bf3/core/level/census.h"
#include "bf3/core/level/eor_camera.h"
#include "bf3/core/level/world.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace bf3::render {

using core::EbxInstance;
using core::EbxResolver;
using core::EbxValue;
using core::level::InstanceKey;
using core::level::InstanceKeyHash;
using core::level::LevelTransform;
using core::level::ObjectKind;
using core::level::PlacedObject;
using core::level::World;

namespace {

enum class Refusal : uint8_t {
	None,
	Invisible,
	Unlit,
	Unusable,
};

struct LocalLight {
	EbxInstance entity;
	LevelTransform local;
};

bool flag(const EbxInstance &p_instance, std::string_view p_field, bool p_default) {
	const std::optional<EbxValue> v = p_instance.get(p_field);
	const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
	return b.value_or(p_default);
}

LevelTransform local_transform(const EbxInstance &p_instance) {
	const std::optional<EbxValue> v = p_instance.get("Transform");
	const std::optional<core::level::Rows> rows = v ? v->as_transform() : std::nullopt;
	return rows ? LevelTransform::from_rows(*rows) : LevelTransform::identity();
}

std::vector<LocalLight> health_state_lights(const EbxResolver &p_db, const EbxInstance &p_entity) {
	struct Pending {
		EbxInstance component;
		LevelTransform frame;
	};
	std::vector<LocalLight> out;
	std::vector<Pending> pending;
	for (const EbxInstance &component : p_db.follow_all(p_entity.data(), "Components")) {
		pending.push_back(Pending{ component, LevelTransform::identity() });
	}
	std::reverse(pending.begin(), pending.end());
	size_t seen = 0;
	while (!pending.empty()) {
		const Pending at = pending.back();
		pending.pop_back();
		if (++seen > kMaxLightComponents) {
			break;
		}
		const LevelTransform frame = local_transform(at.component).in_parent(at.frame);
		std::vector<EbxInstance> children = p_db.follow_all(at.component.data(), "Components");
		std::reverse(children.begin(), children.end());
		for (const EbxInstance &child : children) {
			if (pending.size() < kMaxLightComponents) {
				pending.push_back(Pending{ child, frame });
			}
		}
		if (!at.component.is_a("PartComponentData")) {
			continue;
		}
		const std::vector<EbxInstance> states = p_db.follow_all(at.component.data(), "HealthStates");
		if (states.empty()) {
			continue;
		}
		for (const EbxInstance &object : p_db.follow_all(states.front().data(), "Objects")) {
			if (object.is_a("LocalLightEntityData")) {
				out.push_back(LocalLight{ object, local_transform(object).in_parent(frame) });
			}
		}
	}
	return out;
}

Refusal read_light(const EbxResolver &p_db, const EbxInstance &p_entity, SceneLight &r_light, std::string &r_note) {
	if (!flag(p_entity, "Visible", true)) {
		return Refusal::Invisible;
	}
	if (p_entity.is_a("SpotLightEntityData")) {
		core::Result<core::level::SpotLight> spot = core::level::read_spot_light(p_db, p_entity);
		if (!spot) {
			r_note = spot.error().message;
			return Refusal::Unusable;
		}
		r_light.color = spot->color;
		r_light.intensity = spot->intensity;
		r_light.radius = spot->radius;
		r_light.specular = spot->specular;
		r_light.spot = std::move(*spot);
	} else if (p_entity.is_a("PointLightEntityData")) {
		const std::optional<core::Vec3> color = core::level::ebx_vec3(p_entity, "Color");
		const std::optional<float> intensity = core::level::ebx_f32(p_entity, "Intensity");
		const std::optional<float> radius = core::level::ebx_f32(p_entity, "Radius");
		if (!color || !intensity || !radius) {
			r_note = "light " + p_entity.guid().to_string() + " has no Color, Intensity or Radius";
			return Refusal::Unusable;
		}
		r_light.color = *color;
		r_light.intensity = *intensity;
		r_light.radius = *radius;
		r_light.specular = flag(p_entity, "SpecularEnable", false);
	} else {
		r_note = "light " + p_entity.guid().to_string() + " is a " + std::string(p_entity.type_name()) +
				", neither a point nor a spot light";
		return Refusal::Unusable;
	}
	const float peak = std::fmax(std::fmax(r_light.color[0], r_light.color[1]), r_light.color[2]);
	if (!(r_light.radius > 0.0f) || !(r_light.intensity > 0.0f) || !(peak > 0.0f)) {
		return Refusal::Unlit;
	}
	return Refusal::None;
}

core::Result<bool> starts_off(const World &p_world, const PlacedObject &p_object) {
	PlacedObject probe = p_object;
	for (const std::optional<EbxInstance> &target :
			{ p_object.reference, std::optional<EbxInstance>(p_object.entity) }) {
		if (!target) {
			continue;
		}
		probe.reference = target;
		core::Result<std::vector<core::level::VisibilityDriver>> drivers =
				core::level::visibility_drivers(p_world, probe, core::gameplay::LogicSide::Client);
		if (!drivers) {
			return drivers.error();
		}
		for (const core::level::VisibilityDriver &d : *drivers) {
			if (!d.start) {
				return true;
			}
		}
	}
	return false;
}

}

size_t SceneLightPlan::spots() const {
	return size_t(
			std::count_if(lights.begin(), lights.end(), [](const SceneLight &p_l) { return p_l.spot.has_value(); }));
}

size_t SceneLightPlan::points() const {
	return lights.size() - spots();
}

core::Result<SceneLightPlan> plan_level_lights(const EbxResolver &p_db, const World &p_world) {
	SceneLightPlan plan;
	std::optional<uint32_t> end_of_round;
	if (core::Result<std::optional<core::level::EorCamera>> camera = core::level::read_eor_camera(p_db, p_world);
			camera && *camera && (*camera)->light) {
		end_of_round = (*camera)->light->object;
	}
	std::unordered_map<InstanceKey, std::vector<LocalLight>, InstanceKeyHash> carried;
	const auto refuse = [&](Refusal p_refusal, const std::string &p_note) {
		++(p_refusal == Refusal::Invisible			  ? plan.stats.invisible
						: p_refusal == Refusal::Unlit ? plan.stats.unlit
													  : plan.stats.unusable);
		if (!p_note.empty()) {
			plan.notes.push_back(p_note);
		}
	};
	for (uint32_t at = 0; at < p_world.objects.size(); ++at) {
		const PlacedObject &object = p_world.objects[at];
		if (plan.lights.size() >= kMaxSceneLights) {
			plan.notes.push_back("more than " + std::to_string(kMaxSceneLights) + " lights: the rest ignored");
			break;
		}
		if (object.kind == ObjectKind::Light) {
			const bool spot = object.entity.is_a("SpotLightEntityData");
			++(spot ? plan.stats.placed_spot : plan.stats.placed_point);
			if (end_of_round == at) {
				++plan.stats.end_of_round;
				continue;
			}
			SceneLight light;
			light.source = SceneLightSource::Placed;
			light.object = at;
			light.transform = object.transform;
			std::string note;
			if (const Refusal refusal = read_light(p_db, object.entity, light, note); refusal != Refusal::None) {
				refuse(refusal, note);
				continue;
			}
			core::Result<bool> off = starts_off(p_world, object);
			if (!off) {
				return core::error_at("level lights", off.error());
			}
			if (*off) {
				++plan.stats.switched_off;
				continue;
			}
			plan.lights.push_back(std::move(light));
			continue;
		}
		if (object.kind != ObjectKind::Mesh) {
			continue;
		}
		const InstanceKey key = InstanceKey::of(object.entity);
		auto found = carried.find(key);
		if (found == carried.end()) {
			found = carried.emplace(key, health_state_lights(p_db, object.entity)).first;
		}
		if (found->second.empty()) {
			continue;
		}
		core::Result<bool> hidden = core::level::starts_hidden(p_world, object, core::gameplay::LogicSide::Client);
		if (!hidden) {
			return core::error_at("level lights", hidden.error());
		}
		for (const LocalLight &local : found->second) {
			const bool spot = local.entity.is_a("SpotLightEntityData");
			++(spot ? plan.stats.health_state_spot : plan.stats.health_state_point);
			SceneLight light;
			light.source = SceneLightSource::HealthState;
			light.object = at;
			light.transform = local.local.in_parent(object.transform);
			std::string note;
			if (const Refusal refusal = read_light(p_db, local.entity, light, note); refusal != Refusal::None) {
				refuse(refusal, note);
				continue;
			}
			if (*hidden) {
				++plan.stats.switched_off;
				continue;
			}
			plan.lights.push_back(std::move(light));
		}
	}
	return plan;
}

void nearest_lights(std::span<const SceneLight> p_lights, const core::Vec3 &p_eye, size_t p_budget,
		std::vector<uint32_t> &r_shown) {
	r_shown.clear();
	if (p_lights.size() <= p_budget) {
		r_shown.resize(p_lights.size());
		for (uint32_t i = 0; i < r_shown.size(); ++i) {
			r_shown[i] = i;
		}
		return;
	}
	std::vector<std::pair<float, uint32_t>> reach(p_lights.size());
	for (uint32_t i = 0; i < p_lights.size(); ++i) {
		const core::Vec3 &at = p_lights[i].transform.trans;
		const float dx = at[0] - p_eye[0], dy = at[1] - p_eye[1], dz = at[2] - p_eye[2];
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - p_lights[i].radius;
		reach[i] = { std::isfinite(distance) ? distance : INFINITY, i };
	}
	std::nth_element(reach.begin(), reach.begin() + std::ptrdiff_t(p_budget), reach.end());
	r_shown.reserve(p_budget);
	for (size_t i = 0; i < p_budget; ++i) {
		r_shown.push_back(reach[i].second);
	}
	std::sort(r_shown.begin(), r_shown.end());
}

}

#include "bf3/core/gameplay/vehicle_sound.h"

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/level_transform.h"
#include "gameplay_read.h"

#include <algorithm>
#include <span>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr size_t kMaxSoundViewDepth = 4;

struct PropertyLink {
	EbxInstance source;
	int32_t source_field = 0;
	EbxInstance target;
	int32_t target_field = 0;
};

std::vector<PropertyLink> property_links(
		const EbxResolver &p_db, const EbxInstance &p_blueprint, WorkBudget &r_budget) {
	std::vector<PropertyLink> out;
	const std::optional<EbxValue> list = p_blueprint.get("PropertyConnections");
	const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
	if (!array) {
		return out;
	}
	for (const EbxValue entry : *array) {
		if (!r_budget.take(1)) {
			return out;
		}
		const auto end = [&](std::string_view p_field) -> std::optional<EbxInstance> {
			const std::optional<EbxValue> v = entry.get(p_field);
			const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
			return r ? p_db.resolve(p_blueprint.partition(), *r) : std::nullopt;
		};
		const auto id = [&](std::string_view p_field) -> int32_t {
			const std::optional<EbxValue> v = entry.get(p_field);
			const std::optional<int32_t> i = v ? v->as_i32() : std::nullopt;
			return i.value_or(0);
		};
		const std::optional<EbxInstance> source = end("Source");
		const std::optional<EbxInstance> target = end("Target");
		if (source && target) {
			out.push_back(PropertyLink{ *source, id("SourceFieldId"), *target, id("TargetFieldId") });
		}
	}
	return out;
}

void sound_view_cameras(std::span<const PropertyLink> p_links, const EbxInstance &p_source, int32_t p_field,
		size_t p_depth, std::vector<uint32_t> &r_cameras) {
	if (p_depth > kMaxSoundViewDepth) {
		return;
	}
	if (p_source.is_a("CameraComponentData")) {
		if (p_field == kCameraViewActiveField &&
				std::find(r_cameras.begin(), r_cameras.end(), p_source.index()) == r_cameras.end()) {
			r_cameras.push_back(p_source.index());
		}
		return;
	}
	if (!p_source.is_a("OrEntityData") && !p_source.is_a("Or4EntityData")) {
		return;
	}
	for (const PropertyLink &l : p_links) {
		if (l.target == p_source) {
			sound_view_cameras(p_links, l.source, l.source_field, p_depth + 1, r_cameras);
		}
	}
}

struct Walk {
	const EbxResolver &db;
	WorkBudget &budget;
	VehicleSoundDef &out;
	std::span<const PropertyLink> links;
	size_t components = 0;
	bool bad_transform = false;

	void sound_refs(const EbxInstance &p_component) {
		const EbxStruct c = p_component.data();
		if (p_component.is_a("WarningSystemComponentData")) {
			out.missile_warning = ref_name(budget, db, c, "MissileWarnSoundEffect");
			out.locked_warning = ref_name(budget, db, c, "LockedWarnSoundEffect");
			out.locking_warning = ref_name(budget, db, c, "LockingWarnSoundEffect");
		}
		if (p_component.is_a("CameraComponentData") && out.switch_to_alternate_view.empty()) {
			if (const std::optional<EbxInstance> s = db.follow(c, "CameraSoundData");
					s && budget.take_type(s->data())) {
				out.switch_to_alternate_view = ref_name(budget, db, s->data(), "SwitchToAlternateViewSound");
				out.switch_to_normal_view = ref_name(budget, db, s->data(), "SwitchToNormalViewSound");
			}
		}
	}

	void walk(const EbxInstance &p_container, const level::LevelTransform &p_frame, size_t p_depth) {
		if (p_depth > kMaxVehicleComponentDepth) {
			return;
		}
		for (const EbxInstance &component : follow_all(budget, db, p_container.data(), "Components")) {
			if (components >= kMaxVehicleComponents) {
				return;
			}
			if (!active_component(component)) {
				continue;
			}
			++components;
			const Result<level::LevelTransform> local = component_transform(component);
			bad_transform = bad_transform || !local;
			const level::LevelTransform here = (local ? *local : level::LevelTransform::identity()).in_parent(p_frame);
			const EbxStruct c = component.data();
			if (component.is_a("EngineComponentData")) {
				VehicleEngineSound e;
				e.patch = ref_name(budget, db, c, "SoundEffect");
				e.surface_patch = ref_name(budget, db, c, "SurfaceSoundEffect");
				e.transform = Transform::from_axes({ here.right, here.up, here.forward }, here.trans);
				e.first_person_sounds = boolean(c, "UseFirstPersonSounds").value_or(false);
				for (const PropertyLink &l : links) {
					if (l.target == component && l.target_field == kUseFirstPersonSoundsField) {
						sound_view_cameras(links, l.source, l.source_field, 0, e.first_person_cameras);
					}
				}
				if (!e.patch.empty() || !e.surface_patch.empty()) {
					out.engines.push_back(std::move(e));
				}
			}
			sound_refs(component);
			walk(component, here, p_depth + 1);
		}
	}
};

}

Result<VehicleSoundDef> read_vehicle_sounds(
		const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string name = to_lower_ascii(p_blueprint);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("vehicle sounds: no partition `" + name + "`");
	}
	const EbxInstance blueprint = partition->primary();
	BF3_TRY(budget.charge_type(blueprint.data()));
	const std::optional<EbxInstance> entity = p_db.follow(blueprint.data(), "Object");
	if (!entity || !entity->is_a("VehicleEntityData")) {
		return make_error("vehicle sounds: `" + name + "` has no VehicleEntityData");
	}
	BF3_TRY(budget.charge_type(entity->data()));
	VehicleSoundDef out;
	out.blueprint = name;
	if (const std::optional<EbxInstance> s = p_db.follow(entity->data(), "Sound"); s && budget.take_type(s->data())) {
		out.bullet_impact = ref_name(budget, p_db, s->data(), "BulletImpact");
	}
	const std::vector<PropertyLink> links = property_links(p_db, blueprint, budget);
	Walk w{ p_db, budget, out, links };
	w.walk(*entity, level::LevelTransform::identity(), 0);
	if (w.bad_transform) {
		return make_error("vehicle sounds " + name + ": non-finite or out-of-range component Transform");
	}
	Result<VehicleDef> def = read_vehicle_def(p_db, name, &budget);
	if (!def) {
		return make_error("vehicle sounds " + name + ": " + def.error().message);
	}
	for (size_t t = 0; t < def->turrets.size() && t < size_t(INT8_MAX); ++t) {
		if (def->turrets[t].sound.empty()) {
			continue;
		}
		Vec3 position = def->turrets[t].frame.trans;
		for (int up = def->turrets[t].parent; up >= 0 && size_t(up) < t; up = def->turrets[size_t(up)].parent) {
			position = def->turrets[size_t(up)].frame.transform_point(position);
		}
		out.turrets.push_back(VehicleTurretSound{ def->turrets[t].sound, position, static_cast<int8_t>(t),
				def->turrets[t].sound_start_rpm, def->turrets[t].sound_stop_rpm });
	}
	return out;
}

}

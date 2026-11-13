#include "bf3/core/gameplay/vehicle_sound.h"

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/level_transform.h"
#include "gameplay_read.h"

namespace bf3::core::gameplay {

using namespace detail;

namespace {

struct Walk {
	const EbxResolver &db;
	WorkBudget &budget;
	VehicleSoundDef &out;
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
				e.position = here.trans;
				e.first_person_sounds = boolean(c, "UseFirstPersonSounds").value_or(false);
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
	Walk w{ p_db, budget, out };
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
		out.turrets.push_back(VehicleTurretSound{ def->turrets[t].sound, position, static_cast<int8_t>(t) });
	}
	return out;
}

}

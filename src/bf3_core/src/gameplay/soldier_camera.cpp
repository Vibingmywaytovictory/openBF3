#include "bf3/core/gameplay/soldier_camera.h"

#include "ebx_fields.h"

#include <optional>

namespace bf3::core::gameplay {

using detail::Fields;

namespace {

ChaseCameraDef read_chase(Fields &r_f, const EbxStruct &p_s) {
	ChaseCameraDef c;
	c.target_offset = r_f.vec3(p_s, "TargetOffset");
	c.target_rotation_offset_deg = r_f.f32(p_s, "TargetRotationOffset");
	c.wanted_angle_deg = r_f.f32(p_s, "WantedAngleDeg");
	c.wanted_distance = r_f.f32(p_s, "WantedDistance");
	c.max_distance = r_f.f32(p_s, "MaxDistance");
	c.snap_distance = r_f.f32(p_s, "SnapDistance");
	c.to_wanted_position_scale = r_f.vec3(p_s, "ToWantedPositionScale");
	c.velocity_drag = r_f.f32(p_s, "VelocityDrag");
	c.max_velocity = r_f.f32(p_s, "MaxVelocity");
	c.away_from_target_force_scale = r_f.f32(p_s, "AwayFromTargetForceScale");
	c.force_field_radius = r_f.f32(p_s, "ForceFieldRadius");
	c.force_field_force_scale = r_f.f32(p_s, "ForceFieldForceScale");
	c.collision_radius = r_f.f32(p_s, "CollisionRadius");
	c.has_collision = r_f.flag(p_s, "HasCollision");
	c.pill_minimum_collision_radius = r_f.f32(p_s, "PillMinimumCollisionRadius");
	c.pill_maximum_collision_radius = r_f.f32(p_s, "PillMaximumCollisionRadius");
	c.pill_minimum_collision_length = r_f.f32(p_s, "PillMinimumCollisionLength");
	c.pill_expand_size_speed_acceleration = r_f.f32(p_s, "PillExpandSizeSpeedAcceleration");
	c.look_distance_scale = r_f.f32(p_s, "LookDistanceScale");
	c.look_distance_in_front_of_target = r_f.f32(p_s, "LookDistanceInFrontOfTarget");
	c.max_view_rotation_angle_deg = r_f.f32(p_s, "MaxViewRotationAngleDeg");
	c.keep_target_pitch = r_f.flag(p_s, "KeepTargetPitch");
	c.should_roll_with_target = r_f.flag(p_s, "ShouldRollWithTarget");
	c.inherit_target_velocity = r_f.flag(p_s, "InheritTargetVelocity");
	c.update_rate = float(r_f.i32(p_s, "UpdateRate"));
	return c;
}

}

const SoldierCameraDef *SoldierCameras::first_person() const {
	for (const SoldierCameraDef &c : cameras) {
		if (c.kind == SoldierCameraKind::FirstPerson) {
			return &c;
		}
	}
	return nullptr;
}

Result<SoldierCameras> read_soldier_cameras(const EbxResolver &p_db, std::string_view p_soldier) {
	Fields f(p_db);
	const EbxPartition *part = p_db.partition_by_name(p_soldier);
	if (part == nullptr) {
		return make_error(std::string(p_soldier) + ": partition not found");
	}
	const std::vector<EbxInstance> components = part->instances_of("SoldierCameraComponentData");
	if (components.size() != 1) {
		return make_error(std::string(p_soldier) + ": " + std::to_string(components.size()) +
				" SoldierCameraComponentData, expected one");
	}
	const EbxStruct c = components.front().data();
	const OptionalEbxArray list = f.array(c, "Cameras");
	if (!list) {
		return f.error();
	}
	SoldierCameras out;
	for (size_t i = 0; i < list->size(); ++i) {
		const std::optional<EbxValue> v = list->get(i);
		const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
		if (!r || r->is_null()) {
			return make_error(f.where(c, "Cameras") + ": entry " + std::to_string(i) + " is not a camera reference");
		}
		const std::optional<EbxInstance> cam =
				f.follow_ref(components.front().partition(), *r, "CameraData", f.where(c, "Cameras"));
		if (!cam) {
			return f.error();
		}
		SoldierCameraDef d;
		d.type = std::string(cam->type_name());
		const EbxStruct s = cam->data();
		d.shake_factor = f.f32(s, "ShakeFactor");
		d.fade_time = f.f32(s, "FadeTime");
		if (cam->is_a("ChaseCameraData")) {
			d.kind = SoldierCameraKind::Chase;
			d.chase = read_chase(f, s);
		} else if (cam->is_a("FPSCameraData")) {
			d.kind = SoldierCameraKind::FirstPerson;
		}
		out.cameras.push_back(std::move(d));
	}
	if (!f.ok()) {
		return f.error();
	}
	return out;
}

}

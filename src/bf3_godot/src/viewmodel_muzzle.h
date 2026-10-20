#pragma once

#include "bf3/fx/triggers.h"

#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace bf3::godot_side {

class Bf3Viewmodel;

enum class ViewmodelMuzzleSource : uint8_t {
	None = 0,
	WeaponRoot = 1,
	CameraStandIn = 2,
};

inline const godot::Vector3 kMuzzleCameraStandIn(0.10f, -0.11f, -0.70f);

struct ViewmodelMuzzle {
	bool valid = false;
	ViewmodelMuzzleSource source = ViewmodelMuzzleSource::None;
	int32_t joint = -1;
	godot::Transform3D weapon;
	godot::Transform3D muzzle;
};

ViewmodelMuzzle viewmodel_muzzle_transform(const Bf3Viewmodel &p_viewmodel);

ViewmodelMuzzle viewmodel_muzzle_transform(const Bf3Viewmodel &p_viewmodel, const godot::Transform3D &p_offset);

fx::MuzzleSample muzzle_sample(const ViewmodelMuzzle &p_muzzle, uint8_t p_slot, const fx::Vec3 &p_velocity);

godot::Transform3D to_godot_transform(const fx::Transform &p_t);
fx::Transform to_fx_transform(const godot::Transform3D &p_t);

}

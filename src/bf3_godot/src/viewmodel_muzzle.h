#pragma once
// G6 (W3.0 contract, docs/design/m2a-w3.md; written by the integrator with its body, fx3 owns it afterwards): the own
// 1p muzzle in world space from a presented Bf3Viewmodel, for VFX3's OwnShot effects (FireEffects1p) and the own
// weapon sound's emitter. vm's files are not touched: everything here reads Bf3Viewmodel's public accessors.
//
// The joint [verified: bf3dump dump of animations/skeletons/weapon/weaponske01 on MP_Subway]: the weapon skeleton has
// no muzzle joint (no Wep_Muzzle, no Wep_Barrel: its bones are Wep_Root, Wep_Extra1-3, Wep_Trigger, Wep_Slide,
// Wep_Grenade1-2, Wep_Mag, Wep_Mag_Ammo, Wep_Physic1-3, Wep_Belt1-5, Wep_Bipod1-3, IK_Joint_Left/RightHand, Wep_Aim).
// A weapon's FireEffects1p offsets are relative to the weapon (fx_demo.cpp, m2a-fx2.md §6.1), whose frame on the rig
// is Wep_Root (matched by name, anim/skeleton.h). So:
//   weapon  = the rig node's global transform x model[Wep_Root] (Bf3Viewmodel::model(), this frame's pose), in
//             Godot world space; the rig keeps Frostbite axes below its node (viewmodel.cpp places the node with the
//             180 degree turn), so an offset is applied in Frostbite weapon axes, as the data stores it;
//   muzzle  = weapon x p_offset: fx3 passes the FireEffects1p entry's offset (triggers.h FireEffect::offset);
//             without one the muzzle is the weapon frame itself.
// Fallback when the viewmodel has no rig, no item in hand or no Wep_Root: the nominal camera-space point of fx2's demo
// (0.10, -0.11, -0.70) [inferred] on the rig's CameraJoint, flagged CameraStandIn so the E8 report counts it.
// Main thread, after Bf3Viewmodel::present for the frame.

#include "bf3/fx/triggers.h"

#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace bf3::godot_side {

class Bf3Viewmodel;

enum class ViewmodelMuzzleSource : uint8_t {
	None = 0, // not built / never presented
	WeaponRoot = 1, // Wep_Root x offset (the data's frame)
	CameraStandIn = 2, // fx2's camera-space point
};

// fx2's nominal 1p muzzle in camera space (Godot: x right, y up, -z forward) [inferred, fx_demo.cpp].
inline const godot::Vector3 kMuzzleCameraStandIn(0.10f, -0.11f, -0.70f);

struct ViewmodelMuzzle {
	bool valid = false;
	ViewmodelMuzzleSource source = ViewmodelMuzzleSource::None;
	int32_t joint = -1; // the rig index of Wep_Root (-1: stand-in)
	godot::Transform3D weapon; // world: the frame FireEffects1p offsets are relative to
	godot::Transform3D muzzle; // world: weapon x offset (the stand-in: on the camera, -Z = forward)
};

ViewmodelMuzzle viewmodel_muzzle_transform(const Bf3Viewmodel &p_viewmodel);
// p_offset in Frostbite weapon axes (a FireEffects1p entry's transform, converted with render_bridge's to_godot).
ViewmodelMuzzle viewmodel_muzzle_transform(const Bf3Viewmodel &p_viewmodel, const godot::Transform3D &p_offset);

// fx3: the muzzle as VFX3's MuzzleSample (first_person, the effect frame of triggers.h: local +Y = the barrel, local +Z
// = the weapon's up). WeaponRoot: the muzzle's basis is the Frostbite weapon frame (column 2 = the barrel, column 1 =
// up), so the frame is effect_frame_of_weapon of it; CameraStandIn: the camera's -Z is the barrel. Invalid when the
// muzzle is. p_velocity: the own soldier's velocity (InheritSpeedAndDirectionFromEmitter).
fx::MuzzleSample muzzle_sample(const ViewmodelMuzzle &p_muzzle, uint8_t p_slot, const fx::Vec3 &p_velocity);
// fx::Transform (rows right, up, forward, trans: world vectors) <-> godot::Transform3D (basis columns, origin).
godot::Transform3D to_godot_transform(const fx::Transform &p_t);
fx::Transform to_fx_transform(const godot::Transform3D &p_t);

} // namespace bf3::godot_side

#pragma once
// Everything the 3p crowd needs, read from the install and prepared for upload (S3; docs/design/s3.md §4, §6).
// It extends the M0 viewmodel loader (viewmodel_assets.h) from 1P arms to the two MP soldiers. Godot-free: it
// runs on a loader thread, and the Godot side only creates resources from the result.
//
// Default spec [verified 2026-10-15, superbundle Win32/Levels/MP_Subway/MP_Subway, bundle
// Win32/levels/mp_subway/teamdeathmatch]:
//   US  appearance persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01, linking
//       helmet01, head01, upperbody01, lowerbody01 -> 3p meshes (LOD0 verts / tris / bones):
//         characters/upperbody/us_upperbody04/us_upperbody04_mesh  var us_upperbody04_desert  8,436 / 12,426 / 62
//         characters/lowerbody/us_lowerbody03/us_lb03_mesh         var us_lb03_desert         3,722 /  5,666 / 14
//         characters/heads/sp_cole/head_cole_mp_mesh                                         2,302 /  4,256 / 41
//         characters/headgear/us_helmet09/us_helmet09_mesh         var us_helmet09_desert     4,541 /  6,708 /  1
//       weapon weapons/m16a4/m16a4_3p_mesh (10,200 / 12,450 / 5); SoldierWeaponData weapons/m16a4/m16a4, WeaponDef weapons/m16a4/u_m16a4
//       team total: 113 binds, 29,201 vertices, 41,506 triangles
//   RU  appearance persistence/unlocks/soldiers/visual/mp/ru/mp_ru_assault_appearance01 (no head part):
//         characters/upperbody/ru_upperbody01/ru_upperbody01_mesh  var ru_upperbody01_desert  8,460 / 12,796 / 62
//         characters/lowerbody/us_lowerbody03/us_lb03_mesh         var ru_lb03_desert         3,722 /  5,666 / 14
//         characters/headgear/ru_helmet05/ru_helmet05_mesh         var ru_helmet05_desert     6,475 /  9,942 / 34
//       weapon weapons/ak74m/ak74_3p_mesh (12,338 / 15,330 / 5); SoldierWeaponData weapons/ak74m/ak74, WeaponDef weapons/ak74m/u_ak74m
//       team total: 107 binds, 30,995 vertices, 43,734 triangles
// Body parts skin to the EBX skeleton animations/skeletons/veniceantske01 (213 bones, every name in the rig;
// the rig adds Left/RightAnkleEffectorAux). Weapons skin to animations/skeletons/weapon/weaponske01. Every bone
// slot's name hash equals hash_quick(lowercase EBX name), and binding by name into the rig (215 joints)
// resolves for every LOD0 slot. At rig rest, 109 of the 113 US skin matrices (103 of the 107 RU) are identity
// within 1e-3. The rest are weapon bones (worst 0.338, Wep_Mag_Ammo / Wep_Mag), as expected for a weapon bound
// from weaponske01. No mesh names a *Cloth or *Phys joint (the clips leave those undriven).
// The 3p animatable's skeleton animations/skeletons/venice3pmasterskeleton [verified] is a MasterSkeletonAsset:
// MasterSkeleton = veniceantske01 itself, plus one SubSkeleton weaponske01 with a BoneMap of 25 (weapon bone k ->
// master bone 0, 155..178, same names) and a TransformMap of 25 pure translations (no rotation, up to 0.80 m).
// So the body binds are exactly veniceantske01's, as used here. The TransformMap does not explain the weapon
// bind offsets (residuals of 0.09-1.12 m either way), so S3 does not apply it; the weapon is fixed instead by the
// weaponske01 layout of its undriven joints (SoldierTeam::weapon_joints / weapon_local, crowd_eval.h step 3):
// 23 of the 25 weaponske01 bones have a parent of the same name in veniceantske01 (the other two are the roots
// exportAnimation and Wep_Root, which hangs under Spine2 in the rig).
// Materials come from MeshAsset.Materials through the MeshVariationDatabase (material_lookup.h), as for the
// level: shaders/root/characterroot (+ _alphatest, _lod, _skin), eye_simple, teeth, alphablend, and
// weapons/shaders/weaponpreset3p. Material-family mapping belongs to the L track; S3 only calls
// mesh_materials and make_standard_material.

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/anim/temporal_lod.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"
#include "bf3/render/viewmodel_assets.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

struct SoldierTeamSpec {
	std::string appearance; // VisualUnlockAsset whose linked parts give the 3p meshes
	std::string weapon_mesh; // 3p weapon SkinnedMeshAsset
	std::string weapon_unlock; // SoldierWeaponData partition: WeaponStates[0].Weapon (AntRef) -> reload chooser index
	std::string weapon_def; // WeaponDef source for the reload time (reload playback rate, s3.md §5.4)
	// PT1 iff [kits] (docs/design/pt1-iff.md §2.1): the team's TeamData (gameplay/teams/us). Set: each kit's default
	// appearance (core::gameplay::read_kit_appearance) joins the team's parts, SoldierPart::kit_mask says which kits wear
	// a part; empty (the default, S3): `appearance` alone, every part worn by every kit, exactly as before.
	std::string team_data; // PT1 iff [kits]
};

struct SoldierSpec {
	std::string superbundle = "Win32/Levels/MP_Subway/MP_Subway";
	std::string settings_superbundle = "Win32/default_settings_win32"; // GameAnimationSettings (temporal LOD)
	std::string settings_partition = "default/settings_win32";
	std::string body_skeleton = "animations/skeletons/veniceantske01";
	std::string weapon_skeleton = "animations/skeletons/weapon/weaponske01";
	uint32_t lod = 0; // mesh LOD prepared (S3 draws one LOD; s3.md §13)
	std::array<SoldierTeamSpec, 2> teams{ {
			{ "persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01", "weapons/m16a4/m16a4_3p_mesh", "weapons/m16a4/m16a4",
					"weapons/m16a4/u_m16a4" },
			{ "persistence/unlocks/soldiers/visual/mp/ru/mp_ru_assault_appearance01", "weapons/ak74m/ak74_3p_mesh", "weapons/ak74m/ak74",
					"weapons/ak74m/u_ak74m" },
	} };
	anim::CrowdClipNames clips;
};

struct SoldierPart {
	std::string mesh; // MeshAsset partition
	std::string variation; // ObjectVariation partition ("" = default)
	uint32_t variation_hash = 0;
	bool weapon = false; // skinned against weapon_skeleton
	uint8_t kit_mask = 0x0F; // PT1 iff [kits]: bit k = kit k (host::KitRole) wears the part
	MeshMaterials materials;
	PreparedMesh prepared; // LOD SoldierSpec::lod, bones = indices into the team's SkinBinds
	core::CachedItem cache;
	double seconds_mesh = 0.0;
};

struct SoldierTeam {
	std::string appearance;
	std::array<std::string, 4> kit_appearances; // PT1 iff [kits]: per host::KitRole ("" = `appearance`)
	std::vector<SoldierPart> parts; // body parts in appearance order, then the weapon
	anim::SkinBinds binds; // one list for every part of the team: one skeleton per soldier
	// crowd_eval.h step 3 (rig-sized): 1 for the rig joints that are weaponske01 bones below Wep_Root and that the
	// locomotion clips leave undriven (21 [verified]); their weaponske01 LocalPose. render.s3 checks that at rig rest
	// with this layout every weapon skin matrix equals Wep_Root's within 1e-3.
	anim::BoneMask weapon_joints;
	std::vector<anim::Xform> weapon_local;
	uint32_t reload_index = 0; // chooser index (M16A4 41, AK-74M 33)
	anim::CrowdClipData reload; // load_chosen_clip(reload_chooser, reload_index)
	float reload_time = 0.0f; // WeaponDef reload time (0 when unresolved: rate 1)
	uint64_t vertices = 0, triangles = 0;
};

struct SoldierAssets {
	std::unique_ptr<core::ant::AntLibrary> library; // the clip bindings' rig points into it
	anim::CrowdClipSet clips; // locomotion + aim (+ noise) on the rig
	anim::EbxSkeleton body_skeleton;
	anim::EbxSkeleton weapon_skeleton;
	std::array<SoldierTeam, 2> teams;
	anim::TemporalLodTable lod; // from GameAnimationSettings (defaults + a note when unreadable)
	std::vector<PreparedTexture> textures; // every texture the drawn surfaces use, unique by name
	std::vector<std::string> notes;
	double seconds_ant = 0.0, seconds_ebx = 0.0, seconds_meshes = 0.0, seconds_textures = 0.0, seconds_total = 0.0;

	const PreparedTexture *texture(const std::string &p_name) const;
};

// Loads and prepares both teams. Mesh blobs go through p_cache (kind "mesh", key with the team's lane binds,
// mesh_cache.h); clips and textures are decoded from the install each run (plan §2.4). Texture decode runs on
// parallel_for. Fails when an appearance, mesh, skeleton or clip does not resolve; a missing variation, head
// part or reload time is a note.
core::Result<std::unique_ptr<SoldierAssets>> load_soldier_assets(const core::InstallReader &p_install, const SoldierSpec &p_spec,
		LoadProgress *r_progress, core::Cache *p_cache = nullptr);

// Shoulder pinch metric on CPU-skinned vertices (s3.md §9 G3). For each vertex whose largest weight is on an
// upper-arm bone (LeftArm / RightArm and their Roll joints), within p_radius m of the shoulder joint in the bind
// pose, the ratio = (posed distance to the upper-arm bone axis) / (bind distance to that axis). Reports the
// minimum ratio per side and how many vertices fall below 0.7.
struct PinchReport {
	float min_ratio_left = 1.0f, min_ratio_right = 1.0f;
	uint32_t vertices_left = 0, vertices_right = 0; // vertices measured
	uint32_t below_left = 0, below_right = 0; // ratio < 0.7
	float hand_ik_m = 0.0f; // |LeftHand - IK_Joint_LeftHand| in the posed model space (grip error)
};
PinchReport measure_pinch(const SoldierTeam &p_team, const anim::Skeleton &p_rig, std::span<const anim::Xform> p_model,
		std::span<const anim::Mat34> p_palette, float p_radius = 0.18f);

} // namespace bf3::render

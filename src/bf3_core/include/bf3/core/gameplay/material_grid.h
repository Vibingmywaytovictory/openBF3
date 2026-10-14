#pragma once
// The level's material interaction grid and the soldier hit capsules (plan §3.2, topics/01 §1.7,
// EBX probe §4 "Damage multipliers").
//
// Damage lookup [verified by reproduction of the community multipliers]:
//   pair = the MaterialContainerPair of the projectile and of the hit capsule (or surface);
//   row  = MaterialGridData.MaterialIndexMap[pair.PhysicsPropertyIndex & 0xFF];
//   cell = InteractionGrid[row_projectile].Items[row_target].PhysicsPropertyProperties
//          -> MaterialRelationDamageData (+ MaterialRelationPenetrationData).
// Which DamageRelation field scales hit damage is [inferred]: DamageProtectionMultiplier reproduces
// the 2.0 / 2.35 headshots and the .338 1.06 chest rule. Impact effects and sounds use the other
// index space (PhysicsMaterialIndex, PhysicsMaterialProperties) and are not read here.
//
// Pairs live in `materials/materialcontainer` (MaterialContainerAsset, partition b50615c2). On the
// delta (XP2-XP4) levels the copy in the level's own `<L>_gameconfiglight_win32` bundle is the base
// one and lacks pairs the patched grid references (handoff 01 §"XP2/3/4 caveat", 3,842 unresolved on
// XP2_Palace), so pairs missing from the level's copy are taken from fallback resolvers (any other
// level carries the patched container under the same partition GUID).
//
// Grid lookup through LevelData.RuntimeMaterialGrid follows IW4L level_bf3 (collision/material.rs),
// Apache-2.0.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::gameplay {

// MaterialRelationDamageData (+ MaterialRelationPenetrationData) of one grid cell.
struct DamageRelation {
	bool present = false; // the cell holds a MaterialRelationDamageData
	float protection_multiplier = 1; // DamageProtectionMultiplier
	float protection_threshold = 0;
	float penetration_multiplier = 1;
	float collision_multiplier = 1;
	float collision_threshold = 0;
	float explosion_cover_modifier = 1;
	bool inflicts_demolition = false;
	bool has_penetration = false; // the cell holds a MaterialRelationPenetrationData
	bool never_penetrate = false;
};

class MaterialGrid {
public:
	struct LoadReport {
		std::string grid; // partition
		size_t grid_pairs = 0; // MaterialGridData.MaterialPairs entries
		size_t unresolved_in_level = 0; // of those, missing from the level's own container copy
		size_t unresolved = 0; // still missing after the fallbacks
		size_t container_pairs = 0; // pairs known (level copy + fallback additions)
		size_t from_fallback = 0; // pairs taken from a fallback container
	};

	// The grid named by the level's LevelData.RuntimeMaterialGrid (p_level = "levels/mp_subway/mp_subway").
	static std::optional<std::string> grid_of_level(const EbxResolver &p_db, std::string_view p_level);
	// Reads a MaterialGridData partition and its container. p_fallbacks are searched, in order, for
	// container pairs the level's copy lacks.
	static Result<MaterialGrid> load(const EbxResolver &p_db, std::string_view p_grid, std::span<const EbxResolver *const> p_fallbacks = {});

	const LoadReport &report() const { return report_; }
	size_t row_count() const { return rows_; }
	// MaterialContainerAsset.MaterialNames: names of the property / material indices ("Head", "BulletDamage").
	const std::vector<std::string> &material_names() const { return names_; }
	std::string_view material_name(uint8_t p_index) const;
	std::optional<uint8_t> property_by_name(std::string_view p_name) const;

	// A container pair by instance GUID (level copy first, then fallbacks).
	std::optional<MaterialPairRef> pair(const Guid &p_instance) const;
	// Fills r_ref's values from the container when the resolver that produced it could not.
	bool complete(MaterialPairRef &r_ref) const;

	std::optional<uint32_t> row_of_index(uint8_t p_index) const;
	// The cell for (projectile property, target property); a default (absent) relation when either
	// index has no row.
	DamageRelation damage(uint8_t p_projectile_property, uint8_t p_target_property) const;

private:
	LoadReport report_;
	size_t rows_ = 0;
	std::vector<int64_t> index_map_;
	std::vector<DamageRelation> cells_; // rows_ * rows_
	std::vector<std::string> names_;
	std::unordered_map<Guid, MaterialPairRef, GuidHash> pairs_;
};

// BoneCollisionData of `characters/soldiers/defaultsoldierbonecollision` (SkeletonCollisionData).
struct HitCapsule {
	std::string bone;
	std::string hit_reaction; // AnimationHitReactionType
	float radius = 0;
	float length = 0;
	Vec3 offset{};
	int32_t bone_axis = 0;
	bool valid_hi_lod = false;
	bool valid_low_lod = false;
	bool deactivate_if_behind_wall = false;
	bool use_physics_rotation = false;
	MaterialPairRef material;
};

inline constexpr std::string_view kSoldierBoneCollision = "characters/soldiers/defaultsoldierbonecollision";

Result<std::vector<HitCapsule>> read_hit_capsules(const EbxResolver &p_db, std::string_view p_partition = kSoldierBoneCollision);

// Damage multipliers of the hi-LOD capsules for one projectile property, by the capsules' property
// names: head = "Head", body = "Body" (arms, lower spine), chest = "Chest" (upper-spine sphere),
// legs = "Foot" (thighs, shins). NaN where no capsule carries that property.
struct BodyMultipliers {
	float head = 0;
	float body = 0;
	float chest = 0;
	float legs = 0;
};
BodyMultipliers body_multipliers(const MaterialGrid &p_grid, std::span<const HitCapsule> p_capsules, uint8_t p_projectile_property);

} // namespace bf3::core::gameplay

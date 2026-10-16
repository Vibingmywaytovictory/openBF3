#pragma once
// What a hit is made of, for damage, penetration and the impact descriptor (docs/design/m2a-weapons.md §5.8;
// plan §3.2 "Damage", "Effects and impacts"; topics/01 §1.7).
//
// Two index spaces read the same level grid (plan §3.2, gap VFX §3) [verified]:
//   damage / penetration: row = MaterialIndexMap[PhysicsPropertyIndex & 0xFF] (MaterialGrid::damage);
//   impact effects, sounds, decals: the pair's PhysicsMaterialIndex through the same map (bf3_fx / audio).
// A static hit names its pair by grid index (SurfaceTag::material & kMaterialPairMask, grid_pairs.h); a capsule
// hit by the capsule's pair; a projectile carries its own pair. The table is built once per level load (the
// level's MaterialGrid, the grid pair list, the soldier capsules) and shared read-only by every owner thread.

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/weapons/hit_target.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim::weapons {

// The grid's view of one material pair (container pair, resolved).
struct SurfacePair {
	bool resolved = false;
	uint8_t property = 0; // PhysicsPropertyIndex & 0xFF: the damage row / column
	uint8_t physics_material = 0; // PhysicsMaterialIndex & 0xFF: the impact effect / sound / decal row
	bool penetrable = false; // FlagsAndIndex bit 24 (topics/01 §1.7, C9: 1,800 of 32,400 pairs)
	bool see_through = false; // FlagsAndIndex bit 27
	uint32_t flags_and_index = 0;
};

class SurfaceMaterialTable {
public:
	// p_grid_pairs: read_grid_pairs of the level's grid (index = SurfaceTag::material & 0xFFFFFF).
	// p_capsules: read_hit_capsules; the zone properties are taken from the hi-LOD capsules' pairs by the
	// property names Head / Body / Chest / Foot (an absent zone is an error).
	static core::Result<SurfaceMaterialTable> make(std::shared_ptr<const core::gameplay::MaterialGrid> p_grid,
			std::span<const core::gameplay::MaterialPairRef> p_grid_pairs, std::span<const core::gameplay::HitCapsule> p_capsules);

	// The pair of a static triangle's material value (the low 24 bits; out of range or unresolved: resolved false).
	SurfacePair static_pair(uint32_t p_material_value) const;
	// A projectile's or explosion's own pair (completed from the grid's container when the resolver left it open).
	SurfacePair pair_of(const core::gameplay::MaterialPairRef &p_ref) const;
	// The grid cell (projectile property, target property); an absent cell is DamageRelation{} (present false).
	core::gameplay::DamageRelation relation(uint8_t p_projectile_property, uint8_t p_target_property) const;
	// The property the capsules of a zone carry (Head 65, Body 66, Chest 158, Foot 91 on Metro [verified,
	// topics/01 §1.7]).
	uint8_t zone_property(HitZone p_zone) const { return zone_property_[size_t(p_zone)]; }
	// A default-constructed table (no grid: every relation absent, every pair unresolved) serves the synthetic
	// defs of the no-data tests.
	bool has_grid() const { return grid_ != nullptr; }
	const core::gameplay::MaterialGrid &grid() const { return *grid_; }
	size_t grid_pair_count() const { return pairs_.size(); }

private:
	std::shared_ptr<const core::gameplay::MaterialGrid> grid_;
	std::vector<SurfacePair> pairs_;
	std::array<uint8_t, kHitZoneCount> zone_property_{};
};

} // namespace bf3::sim::weapons

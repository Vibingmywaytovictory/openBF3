#pragma once

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

struct SurfacePair {
	bool resolved = false;
	uint8_t property = 0;
	uint8_t physics_material = 0;
	bool penetrable = false;
	bool see_through = false;
	uint32_t flags_and_index = 0;
};

class SurfaceMaterialTable {
public:

	static core::Result<SurfaceMaterialTable> make(std::shared_ptr<const core::gameplay::MaterialGrid> p_grid,
			std::span<const core::gameplay::MaterialPairRef> p_grid_pairs, std::span<const core::gameplay::HitCapsule> p_capsules);

	SurfacePair static_pair(uint32_t p_material_value) const;

	SurfacePair pair_of(const core::gameplay::MaterialPairRef &p_ref) const;

	core::gameplay::DamageRelation relation(uint8_t p_projectile_property, uint8_t p_target_property) const;

	uint8_t zone_property(HitZone p_zone) const { return zone_property_[size_t(p_zone)]; }

	bool has_grid() const { return grid_ != nullptr; }
	const core::gameplay::MaterialGrid &grid() const { return *grid_; }
	size_t grid_pair_count() const { return pairs_.size(); }

private:
	std::shared_ptr<const core::gameplay::MaterialGrid> grid_;
	std::vector<SurfacePair> pairs_;
	std::array<uint8_t, kHitZoneCount> zone_property_{};
};

}

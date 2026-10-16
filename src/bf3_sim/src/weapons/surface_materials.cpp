// SurfaceMaterialTable: the level grid's pairs by index, the zone properties of the soldier capsules and the
// damage cells (docs/design/m2a-weapons.md §4.3, §5.8, §5.9).
#include "bf3/sim/weapons/surface_materials.h"

#include <string>

namespace bf3::sim::weapons {

namespace {

SurfacePair surface_pair(const core::gameplay::MaterialPairRef &p_ref) {
	SurfacePair s;
	if (!p_ref.resolved) {
		return s;
	}
	s.resolved = true;
	s.property = p_ref.property();
	s.physics_material = p_ref.material();
	s.flags_and_index = p_ref.flags_and_index;
	s.penetrable = ((p_ref.flags_and_index >> 24) & 1u) != 0;
	s.see_through = ((p_ref.flags_and_index >> 27) & 1u) != 0;
	return s;
}

} // namespace

const char *hit_zone_name(HitZone p_zone) {
	switch (p_zone) {
		case HitZone::Head:
			return "head";
		case HitZone::Body:
			return "body";
		case HitZone::Chest:
			return "chest";
		case HitZone::Legs:
			return "legs";
	}
	return "?";
}

core::Result<SurfaceMaterialTable> SurfaceMaterialTable::make(std::shared_ptr<const core::gameplay::MaterialGrid> p_grid,
		std::span<const core::gameplay::MaterialPairRef> p_grid_pairs, std::span<const core::gameplay::HitCapsule> p_capsules) {
	if (p_grid == nullptr) {
		return core::make_error("SurfaceMaterialTable: no material grid");
	}
	if (p_grid_pairs.size() > (size_t(1) << 24)) {
		return core::make_error("SurfaceMaterialTable: " + std::to_string(p_grid_pairs.size()) + " grid pairs (limit 2^24)");
	}
	SurfaceMaterialTable t;
	t.grid_ = std::move(p_grid);
	t.pairs_.reserve(p_grid_pairs.size());
	for (const core::gameplay::MaterialPairRef &r : p_grid_pairs) {
		core::gameplay::MaterialPairRef c = r;
		t.grid_->complete(c);
		t.pairs_.push_back(surface_pair(c));
	}
	// Zone properties from the hi-LOD capsules' pairs by their property names (gameplay::body_multipliers' rule).
	constexpr const char *kZoneNames[kHitZoneCount] = { "Head", "Body", "Chest", "Foot" };
	std::array<bool, kHitZoneCount> found{};
	for (const core::gameplay::HitCapsule &cap : p_capsules) {
		if (!cap.valid_hi_lod) {
			continue;
		}
		core::gameplay::MaterialPairRef m = cap.material;
		if (!t.grid_->complete(m)) {
			continue;
		}
		const std::string_view name = t.grid_->material_name(m.property());
		for (size_t z = 0; z < kHitZoneCount; ++z) {
			if (!found[z] && name == kZoneNames[z]) {
				found[z] = true;
				t.zone_property_[z] = m.property();
			}
		}
	}
	for (size_t z = 0; z < kHitZoneCount; ++z) {
		if (!found[z]) {
			return core::make_error(std::string("SurfaceMaterialTable: no hi-LOD capsule carries the property \"") + kZoneNames[z] + "\"");
		}
	}
	return t;
}

SurfacePair SurfaceMaterialTable::static_pair(uint32_t p_material_value) const {
	const uint32_t index = p_material_value & 0x00FF'FFFFu;
	return index < pairs_.size() ? pairs_[index] : SurfacePair{};
}

SurfacePair SurfaceMaterialTable::pair_of(const core::gameplay::MaterialPairRef &p_ref) const {
	core::gameplay::MaterialPairRef c = p_ref;
	if (grid_ != nullptr) {
		grid_->complete(c);
	}
	return surface_pair(c);
}

core::gameplay::DamageRelation SurfaceMaterialTable::relation(uint8_t p_projectile_property, uint8_t p_target_property) const {
	return grid_ != nullptr ? grid_->damage(p_projectile_property, p_target_property) : core::gameplay::DamageRelation{};
}

} // namespace bf3::sim::weapons

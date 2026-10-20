#pragma once

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

struct DamageRelation {
	bool present = false;
	float protection_multiplier = 1;
	float protection_threshold = 0;
	float penetration_multiplier = 1;
	float collision_multiplier = 1;
	float collision_threshold = 0;
	float explosion_cover_modifier = 1;
	bool inflicts_demolition = false;
	bool has_penetration = false;
	bool never_penetrate = false;
};

class MaterialGrid {
public:
	struct LoadReport {
		std::string grid;
		size_t grid_pairs = 0;
		size_t unresolved_in_level = 0;
		size_t unresolved = 0;
		size_t container_pairs = 0;
		size_t from_fallback = 0;
	};

	static std::optional<std::string> grid_of_level(const EbxResolver &p_db, std::string_view p_level);

	static Result<MaterialGrid> load(const EbxResolver &p_db, std::string_view p_grid, std::span<const EbxResolver *const> p_fallbacks = {});

	const LoadReport &report() const { return report_; }
	size_t row_count() const { return rows_; }

	const std::vector<std::string> &material_names() const { return names_; }
	std::string_view material_name(uint8_t p_index) const;
	std::optional<uint8_t> property_by_name(std::string_view p_name) const;

	std::optional<MaterialPairRef> pair(const Guid &p_instance) const;

	bool complete(MaterialPairRef &r_ref) const;

	std::optional<uint32_t> row_of_index(uint8_t p_index) const;

	DamageRelation damage(uint8_t p_projectile_property, uint8_t p_target_property) const;

private:
	LoadReport report_;
	size_t rows_ = 0;
	std::vector<int64_t> index_map_;
	std::vector<DamageRelation> cells_;
	std::vector<std::string> names_;
	std::unordered_map<Guid, MaterialPairRef, GuidHash> pairs_;
};

struct HitCapsule {
	std::string bone;
	std::string hit_reaction;
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

struct BodyMultipliers {
	float head = 0;
	float body = 0;
	float chest = 0;
	float legs = 0;
};
BodyMultipliers body_multipliers(const MaterialGrid &p_grid, std::span<const HitCapsule> p_capsules, uint8_t p_projectile_property);

}

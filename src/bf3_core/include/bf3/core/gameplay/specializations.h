#pragma once

#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::gameplay {

enum class SpecializationTarget : uint8_t {
	Sprint,
	AmmoClips,
	Explosives,
	Grenades,
	UnderslungGrenades,
	ExplosionDamage,
	SuppressionResist,
	SuppressionBoost,
	RegenerationDelay,
	Count
};

inline constexpr size_t kSpecializationTargets = size_t(SpecializationTarget::Count);

std::string_view specialization_target_field(SpecializationTarget p_target);

struct SpecializationEffects {
	std::array<float, kSpecializationTargets> value{ 1, 1, 1, 0, 0, 1, 1, 1, 1 };

	float operator[](SpecializationTarget p_target) const { return value[size_t(p_target)]; }
	bool operator==(const SpecializationEffects &) const = default;
};

struct KitSpecialization {
	uint32_t identifier = 0;
	std::string unlock;
	bool squad = false;
	std::vector<std::pair<SpecializationTarget, float>> effects;
};

struct SpecializationTable {
	SpecializationEffects defaults;
	std::vector<KitSpecialization> entries;
	uint64_t hash = 0;

	const KitSpecialization *find(uint32_t p_identifier) const;
};

Result<SpecializationTable> make_specialization_table(const SoldierDef &p_soldier, std::span<const KitDef> p_kits);

SpecializationEffects specialization_effects(
		const SpecializationTable &p_table, uint32_t p_own, std::span<const uint32_t> p_squad);

int32_t boosted_magazines(int32_t p_magazines, const SpecializationEffects &p_effects);

uint64_t specialization_table_hash(const SpecializationTable &p_table);

}

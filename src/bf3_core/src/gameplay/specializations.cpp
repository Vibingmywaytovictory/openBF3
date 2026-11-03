#include "bf3/core/gameplay/specializations.h"

#include "bf3/core/util/fnv.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace bf3::core::gameplay {

namespace {

constexpr std::array<std::string_view, kSpecializationTargets> kTargetFields{ "SprintMultiplier",
	"AmmoClipIncreaseMultiplier", "ExplosiveIncreaseMultiplier", "GrenadeIncrease", "UnderslungGrenadeIncrease",
	"ExplosionDamageModifier", "SuppressionResistModifier", "SuppressionBoostModifier", "RegenerationDelayModifier" };

std::optional<SpecializationTarget> target_of(std::string_view p_field) {
	for (size_t i = 0; i < kTargetFields.size(); ++i) {
		if (kTargetFields[i] == p_field) {
			return SpecializationTarget(i);
		}
	}
	return std::nullopt;
}

}

std::string_view specialization_target_field(SpecializationTarget p_target) {
	return size_t(p_target) < kTargetFields.size() ? kTargetFields[size_t(p_target)] : std::string_view();
}

const KitSpecialization *SpecializationTable::find(uint32_t p_identifier) const {
	if (p_identifier == 0) {
		return nullptr;
	}
	for (const KitSpecialization &e : entries) {
		if (e.identifier == p_identifier) {
			return &e;
		}
	}
	return nullptr;
}

Result<SpecializationTable> make_specialization_table(const SoldierDef &p_soldier, std::span<const KitDef> p_kits) {
	SpecializationTable out;
	for (const SpecializationDef &s : p_soldier.specializations) {
		const std::optional<SpecializationTarget> t = target_of(s.target_field);
		if (!t) {
			return make_error("specializations: " + s.unlock + " " + s.id + " drives " + s.target_type + "." +
					s.target_field + ", which no simulated field reads");
		}
		if (!std::isfinite(s.value) || !std::isfinite(s.default_value)) {
			return make_error("specializations: " + s.unlock + " " + s.id + " has a non-finite value");
		}
		out.defaults.value[size_t(*t)] = float(s.default_value);
	}
	for (const KitDef &k : p_kits) {
		for (const KitUnlock &u : k.specializations) {
			if (u.identifier == 0 || out.find(u.identifier) != nullptr) {
				continue;
			}
			KitSpecialization e;
			e.identifier = u.identifier;
			e.unlock = to_lower_ascii(u.unlock);
			bool matched = false;
			for (const SpecializationDef &s : p_soldier.specializations) {
				if (to_lower_ascii(s.unlock) != e.unlock) {
					continue;
				}
				matched = true;
				e.squad = s.squad;
				e.effects.emplace_back(*target_of(s.target_field), float(s.value));
			}
			if (!matched && !e.unlock.starts_with(kNoAccessoryUnlockPrefix)) {
				return make_error("specializations: the kit " + k.asset + " lists " + e.unlock +
						", which the soldier's UnlockComponentData do not wire");
			}
			out.entries.push_back(std::move(e));
		}
	}
	out.hash = specialization_table_hash(out);
	return out;
}

SpecializationEffects specialization_effects(
		const SpecializationTable &p_table, uint32_t p_own, std::span<const uint32_t> p_squad) {
	SpecializationEffects out = p_table.defaults;
	const auto apply = [&](const KitSpecialization *p_entry) {
		if (p_entry == nullptr) {
			return;
		}
		for (const auto &[target, value] : p_entry->effects) {
			out.value[size_t(target)] = value;
		}
	};
	const KitSpecialization *own = p_table.find(p_own);
	if (own != nullptr && !own->squad) {
		apply(own);
	}
	for (const uint32_t id : p_squad) {
		const KitSpecialization *e = p_table.find(id);
		if (e != nullptr && e->squad) {
			apply(e);
		}
	}
	return out;
}

int32_t boosted_magazines(int32_t p_magazines, const SpecializationEffects &p_effects) {
	const double boosted = std::round(double(p_magazines) * double(p_effects[SpecializationTarget::AmmoClips]));
	return int32_t(std::clamp(boosted, 0.0, double(INT32_MAX)));
}

uint64_t specialization_table_hash(const SpecializationTable &p_table) {
	uint64_t h = kFnv1aOffset;
	for (const float v : p_table.defaults.value) {
		fnv1a_value(h, v);
	}
	fnv1a_value(h, uint64_t(p_table.entries.size()));
	for (const KitSpecialization &e : p_table.entries) {
		fnv1a_value(h, e.identifier);
		fnv1a(h, e.unlock.data(), e.unlock.size());
		fnv1a_value(h, e.squad);
		fnv1a_value(h, uint64_t(e.effects.size()));
		for (const auto &[target, value] : e.effects) {
			fnv1a_value(h, target);
			fnv1a_value(h, value);
		}
	}
	return h;
}

}

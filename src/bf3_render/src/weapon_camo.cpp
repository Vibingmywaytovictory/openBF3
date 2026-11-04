#include "bf3/render/weapon_camo.h"

#include "bf3/core/util/text.h"

namespace bf3::render {

namespace {

std::string partition_name(const core::EbxResolver &p_db, const core::EbxInstance &p_instance) {
	return core::to_lower_ascii(p_db.name_of(p_instance.partition().guid()).value_or(std::string_view()));
}

std::vector<core::EbxInstance> linked(const core::EbxResolver &p_db, std::string_view p_unlock) {
	const core::EbxPartition *part = p_db.partition_by_name(p_unlock);
	if (part == nullptr || !part->primary().is_a("UnlockAssetBase")) {
		return {};
	}
	return p_db.follow_all(part->primary().data(), "LinkedTo");
}

}

std::optional<WeaponCamo> weapon_camo(
		const core::EbxResolver &p_db, std::string_view p_unlock, std::string_view p_weapon_blueprint) {
	const std::string blueprint = core::to_lower_ascii(p_weapon_blueprint);
	for (const core::EbxInstance &pair : linked(p_db, p_unlock)) {
		if (!pair.is_a("BlueprintAndVariationPair")) {
			continue;
		}
		const std::optional<core::EbxInstance> base = p_db.follow(pair.data(), "BaseAsset");
		const std::optional<core::EbxInstance> variation = p_db.follow(pair.data(), "Variation");
		if (!base || !variation || !variation->is_a("ObjectVariation") || partition_name(p_db, *base) != blueprint) {
			continue;
		}
		const std::optional<core::EbxValue> hash = variation->get("NameHash");
		const uint32_t h = hash ? hash->as_u32().value_or(0) : 0;
		if (h == 0) {
			continue;
		}
		return WeaponCamo{ core::to_lower_ascii(p_unlock), partition_name(p_db, *variation), h };
	}
	return std::nullopt;
}

bool unlock_links_nothing(const core::EbxResolver &p_db, std::string_view p_unlock) {
	const core::EbxPartition *part = p_db.partition_by_name(p_unlock);
	if (part == nullptr || !part->primary().is_a("UnlockAssetBase")) {
		return false;
	}
	const std::optional<core::EbxValue> links = part->primary().get("LinkedTo");
	const core::OptionalEbxArray array = links ? links->as_array() : core::OptionalEbxArray();
	return array && array->size() == 0;
}

uint32_t weapon_camo_hash(
		const core::EbxResolver &p_db, const WeaponAccessories &p_accessories, std::string_view p_weapon_blueprint) {
	for (const std::string &unlock : p_accessories.active) {
		if (const std::optional<WeaponCamo> camo = weapon_camo(p_db, unlock, p_weapon_blueprint)) {
			return camo->variation_hash;
		}
	}
	return 0;
}

}

#include "bf3/anim/temporal_lod.h"

#include "bf3/core/ebx.h"

#include <cmath>
#include <optional>
#include <string>

namespace bf3::anim {

namespace {

std::optional<float> read_f32(const core::EbxInstance &p_instance, const std::string &p_field) {
	const std::optional<core::EbxValue> v = p_instance.get(p_field);
	if (!v) {
		return std::nullopt;
	}
	return v->as_f32();
}

}

core::Result<TemporalLodTable> read_game_animation_settings(const core::EbxPartition &p_partition) {
	const std::vector<core::EbxInstance> found = p_partition.instances_of("GameAnimationSettings");
	if (found.empty()) {
		return core::make_error("GameAnimationSettings: no instance in the partition");
	}
	const core::EbxInstance &s = found.front();
	static const char *const kOrdinals[TemporalLodTable::kTiers] = { "First", "Second", "Third", "Fourth", "Fifth", "Sixth" };
	TemporalLodTable t;
	for (size_t i = 0; i < TemporalLodTable::kTiers; ++i) {
		const std::string distance_field = std::string("TemporalLodding") + kOrdinals[i] + "Distance";
		const std::string delta_field = std::string("TemporalLodding") + kOrdinals[i] + "DeltaTime";
		const std::optional<float> d = read_f32(s, distance_field);
		const std::optional<float> dt = read_f32(s, delta_field);
		if (!d || !dt) {
			return core::make_error("GameAnimationSettings: missing " + (d ? delta_field : distance_field));
		}
		if (!std::isfinite(*d) || !std::isfinite(*dt) || *d < 0.0f || *dt < 0.0f) {
			return core::make_error("GameAnimationSettings: " + distance_field + " / " + delta_field + " not finite or negative");
		}
		if (i > 0 && !(*d > t.tiers[i - 1].distance)) {
			return core::make_error("GameAnimationSettings: " + distance_field + " does not increase");
		}
		t.tiers[i] = TemporalLodTier{ *d, *dt };
	}
	const std::optional<float> far = read_f32(s, "TemporalLoddingFarDistance");
	if (!far || !std::isfinite(*far) || *far < t.tiers[TemporalLodTable::kTiers - 1].distance) {
		return core::make_error("GameAnimationSettings: TemporalLoddingFarDistance missing, not finite or below the last tier");
	}
	t.far_distance = *far;
	if (const std::optional<core::EbxValue> client = s.get("ClientEnable")) {
		t.client_enable = client->as_bool().value_or(true);
	}
	return t;
}

}

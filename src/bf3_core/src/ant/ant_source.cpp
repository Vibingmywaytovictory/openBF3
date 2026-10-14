// Package source helpers shared by every AntPackageSource. Clean-room implementation from
// research/probes/ant-animation-probe.md (§2); see ant_source.h.
#include "bf3/core/ant/ant_source.h"

#include "bf3/core/ids.h"

namespace bf3::core::ant {

std::string package_short_name(std::string_view p_path) {
	const size_t slash = p_path.find_last_of('/');
	return to_lower_ascii(slash == std::string_view::npos ? p_path : p_path.substr(slash + 1));
}

AntPackagingType packaging_from_enum_name(std::string_view p_name) {
	if (p_name.ends_with("Static")) {
		return AntPackagingType::Static;
	}
	if (p_name.ends_with("Chunk")) {
		return AntPackagingType::Chunk;
	}
	if (p_name.ends_with("AnimationSet")) {
		return AntPackagingType::AnimationSet;
	}
	return AntPackagingType::Bundle;
}

const AntPackageInfo *AntPackageSource::find(std::string_view p_short_name) const {
	const std::string key = to_lower_ascii(p_short_name);
	for (const AntPackageInfo &p : packages()) {
		if (p.short_name == key) {
			return &p;
		}
	}
	return nullptr;
}

bool is_mp_relevant(std::string_view p_short_name) {
	for (std::string_view prefix : { "b_sp", "b_coop", "b_faceposer", "b_vo" }) {
		if (p_short_name.substr(0, prefix.size()) == prefix) {
			return false;
		}
	}
	return true;
}

} // namespace bf3::core::ant

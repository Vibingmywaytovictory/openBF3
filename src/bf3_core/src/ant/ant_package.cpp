// Ant AssetBank package header. Clean-room implementation from research/probes/ant-animation-probe.md
// (§2, §3.1); see ant_package.h.
#include "bf3/core/ant/ant_package.h"

#include <algorithm>

namespace bf3::core::ant {

namespace {
uint32_t be32(const uint8_t *p_at) {
	return (uint32_t(p_at[0]) << 24) | (uint32_t(p_at[1]) << 16) | (uint32_t(p_at[2]) << 8) | uint32_t(p_at[3]);
}
} // namespace

const char *packaging_name(AntPackagingType p_type) {
	switch (p_type) {
		case AntPackagingType::Static:
			return "Static";
		case AntPackagingType::Chunk:
			return "Chunk";
		case AntPackagingType::Bundle:
			return "Bundle";
		case AntPackagingType::AnimationSet:
			return "AnimationSet";
	}
	return "?";
}

bool AntPackage::parse(std::string p_name, std::vector<uint8_t> p_bytes, std::string &r_error) {
	name_ = std::move(p_name);
	bytes_ = std::move(p_bytes);
	meta_ = {};
	by_index_.clear();
	controller_by_anim_.clear();
	const uint8_t *d = bytes_.data();
	const size_t n = bytes_.size();
	if (n < 4) {
		r_error = name_ + ": package shorter than its header";
		return false;
	}
	const uint32_t type = be32(d);
	if (type > uint32_t(AntPackagingType::AnimationSet)) {
		r_error = name_ + ": unknown packaging type " + std::to_string(type);
		return false;
	}
	packaging_ = static_cast<AntPackagingType>(type);
	archive_offset_ = 4;
	if (packaging_ != AntPackagingType::AnimationSet) {
		if (n < 4 + 24) {
			r_error = name_ + ": truncated PackageMeta";
			return false;
		}
		meta_.present = true;
		meta_.meta_bytes = be32(d + 4);
		meta_.package_type = be32(d + 8);
		meta_.asset_count = be32(d + 12);
		meta_.import_count = be32(d + 16);
		meta_.asset_bytes = be32(d + 20);
		meta_.extra_bytes = be32(d + 24);
		if (meta_.meta_bytes < 24 || meta_.meta_bytes > n - 4) {
			r_error = name_ + ": PackageMeta size out of range";
			return false;
		}
		// Static extras: u32 BE fields inside the meta (diagnostics only).
		const uint64_t extra_end = std::min<uint64_t>(4ull + meta_.meta_bytes, 28ull + meta_.extra_bytes + 4);
		for (uint64_t at = 28; meta_.extra_bytes != 0 && at + 4 <= extra_end && meta_.extras.size() < 8; at += 4) {
			meta_.extras.push_back(be32(d + at));
		}
		archive_offset_ = 4 + meta_.meta_bytes;
	}
	if (!archive_.parse(std::span<const uint8_t>(bytes_).subspan(archive_offset_), r_error)) {
		r_error = name_ + ": " + r_error;
		return false;
	}
	by_index_.reserve(archive_.object_count());
	for (size_t i = 0; i < archive_.object_count(); ++i) {
		const GdStruct o = archive_.object(i);
		GdGuid g;
		if (o.read_guid("__guid", g)) {
			by_index_.emplace(g.index(), uint32_t(i));
		}
		if (o.type_name() == "ClipControllerAsset") {
			GdGuid anim;
			if (o.read_guid("Anim", anim) && !anim.is_null()) {
				controller_by_anim_.emplace(anim.index(), uint32_t(i));
			}
		}
	}
	return true;
}

GdStruct AntPackage::find(uint32_t p_index) const {
	const auto it = by_index_.find(p_index);
	return it == by_index_.end() ? GdStruct() : archive_.object(it->second);
}

GdStruct AntPackage::controller_for(uint32_t p_anim_index) const {
	const auto it = controller_by_anim_.find(p_anim_index);
	return it == controller_by_anim_.end() ? GdStruct() : archive_.object(it->second);
}

} // namespace bf3::core::ant

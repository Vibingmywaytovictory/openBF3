#pragma once

#include "bf3/core/ant/gd_archive.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::ant {

enum class AntPackagingType : uint32_t {
	Static = 0,
	Chunk = 1,
	Bundle = 2,
	AnimationSet = 3,
};

const char *packaging_name(AntPackagingType p_type);

struct AntPackageMeta {
	bool present = false;
	uint32_t meta_bytes = 0;
	uint32_t package_type = 0;
	uint32_t asset_count = 0;
	uint32_t import_count = 0;
	uint32_t asset_bytes = 0;
	uint32_t extra_bytes = 0;

	std::vector<uint32_t> extras;
};

class AntPackage {
public:
	AntPackage() = default;
	AntPackage(const AntPackage &) = delete;
	AntPackage &operator=(const AntPackage &) = delete;

	bool parse(std::string p_name, std::vector<uint8_t> p_bytes, std::string &r_error);

	const std::string &name() const { return name_; }
	AntPackagingType packaging() const { return packaging_; }
	const AntPackageMeta &meta() const { return meta_; }
	const GdArchive &archive() const { return archive_; }
	size_t byte_size() const { return bytes_.size(); }
	uint32_t archive_offset() const { return archive_offset_; }

	size_t object_count() const { return archive_.object_count(); }
	GdStruct object(size_t p_index) const { return archive_.object(p_index); }

	GdStruct find(uint32_t p_index) const;

	GdStruct controller_for(uint32_t p_anim_index) const;

private:
	std::string name_;
	std::vector<uint8_t> bytes_;
	AntPackagingType packaging_ = AntPackagingType::Static;
	AntPackageMeta meta_;
	uint32_t archive_offset_ = 0;
	GdArchive archive_;
	std::unordered_map<uint32_t, uint32_t> by_index_;
	std::unordered_map<uint32_t, uint32_t> controller_by_anim_;
};

}

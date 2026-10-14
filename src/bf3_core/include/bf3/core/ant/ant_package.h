#pragma once
// Ant AssetBank package: the Frostbite package header around one EA GenericData archive.
//
// Clean-room implementation from research/probes/ant-animation-probe.md (§2, §3.1). No code from Rime,
// IceBloc or AssetBankPlugin was used.
//
//   u32 BE packagingType      0 Static, 1 Chunk, 2 Bundle, 3 AnimationSet
//   unless AnimationSet: PackageMeta, u32 BE each: metaBytes, packageType, assetCount, importCount,
//                        assetBytes, extraBytes (+ Static-only extras, then a GUID->int map and an import
//                        hash table that playback does not need)
//   GD.STRM archive at offset 4 + metaBytes (AnimationSet: offset 4)
//
// Where the bytes come from (AntPackageAsset.PackagingType): Static and Bundle packages are the res named
// lowercase(AntPackageAsset.Name); Chunk packages are the chunk AntPackageAsset.StreamingGuid. This class only
// takes the bytes; AntPackageSource (ant_source.h) is the seam to the container layer.

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
	bool present = false; // false for AnimationSet packages
	uint32_t meta_bytes = 0;
	uint32_t package_type = 0;
	uint32_t asset_count = 0;
	uint32_t import_count = 0;
	uint32_t asset_bytes = 0;
	uint32_t extra_bytes = 0;
	// Static only (extra_bytes != 0); kept for diagnostics, not interpreted.
	std::vector<uint32_t> extras;
};

class AntPackage {
public:
	AntPackage() = default;
	AntPackage(const AntPackage &) = delete;
	AntPackage &operator=(const AntPackage &) = delete;

	// Takes ownership of the package bytes. p_name is a label for messages and lookups (e.g. "m9").
	bool parse(std::string p_name, std::vector<uint8_t> p_bytes, std::string &r_error);

	const std::string &name() const { return name_; }
	AntPackagingType packaging() const { return packaging_; }
	const AntPackageMeta &meta() const { return meta_; }
	const GdArchive &archive() const { return archive_; }
	size_t byte_size() const { return bytes_.size(); }
	uint32_t archive_offset() const { return archive_offset_; }

	size_t object_count() const { return archive_.object_count(); }
	GdStruct object(size_t p_index) const { return archive_.object(p_index); }
	// The object whose "__guid" reference index is p_index (top-level GD.DATA objects only).
	GdStruct find(uint32_t p_index) const;
	// Package-local ClipControllerAsset whose "Anim" reference index is p_anim_index.
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

} // namespace bf3::core::ant

#pragma once
// Where Ant package bytes come from. AntInstallSource (ant_install_source.h) reads them from the install
// through the container layer (res by name, chunk by GUID); the antdump tool adds a source over a directory
// of exported payloads for comparisons. Clean-room implementation from
// research/probes/ant-animation-probe.md (§2).

#include "bf3/core/ant/ant_package.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ant {

struct AntPackageInfo {
	std::string partition; // EBX partition, lowercase, e.g. "animations/antanimations/m9"
	std::string name; // AntPackageAsset.Name, e.g. "Animations/AntAnimations/m9"
	std::string short_name; // lowercase last path element, e.g. "m9"
	AntPackagingType packaging = AntPackagingType::Bundle;
	std::string streaming_guid; // Chunk packages
	uint64_t chunk_size = 0; // Chunk packages: AntPackageAsset.ChunkSize
	bool is_asset_package = true; // false: an AssetBank res without an AntPackageAsset (animation sets)
	// Where the payload is read from: the superbundle and bundle (toc id) of the res, or for Chunk packages
	// those of the AntPackageAsset partition whose mount resolves the chunk.
	std::string superbundle;
	std::string bundle;
};

class AntPackageSource {
public:
	virtual ~AntPackageSource() = default;
	// Every AntPackageAsset, sorted by partition name.
	virtual const std::vector<AntPackageInfo> &packages() const = 0;
	// Every AssetBank res, sorted by res name, including animation-set banks without an AntPackageAsset
	// (those have is_asset_package = false and packaging Bundle).
	virtual const std::vector<AntPackageInfo> &all_banks() const = 0;
	// Static / Bundle / animation-set banks: the res named lowercase(name); Chunk packages: the chunk
	// streaming_guid. Thread-safe.
	virtual bool read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const = 0;

	// The AntPackageAsset with that short name (case-insensitive), or null.
	const AntPackageInfo *find(std::string_view p_short_name) const;
};

// The probe's "MP-relevant" package set (research/probes/ant-animation-probe.md §0, census-mp): every
// package except the single-player, co-op, face-poser and voice-over ones (short names starting with
// "b_sp", "b_coop", "b_faceposer", "b_vo"). The static package counts as MP-relevant.
bool is_mp_relevant(std::string_view p_short_name);

// Helpers shared by the sources: lowercase last path element, PackagingType enumerator name -> type
// (the suffix decides, as the export tool does: "..._Static", "..._Chunk", "..._AnimationSet", else Bundle).
std::string package_short_name(std::string_view p_path);
AntPackagingType packaging_from_enum_name(std::string_view p_name);

} // namespace bf3::core::ant

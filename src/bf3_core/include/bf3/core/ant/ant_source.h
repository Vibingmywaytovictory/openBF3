#pragma once
// Where Ant package bytes come from. The runtime will implement AntPackageSource on top of the container
// layer (res by name, chunk by GUID); AntExportDirSource reads a directory of exported payloads for tools
// and tests. Clean-room implementation from research/probes/ant-animation-probe.md (§2).
//
// Export directory layout (produced by a research exporter; it holds BF3 data, so it never enters the repo):
//   res.tsv       name, file, size, res_meta (32 hex chars), superbundle, bundle   (one AssetBank res per row)
//   packages.tsv  partition, type, superbundle, bundle, chunk_file, fields        (one row per AntPackageAsset
//                 EBX partition; fields = "Name=..;PackagingType=AntPackagingType_X;StreamingGuid=..;ChunkSize=N")
//   res/*.bin     res payloads, chunk/<guid>.bin decompressed chunks

#include "bf3/core/ant/ant_package.h"

#include <cstdint>
#include <filesystem>
#include <string>
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
};

class AntPackageSource {
public:
	virtual ~AntPackageSource() = default;
	virtual const std::vector<AntPackageInfo> &packages() const = 0;
	virtual bool read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const = 0;
};

class AntExportDirSource final : public AntPackageSource {
public:
	// Reads the two manifests. Fails if either is missing or malformed.
	bool open(const std::filesystem::path &p_dir, std::string &r_error);

	const std::vector<AntPackageInfo> &packages() const override { return packages_; }
	bool read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const override;

	// Every AssetBank res of the export, including animation-set banks without an AntPackageAsset.
	const std::vector<AntPackageInfo> &all_banks() const { return banks_; }
	const AntPackageInfo *find(std::string_view p_short_name) const;

private:
	struct ResEntry {
		std::string name;
		std::string file;
	};
	std::filesystem::path dir_;
	std::vector<AntPackageInfo> packages_;
	std::vector<AntPackageInfo> banks_;
	std::vector<ResEntry> res_;
	std::vector<std::string> chunk_files_; // parallel to packages_
};

// The probe's "MP-relevant" package set (research/probes/ant-animation-probe.md §0, census-mp): every
// package except the single-player, co-op, face-poser and voice-over ones (short names starting with
// "b_sp", "b_coop", "b_faceposer", "b_vo"). The static package counts as MP-relevant.
bool is_mp_relevant(std::string_view p_short_name);

bool read_whole_file(const std::filesystem::path &p_path, std::vector<uint8_t> &r_bytes, std::string &r_error);

} // namespace bf3::core::ant

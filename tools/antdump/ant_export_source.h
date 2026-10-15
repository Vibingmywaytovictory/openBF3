#pragma once
// AntExportDirSource: Ant package bytes from a directory of exported payloads (the research `antexport`
// output, see README.md "Export directory"). A tool/test source only: it holds BF3 data, so it lives outside
// the repo, and bf3_core itself reads the install through AntInstallSource. Clean-room implementation from
// research/probes/ant-animation-probe.md (§2).
//
// Export directory layout:
//   res.tsv       name, file, size, res_meta (32 hex chars), superbundle, bundle   (one AssetBank res per row)
//   packages.tsv  partition, type, superbundle, bundle, chunk_file, fields        (one row per EBX partition
//                 under animations/antanimations/; fields = "Name=..;PackagingType=AntPackagingType_X;
//                 StreamingGuid=..;ChunkSize=N")
//   res/*.bin     res payloads, chunk/<guid>.bin decompressed chunks

#include "bf3/core/ant/ant_source.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bf3::tools::antdump {

class AntExportDirSource final : public bf3::core::ant::AntPackageSource {
public:
	// Reads the two manifests. Fails if either is missing or malformed.
	bool open(const std::filesystem::path &p_dir, std::string &r_error);

	const std::vector<bf3::core::ant::AntPackageInfo> &packages() const override { return packages_; }
	const std::vector<bf3::core::ant::AntPackageInfo> &all_banks() const override { return banks_; }
	bool read(const bf3::core::ant::AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const override;

private:
	struct ResEntry {
		std::string name;
		std::string file;
		std::string superbundle;
		std::string bundle;
	};
	std::filesystem::path dir_;
	std::vector<bf3::core::ant::AntPackageInfo> packages_;
	std::vector<bf3::core::ant::AntPackageInfo> banks_;
	std::vector<ResEntry> res_;
	std::vector<std::string> chunk_files_; // parallel to packages_
};

// Reads a whole local file (export directory, oracle JSON). Windows file API, never the install: install
// files go through InstallReader / ReadOnlyFile.
bool read_whole_file(const std::filesystem::path &p_path, std::vector<uint8_t> &r_bytes, std::string &r_error);

} // namespace bf3::tools::antdump

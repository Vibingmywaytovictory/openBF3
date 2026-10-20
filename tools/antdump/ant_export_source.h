#pragma once

#include "bf3/core/ant/ant_source.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bf3::tools::antdump {

class AntExportDirSource final : public bf3::core::ant::AntPackageSource {
public:

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
	std::vector<std::string> chunk_files_;
};

bool read_whole_file(const std::filesystem::path &p_path, std::vector<uint8_t> &r_bytes, std::string &r_error);

}

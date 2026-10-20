#pragma once

#include "bf3/core/ant/ant_package.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ant {

struct AntPackageInfo {
	std::string partition;
	std::string name;
	std::string short_name;
	AntPackagingType packaging = AntPackagingType::Bundle;
	std::string streaming_guid;
	uint64_t chunk_size = 0;
	bool is_asset_package = true;

	std::string superbundle;
	std::string bundle;
};

class AntPackageSource {
public:
	virtual ~AntPackageSource() = default;

	virtual const std::vector<AntPackageInfo> &packages() const = 0;

	virtual const std::vector<AntPackageInfo> &all_banks() const = 0;

	virtual bool read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const = 0;

	const AntPackageInfo *find(std::string_view p_short_name) const;
};

bool is_mp_relevant(std::string_view p_short_name);

std::string package_short_name(std::string_view p_path);
AntPackagingType packaging_from_enum_name(std::string_view p_name);

}

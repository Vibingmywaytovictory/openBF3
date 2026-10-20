#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/text_db.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class EbxPartition;
class EbxResolver;
}

namespace bf3::core::ui {

bool is_sid(std::string_view p_text);

void collect_sids(const EbxPartition &p_partition, std::vector<std::string> &r_sids);

std::span<const std::string_view> research_census_partitions();
std::span<const std::string_view> research_census_superbundles();

struct SidCensus {
	size_t partitions = 0;
	std::vector<std::string> missing_partitions;
	std::vector<std::string> sids;
	size_t resolved = 0;
	size_t missing_marker = 0;
	size_t absent = 0;
};

SidCensus sid_census(const EbxResolver &p_ebx, std::span<const std::string_view> p_partitions, const TextDatabase &p_db);

}

#pragma once
// SIDs referenced from EBX and how many of them the text database resolves (research
// gap-r1-ui-hud-minimap-localisation.md §4, plan §7.8 "1,247/1,271 EBX SIDs resolve").
//
// A SID is a CString field value (anywhere in an instance: nested structs and arrays included) that
// is entirely `ID_` followed by upper-case letters, digits and underscores. The research census took
// the distinct SIDs of the 170 partitions it had dumped while mapping the UI (the UI metadata and
// component partitions, the Caspian/Metro level and description partitions, soldier, kits, teams,
// default settings, a few vehicles); 28 of them hold any SID. research_census_partitions() lists those
// 28 with the superbundles they were read from, so the census reproduces the research set exactly.

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
} // namespace bf3::core

namespace bf3::core::ui {

// "ID_" + [A-Z0-9_]*, the whole string.
bool is_sid(std::string_view p_text);
// Appends the SIDs of every instance of a partition (duplicates included, in field order).
void collect_sids(const EbxPartition &p_partition, std::vector<std::string> &r_sids);

// The research census set (see the header comment) and the superbundles to serve it from, in order.
std::span<const std::string_view> research_census_partitions();
std::span<const std::string_view> research_census_superbundles();

struct SidCensus {
	size_t partitions = 0; // scanned
	std::vector<std::string> missing_partitions; // not served by the resolver
	std::vector<std::string> sids; // distinct, sorted
	size_t resolved = 0; // present and not "<MISSING>"
	size_t missing_marker = 0; // present as "<MISSING>"
	size_t absent = 0; // no entry for the hash
};

SidCensus sid_census(const EbxResolver &p_ebx, std::span<const std::string_view> p_partitions, const TextDatabase &p_db);

} // namespace bf3::core::ui

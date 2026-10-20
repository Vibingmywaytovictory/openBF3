#include "bf3/core/ui/text_census.h"

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"

#include <algorithm>

namespace bf3::core::ui {

namespace {

constexpr std::string_view kCensusPartitions[] = {
	"characters/soldiers/mpsoldier",
	"gameplay/kits/ruassault",
	"gameplay/kits/ruengineer",
	"gameplay/kits/rurecon",
	"gameplay/kits/rusupport",
	"gameplay/kits/usassault",
	"gameplay/kits/usengineer",
	"gameplay/kits/usrecon",
	"gameplay/kits/ussupport",
	"levels/mp_007/mp_007",
	"levels/mp_007/mp_007/description_win32",
	"levels/mp_subway/mp_subway",
	"levels/mp_subway/mp_subway/description_win32",
	"persistence/richpresencedata",
	"ui/assets/gamehints",
	"ui/uicamometadata",
	"ui/uicomponents/uicombatareacomp",
	"ui/uicomponents/uidetailedserverinfocomp",
	"ui/uigamemodemetadata",
	"ui/uikititemmetadata",
	"ui/uilicensemetadata",
	"ui/uimodmetadata",
	"ui/uispecializationmetadata",
	"ui/uivehicleaccessorymetadata",
	"ui/uivehiclemetadata",
	"ui/uivehicleweaponmetadata",
	"ui/uiweaponaccessorymetadata",
	"ui/uiweaponmetadata",
};

constexpr std::string_view kCensusSuperbundles[] = {
	"Win32/UI",
	"Win32/default_settings_Win32",
	"Win32/Levels/MP_007/MP_007",
	"Win32/Levels/MP_Subway/MP_Subway",
	"Win32/Levels/MP_001/MP_001",
};

void visit(const EbxValue &p_value, std::vector<std::string> &r_sids) {
	switch (p_value.kind()) {
		case EbxValue::Kind::String: {
			const std::optional<std::string_view> text = p_value.as_str();
			if (text && is_sid(*text)) {
				r_sids.emplace_back(*text);
			}
			break;
		}
		case EbxValue::Kind::Struct:
			for (const EbxField &f : p_value.as_struct()->fields()) {
				visit(f.value, r_sids);
			}
			break;
		case EbxValue::Kind::Array:
			for (const EbxValue e : *p_value.as_array()) {
				visit(e, r_sids);
			}
			break;
		default:
			break;
	}
}

}

bool is_sid(std::string_view p_text) {
	if (p_text.size() < 3 || p_text.substr(0, 3) != "ID_") {
		return false;
	}
	return std::all_of(p_text.begin() + 3, p_text.end(), [](char p_c) { return (p_c >= 'A' && p_c <= 'Z') || (p_c >= '0' && p_c <= '9') || p_c == '_'; });
}

void collect_sids(const EbxPartition &p_partition, std::vector<std::string> &r_sids) {
	for (size_t i = 0; i < p_partition.instance_count(); ++i) {
		for (const EbxField &f : p_partition.instance(i)->fields()) {
			visit(f.value, r_sids);
		}
	}
}

std::span<const std::string_view> research_census_partitions() {
	return kCensusPartitions;
}

std::span<const std::string_view> research_census_superbundles() {
	return kCensusSuperbundles;
}

SidCensus sid_census(const EbxResolver &p_ebx, std::span<const std::string_view> p_partitions, const TextDatabase &p_db) {
	SidCensus census;
	for (const std::string_view name : p_partitions) {
		const EbxPartition *partition = p_ebx.partition_by_name(name);
		if (partition == nullptr) {
			census.missing_partitions.emplace_back(name);
			continue;
		}
		++census.partitions;
		collect_sids(*partition, census.sids);
	}
	std::sort(census.sids.begin(), census.sids.end());
	census.sids.erase(std::unique(census.sids.begin(), census.sids.end()), census.sids.end());
	for (const std::string &sid : census.sids) {
		const std::optional<std::string_view> text = p_db.find_sid(sid);
		if (!text) {
			++census.absent;
		} else if (*text == kTextMissingMarker) {
			++census.missing_marker;
		} else {
			++census.resolved;
		}
	}
	return census;
}

}

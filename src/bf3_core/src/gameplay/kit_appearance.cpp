// PT1 iff: the kit's default 3p appearance (kit_appearance.h; docs/design/pt1-iff.md §2.1). Every array scan and name
// copy is charged to the read's WorkBudget first (cost model: logic_world.cpp).
#include "bf3/core/gameplay/kit_appearance.h"

#include "bf3/core/ids.h"
#include "gameplay_read.h"

#include <vector>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

std::string_view tail_of(std::string_view p_path) {
	const size_t slash = p_path.find_last_of('/');
	return slash == std::string_view::npos ? p_path : p_path.substr(slash + 1);
}

} // namespace

bool appearance_matches_kit(std::string_view p_kit_asset, std::string_view p_appearance) {
	const std::string kit = to_lower_ascii(tail_of(p_kit_asset));
	const std::string app = to_lower_ascii(tail_of(p_appearance));
	// mp_<faction>_<role>_appearance<n>
	std::vector<std::string_view> tokens;
	std::string_view rest = app;
	for (size_t guard = 0; guard < 8 && !rest.empty(); ++guard) {
		const size_t us = rest.find('_');
		tokens.push_back(rest.substr(0, us));
		rest = us == std::string_view::npos ? std::string_view() : rest.substr(us + 1);
	}
	if (tokens.size() < 4 || tokens[0] != "mp" || tokens[1].empty() || tokens[2].empty() || tokens[3].substr(0, 10) != "appearance") {
		return false;
	}
	const std::string_view faction = tokens[1], role = tokens[2];
	if (kit.size() <= faction.size() || std::string_view(kit).substr(0, faction.size()) != faction) {
		return false;
	}
	const std::string_view kit_role = std::string_view(kit).substr(faction.size());
	return kit_role.size() >= role.size() && kit_role.substr(0, role.size()) == role;
}

Result<KitAppearance> read_kit_appearance(const EbxResolver &p_db, std::string_view p_kit_asset, WorkBudget *p_budget) {
	WorkBudget own_budget;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own_budget;
	BF3_TRY(budget.status());
	BF3_TRY(budget.charge_bytes(p_kit_asset.size()));
	KitAppearance out;
	out.kit = to_lower_ascii(p_kit_asset);
	const EbxPartition *partition = p_db.partition_by_name(out.kit);
	if (partition == nullptr) {
		return make_error("kit appearance: no partition `" + out.kit + "`");
	}
	const EbxInstance kit = partition->primary();
	BF3_TRY(budget.charge_type(kit.data()));
	if (!kit.is_a("VeniceSoldierCustomizationAsset")) {
		return make_error("kit appearance: `" + out.kit + "` is a " + std::string(kit.type_name()) + ", not a VeniceSoldierCustomizationAsset");
	}
	BF3_TRY(budget.charge(1));
	const std::optional<EbxInstance> table = p_db.follow(kit.data(), "VisualTable");
	if (!table) {
		return make_error("kit appearance: `" + out.kit + "` has no VisualTable");
	}
	BF3_TRY(budget.charge_type(table->data()));
	const std::vector<EbxInstance> parts = follow_all(budget, p_db, table->data(), "UnlockParts");
	BF3_TRY(budget.status());
	if (parts.empty()) {
		return make_error("kit appearance: `" + out.kit + "`: the VisualTable has no UnlockParts");
	}
	BF3_TRY(budget.charge_type(parts.front().data()));
	const std::vector<EbxInstance> camos = follow_all(budget, p_db, parts.front().data(), "SelectableUnlocks");
	BF3_TRY(budget.status());
	if (camos.empty()) {
		return make_error("kit appearance: `" + out.kit + "`: no visual unlock");
	}
	const EbxInstance &camo = camos.front();
	BF3_TRY(budget.charge_type(camo.data()));
	out.visual_unlock = partition_name(budget, p_db, camo);
	out.visual_unlock_guid = camo.guid();
	const std::vector<EbxInstance> links = follow_all(budget, p_db, camo.data(), "LinkedTo");
	BF3_TRY(budget.status());
	if (links.size() > kMaxKitVisualLinks) {
		return make_error("kit appearance: `" + out.visual_unlock + "` links " + std::to_string(links.size()) + " unlocks, more than " +
				std::to_string(kMaxKitVisualLinks));
	}
	out.candidates = links.size();
	size_t matches = 0;
	for (const EbxInstance &link : links) {
		const std::string name = partition_name(budget, p_db, link);
		BF3_TRY(budget.status());
		if (appearance_matches_kit(out.kit, name)) {
			if (matches == 0) {
				out.appearance = name;
			}
			++matches;
		}
	}
	if (matches != 1) {
		return make_error("kit appearance: `" + out.kit + "`: " + std::to_string(matches) + " of the " + std::to_string(links.size()) + " appearances `" +
				out.visual_unlock + "` links match the kit (need exactly 1)");
	}
	return out;
}

} // namespace bf3::core::gameplay

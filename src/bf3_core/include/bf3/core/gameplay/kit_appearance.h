#pragma once
// PT1 iff (docs/design/pt1-iff.md §2.1): the 3p appearance a kit's soldier wears by default, from the kit's data.
//
// A kit (VeniceSoldierCustomizationAsset, gameplay/kits/usassault) has a VisualTable whose first
// CustomizationUnlockParts lists the camo unlocks; the first is the default camo (ui/art/persistence/camo/
// u_camo_default, ungated by the progression tables [verified: ctest pt1.iff.install]). That unlock's LinkedTo names
// every faction's and kit's appearance UnlockAsset of that camo (Metro: the 8 mp_<us|ru>_<kit>_appearance01). The data
// has no field that says which of them belongs to the kit, so the kit's one is picked by the partition names
// [inferred]: kit gameplay/kits/<faction><role> takes persistence/.../mp_<faction>_<r>_appearance<n> where <role>
// starts with <r> (usengineer -> mp_us_engi_appearance01). Exactly one candidate has to match.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace bf3::core::gameplay {

inline constexpr size_t kMaxKitVisualLinks = 4096; // retail: 18 camos, 8 appearances per camo

struct KitAppearance {
	std::string kit; // gameplay/kits/usengineer (lowercase)
	std::string visual_unlock; // the default camo unlock partition
	Guid visual_unlock_guid{}; // its instance GUID (UnlockProgression::gate)
	std::string appearance; // the appearance UnlockAsset partition (load_soldier_assets' SoldierTeamSpec::appearance)
	size_t candidates = 0; // LinkedTo entries of the camo unlock
};

// The naming rule above (lowercase compare; any path prefix). False for names that do not have the shapes.
bool appearance_matches_kit(std::string_view p_kit_asset, std::string_view p_appearance);

// Fails when the kit, its VisualTable or its first visual unlock does not resolve, or not exactly one LinkedTo entry
// matches the kit. p_budget null: a default WorkBudget.
Result<KitAppearance> read_kit_appearance(const EbxResolver &p_db, std::string_view p_kit_asset, WorkBudget *p_budget = nullptr);

} // namespace bf3::core::gameplay

#pragma once
// The HUD data-source table, generated from the install (plan §3.8 "The data model", D16; research/topics/
// gap-r1-ui-hud-minimap-localisation.md §5; docs/design/ui-minimap-icons.md §7). Godot-free.
//
// BF3's HUD reads named signals: every UI component type (UIComponentData and its subtypes UI*CompData,
// HudCameraShakeData, UserComponentData, ...) has one partition under ui/uicomponents/ whose primary instance lists
// DataSources (CString names, e.g. UIWeaponCompData: ActiveWeaponAmmo, ActiveWeaponClipCapacity, ...), UpdateType
// (UIUpdateType_Always/Odd/Even/Never) and UpdatesPerSecond. ui/flow/state/ingamestatemp (UIStateAsset, Win32/UI)
// lists the 58 components the in-game MP state runs (UIComponents).
//
// Verified 2026-10-15 (design §4): 84 partitions under ui/uicomponents/ over "Win32/UI,@all-mp", 84 distinct
// primary types; the 58 in-game ones all resolve in Win32/UI. Source counts: UIWeaponCompData 63, UISoldierCompData
// 16, UIVehicleCompData 82, UISettingsCompData 89, UICustomizationCompData 59, UIEndOfRoundCompData 52, ... (0 for
// the 8 components that bind nothing).
//
// The repo never stores this table (it is BF3 data): it is generated at run time (bf3dump hud-sources, the ctests),
// never cached. The repo holds only hud_model.h's binding rows, the names the HudModel
// binds, which the install test checks against this table in both directions.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::core::ui {

inline constexpr std::string_view kInGameStatePartition = "ui/flow/state/ingamestatemp";
inline constexpr std::string_view kHudComponentPrefix = "ui/uicomponents/";
// The superbundles the table is read from: Win32/UI first (it carries the in-game components), then every MP level
// (a few front-end and mode components live only there); the first superbundle that holds a name wins.
inline constexpr std::string_view kHudSourceSuperbundles = "Win32/UI,@all-mp";

inline constexpr size_t kMaxHudComponentTypes = 1024; // retail 84
inline constexpr size_t kMaxHudSourcesPerType = 4096; // retail at most 89
inline constexpr size_t kMaxHudStateComponents = 1024; // retail 58

struct HudComponentType {
	std::string type; // primary instance type, e.g. UIWeaponCompData
	std::string partition; // ui/uicomponents/uiweaponcomp
	std::string update_type; // UIUpdateType_Even
	int32_t updates_per_second = 0; // -1 = every frame
	std::vector<std::string> sources; // DataSources, data order
	Sha1 sha1{}; // manifest SHA-1 of the partition (printed by bf3dump hud-sources: which data the table came from)
};

struct HudSourceTable {
	uint32_t version = 0; // kHudSourceTableVersion of the code that built it
	std::string state_partition; // ui/flow/state/ingamestatemp
	std::vector<HudComponentType> types; // sorted by type name
	std::vector<uint32_t> state_components; // UIComponents of the state, in order: indices into types
	Sha1 state_sha1{};

	const HudComponentType *find(std::string_view p_type) const;
	size_t source_count() const;
};

// Names under kHudComponentPrefix in the mounts' bundle manifests (no partition is parsed), first mount wins, sorted.
std::vector<std::string> hud_component_partitions(std::span<const LevelMount *const> p_mounts);

// Reads the state's UIComponents and every listed component partition. Fails when a partition does not resolve, a
// primary type repeats, the state names a component outside the list, or a cap is passed. p_budget null: a default
// WorkBudget.
Result<HudSourceTable> read_hud_source_table(const EbxResolver &p_ui, std::span<const std::string> p_component_partitions,
		gameplay::WorkBudget *p_budget = nullptr, std::string_view p_state_partition = kInGameStatePartition);

// The table is built only by bf3dump hud-sources and ctest ui.hud.sources (the game binds kHudBindings, never the
// table), so it is not cached (design §8). version lets a TSV say which code wrote it.
inline constexpr uint32_t kHudSourceTableVersion = 1;

// TSV for bf3dump hud-sources: "type  partition  update_type  updates_per_second  in_state  count  sources(,)".
std::string format_hud_source_table(const HudSourceTable &p_table);

} // namespace bf3::core::ui

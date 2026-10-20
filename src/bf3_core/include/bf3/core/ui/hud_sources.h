#pragma once

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

inline constexpr std::string_view kHudSourceSuperbundles = "Win32/UI,@all-mp";

inline constexpr size_t kMaxHudComponentTypes = 1024;
inline constexpr size_t kMaxHudSourcesPerType = 4096;
inline constexpr size_t kMaxHudStateComponents = 1024;

struct HudComponentType {
	std::string type;
	std::string partition;
	std::string update_type;
	int32_t updates_per_second = 0;
	std::vector<std::string> sources;
	Sha1 sha1{};
};

struct HudSourceTable {
	uint32_t version = 0;
	std::string state_partition;
	std::vector<HudComponentType> types;
	std::vector<uint32_t> state_components;
	Sha1 state_sha1{};

	const HudComponentType *find(std::string_view p_type) const;
	size_t source_count() const;
};

std::vector<std::string> hud_component_partitions(std::span<const LevelMount *const> p_mounts);

Result<HudSourceTable> read_hud_source_table(const EbxResolver &p_ui, std::span<const std::string> p_component_partitions,
		gameplay::WorkBudget *p_budget = nullptr, std::string_view p_state_partition = kInGameStatePartition);

inline constexpr uint32_t kHudSourceTableVersion = 1;

std::string format_hud_source_table(const HudSourceTable &p_table);

}

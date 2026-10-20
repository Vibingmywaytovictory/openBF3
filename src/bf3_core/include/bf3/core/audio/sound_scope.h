#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::audio {

inline constexpr uint16_t kNoScope = 0xFFFF;
inline constexpr size_t kMaxScopes = 1024;
inline constexpr uint32_t kMaxScopeCount = 1024;
inline constexpr std::string_view kAudioSystemPartition = "sound/audiosystem";

struct SoundScope {
	std::string name;
	uint16_t closest = 0;
	uint16_t max_instances = 0;
	uint16_t latest = 0;
	float duplicate_radius_m = 0.0f;
	float duplicate_time_s = 0.0f;

	Guid guid;
	std::string strategy;
	bool group_types = false;
};

struct ScopeTable {
	std::vector<SoundScope> scopes;
	std::vector<std::pair<Guid, uint16_t>> patch_scopes;

	uint16_t scope_of(const Guid &p_patch) const;

	uint16_t scope_index(const Guid &p_scope) const;
};

Result<ScopeTable> read_scopes(const EbxResolver &p_db, gameplay::WorkBudget *p_budget = nullptr);

Status map_patch_scopes(const EbxResolver &p_db, std::span<const Guid> p_patches, ScopeTable &r_table, gameplay::WorkBudget *p_budget = nullptr,
		size_t *r_skipped = nullptr);

}

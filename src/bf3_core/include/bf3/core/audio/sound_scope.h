#pragma once
// The audio system's voice scopes (docs/design/m2b.md §6.2, §2.6 #14; declared in W1.0, owner au). Read-only on the
// install, Godot-free. 33 SoundScopeData strategies [verified: gap-r1-audio-system.md]: e.g. Bullet_Impact
// DuplicateSpawn 1 m / 0.4 s, keep the closest 5; Explosion closest 10; Death_Scream max 4; UI latest 1. 811 of 1,295
// patches name no scope (every handheld weapon and the footsteps): their only limit is the HDR culling and the cap.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bf3::core::audio {

inline constexpr uint16_t kNoScope = 0xFFFF;

// One scope's strategy; 0 = that limit is off. Several can hold at once (Bullet_Impact: duplicates, then closest 5).
struct SoundScope {
	std::string name; // e.g. "Bullet_Impact_Scope"
	uint16_t closest = 0; // keep the N closest to the listener
	uint16_t max_instances = 0; // at most N
	uint16_t latest = 0; // keep the N newest
	float duplicate_radius_m = 0.0f; // DuplicateSpawn: a new one within this radius ...
	float duplicate_time_s = 0.0f; // ... and this time of a playing one is dropped
};

struct ScopeTable {
	std::vector<SoundScope> scopes; // index = scope index
	std::vector<std::pair<Guid, uint16_t>> patch_scopes; // patch partition -> scope index, sorted by Guid
	// The patch's scope index, or kNoScope.
	uint16_t scope_of(const Guid &p_patch) const;
};

// The install's scopes and the patch -> scope map (au): every read charged to the budget.
Result<ScopeTable> read_scopes(const EbxResolver &p_db, gameplay::WorkBudget *p_budget = nullptr);

} // namespace bf3::core::audio

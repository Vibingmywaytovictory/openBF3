#pragma once
// The audio system's voice scopes (docs/design/m2b.md §6.2, §2.6 #14; declared in W1.0, owner au). Read-only on the
// install, Godot-free. 33 SoundScopeData [verified 2026-10-19: bf3dump dump of sound/audiosystem on MP_Subway], each
// with one DefaultStrategy of three types:
//   ClosestInstancesScopeStrategyData {Count}            -> closest = Count (keep the N closest to the listener);
//   MaxInstancesScopeStrategyData {Count, KeepOldest}    -> KeepOldest ? max_instances = Count (a new one over the
//                                                           count is refused) : latest = Count (the oldest goes);
//   DuplicateSpawnScopeStrategyData {Distance, Time, ClosestCount, GroupTypes}
//                                                        -> duplicate_radius_m / duplicate_time_s, closest = ClosestCount.
// e.g. Bullet_Impact_Scope: duplicates within 1 m / 0.4 s, then the closest 5; Explosion_Scope: the closest 10;
// Death_Scream_Scope: Max_Instaces_4 (KeepOldest false: the newest 4); UI_Scope: Latest_1. GroupTypes (true on two
// DuplicateSpawn strategies) has no known meaning: read, not applied, measured = false. A patch names its scope in
// SoundPatchAsset.Scope (a reference into sound/audiosystem); 811 of 1,295 patches name none (every handheld weapon
// and the footsteps): their only limits are the HDR culling and the cap.

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
inline constexpr size_t kMaxScopes = 1024; // hostile data: more is an error
inline constexpr uint32_t kMaxScopeCount = 1024; // a Count above it is an error
inline constexpr std::string_view kAudioSystemPartition = "sound/audiosystem";

// One scope's strategy; 0 = that limit is off. Several can hold at once (Bullet_Impact: duplicates, then closest 5).
struct SoundScope {
	std::string name; // e.g. "Bullet_Impact_Scope"
	uint16_t closest = 0; // keep the N closest to the listener
	uint16_t max_instances = 0; // at most N (the oldest stay, a new one is refused)
	uint16_t latest = 0; // keep the N newest
	float duplicate_radius_m = 0.0f; // DuplicateSpawn: a new one within this radius ...
	float duplicate_time_s = 0.0f; // ... and this time of a playing one is dropped
	// As built (au): the SoundScopeData instance (what SoundPatchAsset.Scope references) and the strategy's name.
	Guid guid;
	std::string strategy;
	bool group_types = false; // DuplicateSpawn GroupTypes: read, not applied (measured = false)
};

struct ScopeTable {
	std::vector<SoundScope> scopes; // index = scope index
	std::vector<std::pair<Guid, uint16_t>> patch_scopes; // patch partition -> scope index, sorted by Guid
	// The patch's scope index, or kNoScope.
	uint16_t scope_of(const Guid &p_patch) const;
	// As built (au): the scope index of a SoundScopeData instance, or kNoScope.
	uint16_t scope_index(const Guid &p_scope) const;
};

// The install's scopes (au): AudioSystemAsset.Scopes in sound/audiosystem, every instance examined charged to the
// budget (a local WorkBudget::kDefaultLimit budget when null). patch_scopes stays empty: map_patch_scopes fills it for
// the patches a level plays (an EbxResolver cannot list partitions by type).
Result<ScopeTable> read_scopes(const EbxResolver &p_db, gameplay::WorkBudget *p_budget = nullptr);

// As built (au): reads SoundPatchAsset.Scope of each patch partition in p_patches (the compiled programs'
// SoundProgram::partition) and adds the ones that name a scope of r_table to patch_scopes (sorted, duplicates kept
// once). A patch partition that is absent, or whose primary instance is not a sound patch, is skipped and counted in
// r_skipped. Every partition examined is charged.
Status map_patch_scopes(const EbxResolver &p_db, std::span<const Guid> p_patches, ScopeTable &r_table, gameplay::WorkBudget *p_budget = nullptr,
		size_t *r_skipped = nullptr);

} // namespace bf3::core::audio

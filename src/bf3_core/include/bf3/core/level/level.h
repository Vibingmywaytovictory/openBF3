#pragma once
// One level x mode loaded for traversal: the level superbundle mounted read-only, the bundles the
// mode includes, and every partition of them parsed into one cross-referenced EbxDb. Ported from
// IW4L level_bf3 (level.rs, mode.rs, info.rs), Apache-2.0.
//
// Bundle selection (handoff sections/03 §1): the level bundle `levels/<id>/<id>`, then its
// `_settings_win32` and `_gameconfiglight_win32` bundles (the settings carry the inclusion
// criterion), then breadth-first every sub-world the mode includes: a SubWorldReferenceObjectData
// (found among the level's objects and, up to 8 layers deep, its WorldPartData layers) is included
// iff AutoLoad and the mode is in EnabledOptions of every GameMode inclusion setting (no settings =
// included). A sub-world bundle the superbundle lacks is recorded as absent (Metro has no
// Conquest_Large: ConquestLarge0 = Conquest_Small + Teams_Large).
// The load charges its sub-world search to the level's WorkBudget (budget.h), which the traversal
// and the helpers run on its World then keep charging.

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::level {

// A GameMode option id as EnabledOptions spells it (ConquestLarge0, TeamDeathMatch0, ...); compared
// case-insensitively. The shipped levels use ConquestLarge0, ConquestSmall0, ConquestAssaultLarge0,
// ConquestAssaultSmall0, RushLarge0, SquadRush0, TeamDeathMatch0, TeamDeathMatchC0,
// SquadDeathMatch0, Domination0, GunMaster0, TankSuperiority0, Scavenger0, CaptureTheFlag0 and
// AirSuperiority0.
bool mode_matches(std::string_view p_mode, std::string_view p_option);

// The 29 multiplayer levels' display names (handoff §9); p_level_id itself for another id.
std::string_view level_display_name(std::string_view p_level_id);
// `MP_Subway` -> `mp_subway`, `XP2_Palace` -> `mp_xp2_palace` (the IW4L zone stem).
std::string zone_name(std::string_view p_level_id);

// UILevelDescriptionComponent.MinimapData, Frostbite metres (X, Z).
struct Minimap {
	std::array<float, 2> world_center{};
	float world_size = 0.0f;
	float world_rotation = 0.0f;
	float world_range = 0.0f;
};

struct LevelInfo {
	std::string id; // as the level partition spells it, e.g. MP_Subway
	std::string zone;
	std::string display_name;
	std::string name_sid; // LevelDescription.Name, e.g. ID_M_LEVELNAME_MP_SUBWAY
	std::string description;
	float world_size_xz = 0.0f;
	std::vector<std::string> modes; // LevelDescriptionAsset categories "GameMode"
	std::optional<Minimap> minimap;
};

// The bundles a level and mode load, as the mount names them (toc ids).
struct BundleSet {
	std::string level;
	std::vector<std::string> settings;
	std::vector<std::string> subworlds; // included sub-worlds, nested ones after their parent
	std::vector<std::string> absent; // BundleNames the mode includes but the superbundle lacks
};

struct PartitionStats {
	size_t partitions = 0;
	size_t duplicates = 0; // partitions whose GUID or name another name already brought in
	std::vector<std::pair<std::string, std::string>> failed; // (name, error)
	size_t imports = 0;
	size_t unresolved_imports = 0;
	double bundle_seconds = 0.0;
	double parse_seconds = 0.0;
};

struct LoadOptions {
	unsigned threads = 0; // parse lanes; 0 = all cores
};

class LoadedLevel;

// Mounts p_level_id (MP_Subway, XP2_Palace, ...) read-only, selects the bundles of p_mode and
// parses their partitions. Fails for an unknown level, a level partition that is not LevelData, or
// a sub-world graph that exhausts the work budget.
Result<std::unique_ptr<LoadedLevel>> load_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const LoadOptions &p_options = {});

class LoadedLevel {
public:
	explicit LoadedLevel(LevelMount p_mount) :
			mount_(std::move(p_mount)) {}
	LoadedLevel(const LoadedLevel &) = delete;
	LoadedLevel &operator=(const LoadedLevel &) = delete;

	const LevelMount &mount() const { return mount_; }
	const EbxDb &db() const { return db_; }
	// The LevelData instance (primary of `levels/<id>/<id>`).
	EbxInstance root() const;

	std::string mode;
	LevelInfo info;
	BundleSet bundles;
	PartitionStats stats;
	std::string root_name; // levels/<id>/<id>
	// The work budget of this level: the load charged it, World::traverse(*this) and the helpers on
	// that World charge it on.
	std::shared_ptr<WorkBudget> budget = std::make_shared<WorkBudget>();

private:
	friend Result<std::unique_ptr<LoadedLevel>> load_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id,
			std::string_view p_mode, const LoadOptions &p_options);
	LevelMount mount_;
	EbxDb db_;
};

// The description of a level (names, modes, minimap) from its level and settings bundles only.
Result<LevelInfo> level_info(const Install &p_install, std::string_view p_level_id);

// Whether a SubWorldReferenceObjectData loads in p_mode (see the header comment). A setting whose
// criterion is not loaded counts as a GameMode one, the only criterion levels use. The Settings and
// each EnabledOptions list are charged to r_budget (raw length) before they are scanned.
Result<bool> includes(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, WorkBudget &r_budget);

// Every SubWorldReferenceObjectData among p_container's Objects and, through world parts, its
// layers (8 deep, at most 2^16 containers per search), in object order. Each container visited
// costs 1 and the raw length of its Objects, charged before they are resolved.
Status subworld_references(const EbxResolver &p_db, const EbxInstance &p_container, WorkBudget &r_budget, std::vector<EbxInstance> &r_out);

// LevelInfo of a loaded root (the level partition and its description_win32 must be in p_db). Each
// list it scans is charged to r_budget first.
Result<LevelInfo> read_level_info(const EbxResolver &p_db, const EbxInstance &p_root, std::string_view p_root_name, std::string_view p_level_id,
		WorkBudget &r_budget);

} // namespace bf3::core::level

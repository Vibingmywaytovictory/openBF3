#pragma once

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

bool mode_matches(std::string_view p_mode, std::string_view p_option);

std::vector<std::string_view> mode_alias_ids(std::string_view p_mode);

Result<std::string> resolve_level_mode(const std::vector<std::string> &p_level_modes, std::string_view p_mode);

std::string_view level_display_name(std::string_view p_level_id);

std::vector<std::string_view> shipped_level_ids();

std::string zone_name(std::string_view p_level_id);

struct Minimap {
	std::array<float, 2> world_center{};
	float world_size = 0.0f;
	float world_rotation = 0.0f;
	float world_range = 0.0f;
};

inline constexpr size_t kMaxLoadingHints = 1024;

struct LoadingScreenInfo {
	std::string movie;
	std::string image_path;
	std::string music_path;
	EbxRef hint_asset;
	std::vector<std::string> hints;
};

struct LevelInfo {
	std::string id;
	std::string zone;
	std::string display_name;
	std::string name_sid;
	std::string description;
	bool multiplayer = false;
	bool coop = false;
	float world_size_xz = 0.0f;
	std::vector<std::string> modes;
	std::optional<Minimap> minimap;
	LoadingScreenInfo loading;
};

struct BundleSet {
	std::string level;
	std::vector<std::string> settings;
	std::vector<std::string> subworlds;
	std::vector<std::string> absent;
};

struct PartitionStats {
	size_t partitions = 0;
	size_t duplicates = 0;
	std::vector<std::pair<std::string, std::string>> failed;
	size_t imports = 0;
	size_t unresolved_imports = 0;
	double bundle_seconds = 0.0;
	double parse_seconds = 0.0;
};

struct LoadOptions {
	unsigned threads = 0;
};

class LoadedLevel;

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

	EbxInstance root() const;

	std::string mode;
	LevelInfo info;
	BundleSet bundles;
	PartitionStats stats;
	std::string root_name;

	std::shared_ptr<WorkBudget> budget = std::make_shared<WorkBudget>();

private:
	friend Result<std::unique_ptr<LoadedLevel>> load_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id,
			std::string_view p_mode, const LoadOptions &p_options);
	LevelMount mount_;
	EbxDb db_;
};

Result<LevelInfo> level_info(const Install &p_install, std::string_view p_level_id);

Result<LevelMount> mount_level_description(const Install &p_install, std::string_view p_level_id);

Result<LevelInfo> level_info(const LevelMount &p_mount, std::string_view p_level_id);

Result<std::vector<std::string>> level_hints(const LevelMount &p_mount, const EbxRef &p_asset);

Status read_game_tips(const EbxInstance &p_asset, WorkBudget &r_budget, std::vector<std::string> &r_out);

Status read_description_modes(const EbxInstance &p_asset, WorkBudget &r_budget, std::vector<std::string> &r_out);

Result<bool> includes(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, WorkBudget &r_budget);

Status subworld_references(const EbxResolver &p_db, const EbxInstance &p_container, WorkBudget &r_budget, std::vector<EbxInstance> &r_out);

Result<LevelInfo> read_level_info(const EbxResolver &p_db, const EbxInstance &p_root, std::string_view p_root_name, std::string_view p_level_id,
		WorkBudget &r_budget);

}

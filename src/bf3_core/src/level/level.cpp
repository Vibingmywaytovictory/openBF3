#include "bf3/core/level/level.h"

#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"

#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace bf3::core::level {

namespace {

constexpr size_t kMaxModes = 4096;

constexpr uint32_t kLayerDepth = 8;

constexpr size_t kLayerBudget = size_t(1) << 16;

struct DisplayName {
	std::string_view id;
	std::string_view name;
};

struct ModeAlias {
	std::string_view alias;
	std::array<std::string_view, 2> ids;
};

constexpr ModeAlias kModeAliases[] = {
	{ "CQL", { "ConquestLarge0" } },
	{ "CQS", { "ConquestSmall0" } },
	{ "CQAL", { "ConquestAssaultLarge0" } },
	{ "CQAS", { "ConquestAssaultSmall0" } },
	{ "RL", { "RushLarge0" } },
	{ "SR", { "SquadRush0" } },
	{ "SQR", { "SquadRush0" } },
	{ "TDM", { "TeamDeathMatch0", "TeamDeathMatchC0" } },
	{ "TDMC", { "TeamDeathMatchC0" } },
	{ "SDM", { "SquadDeathMatch0" } },
	{ "SQDM", { "SquadDeathMatch0" } },
	{ "DOM", { "Domination0" } },
	{ "GM", { "GunMaster0" } },
	{ "TS", { "TankSuperiority0" } },
	{ "SCV", { "Scavenger0" } },
	{ "CTF", { "CaptureTheFlag0" } },
	{ "AS", { "AirSuperiority0" } },
};

constexpr DisplayName kDisplayNames[29] = {
	{ "MP_001", "Grand Bazaar" },
	{ "MP_003", "Tehran Highway" },
	{ "MP_007", "Caspian Border" },
	{ "MP_011", "Seine Crossing" },
	{ "MP_012", "Operation Firestorm" },
	{ "MP_013", "Damavand Peak" },
	{ "MP_017", "Noshahr Canals" },
	{ "MP_018", "Kharg Island" },
	{ "MP_Subway", "Operation Metro" },
	{ "XP1_001", "Strike at Karkand" },
	{ "XP1_002", "Gulf of Oman" },
	{ "XP1_003", "Sharqi Peninsula" },
	{ "XP1_004", "Wake Island" },
	{ "XP2_Factory", "Scrapmetal" },
	{ "XP2_Office", "Operation 925" },
	{ "XP2_Palace", "Donya Fortress" },
	{ "XP2_Skybar", "Ziba Tower" },
	{ "XP3_Alborz", "Alborz Mountains" },
	{ "XP3_Desert", "Bandar Desert" },
	{ "XP3_Shield", "Armored Shield" },
	{ "XP3_Valley", "Death Valley" },
	{ "XP4_FD", "Markaz Monolith" },
	{ "XP4_Parl", "Azadi Palace" },
	{ "XP4_Quake", "Epicenter" },
	{ "XP4_Rubble", "Talah Market" },
	{ "XP5_001", "Operation Riverside" },
	{ "XP5_002", "Nebandan Flats" },
	{ "XP5_003", "Kiasar Railroad" },
	{ "XP5_004", "Sabalan Pipeline" },
};

Status search_layers(const EbxResolver &p_db, const EbxInstance &p_container, uint32_t p_depth, size_t &r_containers, WorkBudget &r_budget,
		std::vector<EbxInstance> &r_out) {
	if (r_containers == 0) {
		return Status::success();
	}
	--r_containers;
	BF3_TRY(r_budget.charge(1));
	std::vector<EbxInstance> objects;
	BF3_TRY(charged_follow_all(p_db, p_container.data(), "Objects", r_budget, objects));
	for (const EbxInstance &object : objects) {
		if (object.is_a("SubWorldReferenceObjectData")) {
			r_out.push_back(object);
		} else if (object.is_a("WorldPartReferenceObjectData") && p_depth < kLayerDepth) {
			if (const std::optional<EbxInstance> part = p_db.follow(object.data(), "Blueprint")) {
				BF3_TRY(search_layers(p_db, *part, p_depth + 1, r_containers, r_budget, r_out));
			}
		}
	}
	return Status::success();
}

std::string join_names(const std::vector<std::string> &p_names) {
	std::string out = "[";
	for (size_t i = 0; i < p_names.size(); ++i) {
		out += (i ? ", " : "") + p_names[i];
	}
	return out + "]";
}

class Loader {
public:
	Loader(const LevelMount &p_mount, EbxDb &p_db, PartitionStats &p_stats, unsigned p_threads) :
			mount_(p_mount), db_(p_db), stats_(p_stats), threads_(p_threads) {}

	Status load(const std::vector<std::string> &p_bundles) {
		if (p_bundles.empty()) {
			return Status::success();
		}
		auto started = std::chrono::steady_clock::now();
		const Status loaded = mount_.load_bundles(p_bundles);
		if (!loaded) {
			return error_at("bundles " + join_names(p_bundles), loaded.error());
		}
		stats_.bundle_seconds += seconds_since(started);

		started = std::chrono::steady_clock::now();
		std::vector<std::string> names;
		for (std::string &name : mount_.ebx_names()) {
			if (parsed_.insert(to_lower_ascii(name)).second) {
				names.push_back(std::move(name));
			}
		}
		std::vector<Result<std::unique_ptr<EbxPartition>>> partitions = parallel_map<Result<std::unique_ptr<EbxPartition>>>(
				names.size(),
				[&](size_t i) -> Result<std::unique_ptr<EbxPartition>> {
					Result<std::vector<uint8_t>> bytes = mount_.read_ebx(names[i]);
					if (!bytes) {
						return bytes.error();
					}
					Result<EbxPartition> partition = EbxPartition::parse(std::move(*bytes));
					if (!partition) {
						return partition.error();
					}
					return std::make_unique<EbxPartition>(std::move(*partition));
				},
				threads_);
		for (size_t i = 0; i < names.size(); ++i) {
			if (!partitions[i]) {
				stats_.failed.emplace_back(names[i], partitions[i].error().message);
			} else if (db_.insert(names[i], std::move(*partitions[i]))) {
				++stats_.partitions;
			} else {
				++stats_.duplicates;
			}
		}
		stats_.parse_seconds += seconds_since(started);
		return Status::success();
	}

	void count_imports() {
		size_t imports = 0, unresolved = 0;
		for (size_t i = 0; i < db_.size(); ++i) {
			for (const EbxImport &import : db_.partition_at(i).imports()) {
				++imports;
				if (!db_.instance(import.partition, import.instance)) {
					++unresolved;
				}
			}
		}
		stats_.imports = imports;
		stats_.unresolved_imports = unresolved;
	}

private:
	const LevelMount &mount_;
	EbxDb &db_;
	PartitionStats &stats_;
	unsigned threads_;
	std::unordered_set<std::string> parsed_;
};

std::optional<std::string> find_bundle(const std::vector<std::string> &p_names, const std::string &p_key) {
	for (const std::string &name : p_names) {
		if (bundle_key(name) == p_key) {
			return name;
		}
	}
	return std::nullopt;
}

class BundleIndex {
public:
	explicit BundleIndex(const std::vector<std::string> &p_names) {
		for (const std::string &name : p_names) {
			by_key_.emplace(bundle_key(name), name);
		}
	}

	std::optional<std::string> find(const std::string &p_key) const {
		const auto found = by_key_.find(p_key);
		return found == by_key_.end() ? std::nullopt : std::optional<std::string>(found->second);
	}

private:
	std::unordered_map<std::string, std::string> by_key_;
};

std::optional<Minimap> read_minimap(const EbxInstance &p_component) {
	const std::optional<EbxValue> data = p_component.get("MinimapData");
	if (!data) {
		return std::nullopt;
	}
	const auto number = [&](std::string_view p_field) -> std::optional<float> {
		const std::optional<EbxValue> v = data->get(p_field);
		return v ? v->as_f32() : std::nullopt;
	};
	const std::optional<EbxValue> center = data->get("WorldCenter");
	if (!center) {
		return std::nullopt;
	}
	const std::optional<EbxValue> x = center->get("x");
	const std::optional<EbxValue> y = center->get("y");
	const std::optional<float> cx = x ? x->as_f32() : std::nullopt;
	const std::optional<float> cy = y ? y->as_f32() : std::nullopt;
	const std::optional<float> size = number("WorldSize");
	if (!cx || !cy || !size) {
		return std::nullopt;
	}
	Minimap m;
	m.world_center = { *cx, *cy };
	m.world_size = *size;
	m.world_rotation = number("WorldRotation").value_or(0.0f);
	m.world_range = number("WorldRange").value_or(0.0f);
	return m;
}

Status read_loading_screen(
		const EbxResolver &p_db, const EbxInstance &p_component, WorkBudget &r_budget, LoadingScreenInfo &r_out) {
	const auto text = [&](std::string_view p_field, std::string &r_text) -> Status {
		const Result<std::optional<std::string_view>> s = charged_str(p_component.data(), p_field, r_budget);
		if (!s) {
			return s.error();
		}
		r_text = std::string(s->value_or(""));
		return Status::success();
	};
	BF3_TRY(text("MPLoadingAssetPath", r_out.movie));
	BF3_TRY(text("LoadingImagePath", r_out.image_path));
	BF3_TRY(text("LoadingMusicPath", r_out.music_path));
	const std::optional<EbxValue> hint = p_component.get("HintAsset");
	r_out.hint_asset = (hint ? hint->as_ref() : std::nullopt).value_or(EbxRef::null());
	if (const std::optional<EbxInstance> tips = p_db.follow(p_component.data(), "HintAsset")) {
		BF3_TRY(read_game_tips(*tips, r_budget, r_out.hints));
	}
	return Status::success();
}

}

Status read_game_tips(const EbxInstance &p_asset, WorkBudget &r_budget, std::vector<std::string> &r_out) {
	if (!p_asset.is_a("GameTipAsset")) {
		return make_error("hint asset is a " + std::string(p_asset.type_name()) + ", not a GameTipAsset");
	}
	const std::optional<EbxValue> tips = p_asset.get("Tips");
	const OptionalEbxArray array = tips ? tips->as_array() : OptionalEbxArray();
	if (!array) {
		return Status::success();
	}
	if (array->size() > kMaxLoadingHints) {
		return make_error("GameTipAsset lists " + std::to_string(array->size()) + " tips, more than " +
				std::to_string(kMaxLoadingHints));
	}
	BF3_TRY(r_budget.charge(array->size()));
	r_out.clear();
	for (const EbxValue tip : *array) {
		const Result<std::optional<std::string_view>> s = charged_str(tip.get("Text"), r_budget);
		if (!s) {
			return s.error();
		}
		if (*s && !(*s)->empty()) {
			r_out.emplace_back(**s);
		}
	}
	return Status::success();
}

bool mode_matches(std::string_view p_mode, std::string_view p_option) {
	return iequals_ascii(p_mode, p_option);
}

std::vector<std::string_view> mode_alias_ids(std::string_view p_mode) {
	for (const ModeAlias &a : kModeAliases) {
		if (iequals_ascii(p_mode, a.alias)) {
			std::vector<std::string_view> out;
			for (const std::string_view id : a.ids) {
				if (!id.empty()) {
					out.push_back(id);
				}
			}
			return out;
		}
	}
	return { p_mode };
}

Result<std::string> resolve_level_mode(const std::vector<std::string> &p_level_modes, std::string_view p_mode) {
	for (const std::string_view wanted : mode_alias_ids(p_mode)) {
		for (const std::string &listed : p_level_modes) {
			if (iequals_ascii(listed, wanted)) {
				return listed;
			}
		}
	}
	std::string listed;
	for (const std::string &m : p_level_modes) {
		listed += (listed.empty() ? "" : ", ") + m;
	}
	return make_error("no game mode `" + std::string(p_mode) + "` (the level lists " +
			(listed.empty() ? std::string("none") : listed) + ")");
}

Status read_description_modes(const EbxInstance &p_asset, WorkBudget &r_budget, std::vector<std::string> &r_out) {
	const std::optional<EbxValue> categories = p_asset.get("Categories");
	const OptionalEbxArray array = categories ? categories->as_array() : OptionalEbxArray();
	if (!array) {
		return Status::success();
	}
	BF3_TRY(r_budget.charge(array->size()));
	for (const EbxValue category : *array) {
		const Result<std::optional<std::string_view>> kind_name = charged_str(category.get("Category"), r_budget);
		if (!kind_name) {
			return kind_name.error();
		}
		if (!*kind_name || !iequals_ascii(**kind_name, "GameMode")) {
			continue;
		}
		const std::optional<EbxValue> modes = category.get("Mode");
		const OptionalEbxArray mode_array = modes ? modes->as_array() : OptionalEbxArray();
		if (!mode_array) {
			continue;
		}
		BF3_TRY(r_budget.charge(mode_array->size()));
		for (const EbxValue mode : *mode_array) {
			const Result<std::optional<std::string_view>> m = charged_str(mode, r_budget);
			if (!m) {
				return m.error();
			}
			if (!*m) {
				continue;
			}
			if (r_out.size() >= kMaxModes) {
				return make_error("level description lists more than " + std::to_string(kMaxModes) + " game modes");
			}
			r_out.emplace_back(**m);
		}
	}
	return Status::success();
}

std::string_view level_display_name(std::string_view p_level_id) {
	for (const DisplayName &d : kDisplayNames) {
		if (iequals_ascii(d.id, p_level_id)) {
			return d.name;
		}
	}
	return p_level_id;
}

std::vector<std::string_view> shipped_level_ids() {
	std::vector<std::string_view> out;
	for (const DisplayName &d : kDisplayNames) {
		out.push_back(d.id);
	}
	return out;
}

std::string zone_name(std::string_view p_level_id) {
	const std::string lower = to_lower_ascii(p_level_id);
	return lower.rfind("mp_", 0) == 0 ? lower : "mp_" + lower;
}

EbxInstance LoadedLevel::root() const {
	return db_.partition_by_name(root_name)->primary();
}

Status subworld_references(const EbxResolver &p_db, const EbxInstance &p_container, WorkBudget &r_budget, std::vector<EbxInstance> &r_out) {
	size_t containers = kLayerBudget;
	return search_layers(p_db, p_container, 0, containers, r_budget, r_out);
}

Result<bool> includes(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, WorkBudget &r_budget) {
	const std::optional<EbxValue> auto_load = p_reference.get("AutoLoad");
	if (!(auto_load ? auto_load->as_bool() : std::nullopt).value_or(false)) {
		return false;
	}
	const std::optional<EbxInstance> settings = p_db.follow(p_reference.data(), "InclusionSettings");
	if (!settings) {
		return true;
	}
	std::vector<EbxInstance> setting_list;
	BF3_TRY(charged_follow_all(p_db, settings->data(), "Settings", r_budget, setting_list));
	for (const EbxInstance &setting : setting_list) {
		const std::optional<EbxInstance> criterion = p_db.follow(setting.data(), "Criterion");
		std::optional<std::string_view> name;
		if (criterion) {
			const Result<std::optional<std::string_view>> text = charged_str(criterion->data(), "Name", r_budget);
			if (!text) {
				return text.error();
			}
			name = *text;
		}
		if (name && !iequals_ascii(*name, "GameMode")) {
			continue;
		}
		bool enabled = false;
		const std::optional<EbxValue> options = setting.get("EnabledOptions");
		const OptionalEbxArray array = options ? options->as_array() : OptionalEbxArray();
		if (array) {
			BF3_TRY(r_budget.charge(array->size()));
			for (const EbxValue option : *array) {
				const Result<std::optional<std::string_view>> text = charged_str(option, r_budget);
				if (!text) {
					return text.error();
				}
				if (*text && mode_matches(p_mode, **text)) {
					enabled = true;
					break;
				}
			}
		}
		if (!enabled) {
			return false;
		}
	}
	return true;
}

Result<LevelInfo> read_level_info(const EbxResolver &p_db, const EbxInstance &p_root, std::string_view p_root_name, std::string_view p_level_id,
		WorkBudget &r_budget) {
	LevelInfo info;
	std::string_view id = p_level_id;
	const Result<std::optional<std::string_view>> root_name = charged_str(p_root.data(), "Name", r_budget);
	if (!root_name) {
		return root_name.error();
	}
	if (const std::optional<std::string_view> name = *root_name) {
		const size_t slash = name->find_last_of("/\\");
		const std::string_view last = slash == std::string_view::npos ? *name : name->substr(slash + 1);
		if (!last.empty()) {
			id = last;
		}
	}
	info.id = std::string(id);
	const std::optional<EbxValue> description = p_root.get("LevelDescription");
	const auto text = [&](std::string_view p_field, std::string &r_out) -> Status {
		const Result<std::optional<std::string_view>> s = charged_str(description ? description->get(p_field) : std::nullopt, r_budget);
		if (!s) {
			return s.error();
		}
		r_out = std::string(s->value_or(""));
		return Status::success();
	};
	info.display_name = std::string(level_display_name(info.id));

	const EbxPartition *asset_partition = p_db.partition_by_name(std::string(p_root_name) + "/description_win32");
	const std::optional<EbxInstance> asset = asset_partition ? std::optional<EbxInstance>(asset_partition->primary()) : std::nullopt;
	if (asset) {
		BF3_TRY(read_description_modes(*asset, r_budget, info.modes));
	}

	std::vector<EbxInstance> components;
	if (const std::optional<EbxStruct> d = description ? description->as_struct() : std::nullopt) {
		BF3_TRY(charged_follow_all(p_db, *d, "Components", r_budget, components));
	}
	if (asset) {
		const std::optional<EbxValue> asset_description = asset->get("Description");
		if (const std::optional<EbxStruct> d = asset_description ? asset_description->as_struct() : std::nullopt) {
			std::vector<EbxInstance> more;
			BF3_TRY(charged_follow_all(p_db, *d, "Components", r_budget, more));
			components.insert(components.end(), more.begin(), more.end());
		}
	}
	for (const EbxInstance &component : components) {
		if (!component.is_a("UILevelDescriptionComponent")) {
			continue;
		}
		if (!info.minimap) {
			info.minimap = read_minimap(component);
		}
		if (info.loading.movie.empty()) {
			BF3_TRY(read_loading_screen(p_db, component, r_budget, info.loading));
		}
	}

	info.zone = zone_name(info.id);
	BF3_TRY(text("Name", info.name_sid));
	BF3_TRY(text("Description", info.description));
	const auto flag = [&](std::string_view p_field) {
		const std::optional<EbxValue> v = description ? description->get(p_field) : std::nullopt;
		return (v ? v->as_bool() : std::nullopt).value_or(false);
	};
	info.multiplayer = flag("IsMultiplayer");
	info.coop = flag("IsCoop");
	const std::optional<EbxValue> size = p_root.get("WorldSizeXZ");
	info.world_size_xz = (size ? size->as_f32() : std::nullopt).value_or(0.0f);
	return info;
}

Result<std::unique_ptr<LoadedLevel>> load_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const LoadOptions &p_options) {
	if (!p_install) {
		return make_error("load_level: no install");
	}
	std::optional<Level> level = p_install->level(p_level_id);
	if (!level) {
		return make_error("unknown level " + std::string(p_level_id));
	}
	Result<LevelMount> mount = LevelMount::mount(p_install, std::move(*level));
	if (!mount) {
		return error_at("mount " + std::string(p_level_id), mount.error());
	}
	auto loaded = std::make_unique<LoadedLevel>(std::move(*mount));
	const BundleIndex names(loaded->mount_.bundle_names());
	const std::string id = to_lower_ascii(p_level_id);
	loaded->root_name = "levels/" + id + "/" + id;
	const std::string &root_name = loaded->root_name;
	const std::optional<std::string> level_bundle = names.find(root_name);
	if (!level_bundle) {
		return make_error("unknown level " + std::string(p_level_id));
	}

	Loader loader(loaded->mount_, loaded->db_, loaded->stats, p_options.threads);
	BF3_TRY(loader.load({ *level_bundle }));
	const EbxPartition *root_partition = loaded->db_.partition_by_name(root_name);
	if (!root_partition) {
		return make_error("missing partition " + root_name);
	}
	if (!root_partition->primary().is_a("LevelData")) {
		return make_error(root_name + ": expected LevelData, found " + std::string(root_partition->primary().type_name()));
	}

	BundleSet &bundles = loaded->bundles;
	bundles.level = *level_bundle;
	for (const char *suffix : { "settings_win32", "gameconfiglight_win32" }) {
		if (std::optional<std::string> bundle = names.find(root_name + "_" + suffix)) {
			bundles.settings.push_back(std::move(*bundle));
		}
	}
	BF3_TRY(loader.load(bundles.settings));

	WorkBudget &budget = *loaded->budget;
	std::vector<std::string> listed_modes;
	if (const EbxPartition *description = loaded->db_.partition_by_name(root_name + "/description_win32")) {
		BF3_TRY(read_description_modes(description->primary(), budget, listed_modes));
	}
	Result<std::string> mode = resolve_level_mode(listed_modes, p_mode);
	if (!mode) {
		return error_at("load " + std::string(p_level_id), mode.error());
	}
	loaded->mode = std::move(*mode);
	std::unordered_set<std::string> seen;
	std::deque<std::string> pending{ root_name };
	while (!pending.empty()) {
		const std::string container = std::move(pending.front());
		pending.pop_front();
		const EbxPartition *partition = loaded->db_.partition_by_name(container);
		if (!partition) {
			continue;
		}
		std::vector<EbxInstance> references;
		BF3_TRY(subworld_references(loaded->db_, partition->primary(), budget, references));

		std::vector<std::string> batch;
		for (const EbxInstance &reference : references) {
			Result<bool> included = includes(loaded->db_, reference, loaded->mode, budget);
			if (!included) {
				return included.error();
			}
			if (!*included) {
				continue;
			}
			const Result<std::optional<std::string_view>> bundle_name = charged_str(reference.data(), "BundleName", budget);
			if (!bundle_name) {
				return bundle_name.error();
			}
			const std::optional<std::string_view> name = *bundle_name;
			if (!name) {
				continue;
			}
			const std::string key = bundle_key(*name);
			if (!seen.insert(key).second) {
				continue;
			}
			if (std::optional<std::string> bundle = names.find(key)) {
				batch.push_back(std::move(*bundle));
				pending.push_back(key);
			} else {
				bundles.absent.emplace_back(*name);
			}
		}
		BF3_TRY(loader.load(batch));
		bundles.subworlds.insert(bundles.subworlds.end(), batch.begin(), batch.end());
	}

	loader.count_imports();
	Result<LevelInfo> info = read_level_info(loaded->db_, loaded->root(), root_name, p_level_id, budget);
	if (!info) {
		return info.error();
	}
	loaded->info = std::move(*info);
	return loaded;
}

Result<LevelInfo> level_info(const Install &p_install, std::string_view p_level_id) {
	Result<LevelMount> mount = mount_level_description(p_install, p_level_id);
	if (!mount) {
		return mount.error();
	}
	return level_info(*mount, p_level_id);
}

Result<LevelMount> mount_level_description(const Install &p_install, std::string_view p_level_id) {
	Result<LevelMount> mount = p_install.mount_level(p_level_id);
	if (!mount) {
		return mount.error();
	}
	const std::vector<std::string> names = mount->bundle_names();
	const std::string id = to_lower_ascii(p_level_id);
	const std::string root_name = "levels/" + id + "/" + id;
	std::vector<std::string> bundles;
	for (const char *suffix : { "", "_settings_win32", "_gameconfiglight_win32", "_uiloadingmp" }) {
		if (std::optional<std::string> bundle = find_bundle(names, root_name + suffix)) {
			bundles.push_back(std::move(*bundle));
		}
	}
	if (bundles.empty() || bundle_key(bundles.front()) != root_name) {
		return make_error("unknown level " + std::string(p_level_id));
	}
	const Status loaded = mount->load_bundles(bundles);
	if (!loaded) {
		return error_at("bundles " + join_names(bundles), loaded.error());
	}
	return mount;
}

Result<LevelInfo> level_info(const LevelMount &p_mount, std::string_view p_level_id) {
	const std::string id = to_lower_ascii(p_level_id);
	const std::string root_name = "levels/" + id + "/" + id;
	EbxDb db;
	const auto parse = [&](const std::string &p_name) -> Status {
		Result<std::vector<uint8_t>> bytes = p_mount.read_ebx(p_name);
		if (!bytes) {
			return Status::success();
		}
		Result<bool> inserted = db.parse_insert(p_name, std::move(*bytes));
		if (!inserted) {
			return error_at(p_name, inserted.error());
		}
		return Status::success();
	};
	for (const std::string &name : { root_name, root_name + "/description_win32" }) {
		BF3_TRY(parse(name));
	}
	const EbxPartition *root = db.partition_by_name(root_name);
	if (!root) {
		return make_error("missing partition " + root_name);
	}
	WorkBudget budget;
	return read_level_info(db, root->primary(), root_name, p_level_id, budget);
}

Result<std::vector<std::string>> level_hints(const LevelMount &p_mount, const EbxRef &p_asset) {
	std::vector<std::string> out;
	if (p_asset.kind != EbxRef::Kind::External) {
		return out;
	}
	Result<std::unordered_map<Guid, std::string, GuidHash>> index = p_mount.ebx_partition_index();
	if (!index) {
		return index.error();
	}
	const auto named = index->find(p_asset.partition);
	if (named == index->end()) {
		return make_error("no loaded bundle holds the hint asset's partition");
	}
	Result<std::vector<uint8_t>> bytes = p_mount.read_ebx(named->second);
	if (!bytes) {
		return error_at(named->second, bytes.error());
	}
	EbxDb db;
	Result<bool> inserted = db.parse_insert(named->second, std::move(*bytes));
	if (!inserted) {
		return error_at(named->second, inserted.error());
	}
	const std::optional<EbxInstance> tips = db.instance(p_asset.partition, p_asset.instance);
	if (!tips) {
		return make_error(named->second + ": no hint asset instance");
	}
	WorkBudget budget;
	BF3_TRY(read_game_tips(*tips, budget, out));
	return out;
}

}

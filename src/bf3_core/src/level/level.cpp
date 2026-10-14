// Level loading: port of IW4L level_bf3 level.rs, mode.rs and info.rs (Apache-2.0). The install is
// only read, through LevelMount.
#include "bf3/core/level/level.h"

#include "bf3/core/thread_pool.h"

#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace bf3::core::level {

namespace {

// Game modes a level description may list (retail levels list at most 15).
constexpr size_t kMaxModes = 4096;
// Layers searched for sub-world references (WorldPartReferenceObjectData nesting).
constexpr uint32_t kLayerDepth = 8;
// Containers one search may visit: the depth limit bounds nesting, not fan-out (a hostile layer
// listing itself many times); retail levels visit at most a few dozen. The work budget bounds the
// slots they list.
constexpr size_t kLayerBudget = size_t(1) << 16;

struct DisplayName {
	std::string_view id;
	std::string_view name;
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

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

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

// Loads bundles into the mount and parses every ebx they add, in the mount's name order.
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

// The mount's bundle names by bundle_key, keeping the first name of each key (the one find_bundle
// picks). Built once per load, so a sub-world key costs a lookup of its (charged) length instead
// of a pass over every bundle name.
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

} // namespace

bool mode_matches(std::string_view p_mode, std::string_view p_option) {
	return iequals_ascii(p_mode, p_option);
}

std::string_view level_display_name(std::string_view p_level_id) {
	for (const DisplayName &d : kDisplayNames) {
		if (iequals_ascii(d.id, p_level_id)) {
			return d.name;
		}
	}
	return p_level_id;
}

std::string zone_name(std::string_view p_level_id) {
	const std::string lower = to_lower_ascii(p_level_id);
	return lower.rfind("mp_", 0) == 0 ? lower : "mp_" + lower;
}

EbxInstance LoadedLevel::root() const {
	return db_.partition_by_name(root_name)->primary(); // load_level checked it
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
		const std::optional<EbxValue> categories = asset->get("Categories");
		const OptionalEbxArray array = categories ? categories->as_array() : OptionalEbxArray();
		if (array) {
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
				BF3_TRY(r_budget.charge(mode_array->size())); // categories may share one array
				for (const EbxValue mode : *mode_array) {
					// Each copy is charged its length: N elements may name one long string.
					const Result<std::optional<std::string_view>> m = charged_str(mode, r_budget);
					if (!m) {
						return m.error();
					}
					if (!*m) {
						continue;
					}
					if (info.modes.size() >= kMaxModes) {
						return make_error("level description lists more than " + std::to_string(kMaxModes) + " game modes");
					}
					info.modes.emplace_back(**m);
				}
			}
		}
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
		if ((info.minimap = read_minimap(component))) {
			break;
		}
	}

	info.zone = zone_name(info.id);
	BF3_TRY(text("Name", info.name_sid));
	BF3_TRY(text("Description", info.description));
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
	loaded->mode = std::string(p_mode);
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
		// Keyed one reference at a time: many references naming one long BundleName cost its
		// length each (charged), but only a new key is kept.
		std::vector<std::string> batch;
		for (const EbxInstance &reference : references) {
			Result<bool> included = includes(loaded->db_, reference, p_mode, budget);
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
	Result<LevelMount> mount = p_install.mount_level(p_level_id);
	if (!mount) {
		return mount.error();
	}
	const std::vector<std::string> names = mount->bundle_names();
	const std::string id = to_lower_ascii(p_level_id);
	const std::string root_name = "levels/" + id + "/" + id;
	std::vector<std::string> bundles;
	for (const char *suffix : { "", "_settings_win32", "_gameconfiglight_win32" }) {
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
	EbxDb db;
	for (const std::string &name : { root_name, root_name + "/description_win32" }) {
		Result<std::vector<uint8_t>> bytes = mount->read_ebx(name);
		if (!bytes) {
			continue;
		}
		Result<bool> inserted = db.parse_insert(name, std::move(*bytes));
		if (!inserted) {
			return error_at(name, inserted.error());
		}
	}
	const EbxPartition *root = db.partition_by_name(root_name);
	if (!root) {
		return make_error("missing partition " + root_name);
	}
	WorkBudget budget;
	return read_level_info(db, root->primary(), root_name, p_level_id, budget);
}

} // namespace bf3::core::level

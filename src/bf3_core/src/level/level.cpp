// Level loading: port of IW4L level_bf3 level.rs, mode.rs and info.rs (Apache-2.0). The install is
// only read, through LevelMount.
#include "bf3/core/level/level.h"

#include "bf3/core/thread_pool.h"

#include <chrono>
#include <deque>
#include <unordered_set>

namespace bf3::core::level {

namespace {

// Layers searched for sub-world references (WorldPartReferenceObjectData nesting).
constexpr uint32_t kLayerDepth = 8;
// Containers one search may visit: the depth limit bounds nesting, not fan-out (a hostile layer
// listing itself many times); retail levels visit at most a few dozen.
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

std::optional<std::string_view> str_field(const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<EbxValue> v = p_object.get(p_field);
	return v ? v->as_str() : std::nullopt;
}

// Every SubWorldReferenceObjectData among p_container's objects and, through world parts, its layers.
void subworld_references(const EbxResolver &p_db, const EbxInstance &p_container, uint32_t p_depth, size_t &r_budget, std::vector<EbxInstance> &r_out) {
	if (r_budget == 0) {
		return;
	}
	--r_budget;
	for (const EbxInstance &object : p_db.follow_all(p_container.data(), "Objects")) {
		if (object.is_a("SubWorldReferenceObjectData")) {
			r_out.push_back(object);
		} else if (object.is_a("WorldPartReferenceObjectData") && p_depth < kLayerDepth) {
			if (const std::optional<EbxInstance> part = p_db.follow(object.data(), "Blueprint")) {
				subworld_references(p_db, *part, p_depth + 1, r_budget, r_out);
			}
		}
	}
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

bool includes(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, size_t *r_slots) {
	const std::optional<EbxValue> auto_load = p_reference.get("AutoLoad");
	if (!(auto_load ? auto_load->as_bool() : std::nullopt).value_or(false)) {
		return false;
	}
	const std::optional<EbxInstance> settings = p_db.follow(p_reference.data(), "InclusionSettings");
	if (!settings) {
		return true;
	}
	if (r_slots) {
		const std::optional<EbxValue> list = settings->get("Settings");
		const OptionalEbxArray slots = list ? list->as_array() : OptionalEbxArray();
		*r_slots += slots ? slots->size() : 0;
	}
	for (const EbxInstance &setting : p_db.follow_all(settings->data(), "Settings")) {
		const std::optional<EbxInstance> criterion = p_db.follow(setting.data(), "Criterion");
		const std::optional<std::string_view> name = criterion ? str_field(criterion->data(), "Name") : std::nullopt;
		if (name && !iequals_ascii(*name, "GameMode")) {
			continue;
		}
		bool enabled = false;
		const std::optional<EbxValue> options = setting.get("EnabledOptions");
		const OptionalEbxArray array = options ? options->as_array() : OptionalEbxArray();
		if (array) {
			if (r_slots) {
				*r_slots += array->size();
			}
			for (const EbxValue option : *array) {
				const std::optional<std::string_view> text = option.as_str();
				if (text && mode_matches(p_mode, *text)) {
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

LevelInfo read_level_info(const EbxResolver &p_db, const EbxInstance &p_root, std::string_view p_root_name, std::string_view p_level_id) {
	LevelInfo info;
	std::string_view id = p_level_id;
	if (const std::optional<std::string_view> name = str_field(p_root.data(), "Name")) {
		const size_t slash = name->find_last_of("/\\");
		const std::string_view last = slash == std::string_view::npos ? *name : name->substr(slash + 1);
		if (!last.empty()) {
			id = last;
		}
	}
	info.id = std::string(id);
	const std::optional<EbxValue> description = p_root.get("LevelDescription");
	const auto text = [&](std::string_view p_field) -> std::string {
		const std::optional<EbxValue> v = description ? description->get(p_field) : std::nullopt;
		const std::optional<std::string_view> s = v ? v->as_str() : std::nullopt;
		return std::string(s.value_or(""));
	};
	info.display_name = std::string(level_display_name(info.id));

	const EbxPartition *asset_partition = p_db.partition_by_name(std::string(p_root_name) + "/description_win32");
	const std::optional<EbxInstance> asset = asset_partition ? std::optional<EbxInstance>(asset_partition->primary()) : std::nullopt;
	if (asset) {
		const std::optional<EbxValue> categories = asset->get("Categories");
		const OptionalEbxArray array = categories ? categories->as_array() : OptionalEbxArray();
		if (array) {
			for (const EbxValue category : *array) {
				const std::optional<EbxValue> kind = category.get("Category");
				const std::optional<std::string_view> kind_name = kind ? kind->as_str() : std::nullopt;
				if (!kind_name || !iequals_ascii(*kind_name, "GameMode")) {
					continue;
				}
				const std::optional<EbxValue> modes = category.get("Mode");
				const OptionalEbxArray mode_array = modes ? modes->as_array() : OptionalEbxArray();
				if (!mode_array) {
					continue;
				}
				for (const EbxValue mode : *mode_array) {
					if (const std::optional<std::string_view> m = mode.as_str()) {
						info.modes.emplace_back(*m);
					}
				}
			}
		}
	}

	std::vector<EbxInstance> components;
	if (const std::optional<EbxStruct> d = description ? description->as_struct() : std::nullopt) {
		components = p_db.follow_all(*d, "Components");
	}
	if (asset) {
		const std::optional<EbxValue> asset_description = asset->get("Description");
		if (const std::optional<EbxStruct> d = asset_description ? asset_description->as_struct() : std::nullopt) {
			for (const EbxInstance &c : p_db.follow_all(*d, "Components")) {
				components.push_back(c);
			}
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
	info.name_sid = text("Name");
	info.description = text("Description");
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
	const std::vector<std::string> names = loaded->mount_.bundle_names();
	const std::string id = to_lower_ascii(p_level_id);
	loaded->root_name = "levels/" + id + "/" + id;
	const std::string &root_name = loaded->root_name;
	const std::optional<std::string> level_bundle = find_bundle(names, root_name);
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
		if (std::optional<std::string> bundle = find_bundle(names, root_name + "_" + suffix)) {
			bundles.settings.push_back(std::move(*bundle));
		}
	}
	BF3_TRY(loader.load(bundles.settings));

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
		size_t budget = kLayerBudget;
		subworld_references(loaded->db_, partition->primary(), 0, budget, references);
		std::vector<std::string> wanted;
		for (const EbxInstance &reference : references) {
			if (!includes(loaded->db_, reference, p_mode)) {
				continue;
			}
			if (const std::optional<std::string_view> bundle = str_field(reference.data(), "BundleName")) {
				wanted.emplace_back(*bundle);
			}
		}
		std::vector<std::string> batch;
		for (const std::string &name : wanted) {
			const std::string key = bundle_key(name);
			if (!seen.insert(key).second) {
				continue;
			}
			if (std::optional<std::string> bundle = find_bundle(names, key)) {
				batch.push_back(std::move(*bundle));
				pending.push_back(key);
			} else {
				bundles.absent.push_back(name);
			}
		}
		BF3_TRY(loader.load(batch));
		bundles.subworlds.insert(bundles.subworlds.end(), batch.begin(), batch.end());
	}

	loader.count_imports();
	loaded->info = read_level_info(loaded->db_, loaded->root(), root_name, p_level_id);
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
	return read_level_info(db, root->primary(), root_name, p_level_id);
}

} // namespace bf3::core::level

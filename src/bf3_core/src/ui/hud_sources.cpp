#include "bf3/core/ui/hud_sources.h"

#include "bf3/core/superbundle_mount.h"
#include "ui_ebx.h"

#include <algorithm>
#include <map>
#include <set>

namespace bf3::core::ui {

namespace {

std::string_view text_of(const std::optional<EbxValue> &p_value) {
	if (!p_value) {
		return {};
	}
	if (const std::optional<std::string_view> e = p_value->enum_name()) {
		return *e;
	}
	return p_value->as_str().value_or(std::string_view());
}

Sha1 partition_sha1(const EbxResolver &p_ui, std::string_view p_name) {
	const EbxLibrary *lib = dynamic_cast<const EbxLibrary *>(&p_ui);
	if (lib == nullptr) {
		return {};
	}
	const std::optional<EbxLibrary::Origin> origin = lib->origin(p_name);
	if (!origin) {
		return {};
	}
	for (size_t i = 0; i < lib->superbundle_count(); ++i) {
		if (iequals_ascii(lib->superbundle(i), origin->superbundle)) {
			return lib->mount(i).entry_sha1(EntryRef::ebx(p_name)).value_or(Sha1{});
		}
	}
	return {};
}

}

const HudComponentType *HudSourceTable::find(std::string_view p_type) const {
	const auto it = std::lower_bound(types.begin(), types.end(), p_type, [](const HudComponentType &a, std::string_view b) { return a.type < b; });
	return it != types.end() && it->type == p_type ? &*it : nullptr;
}

size_t HudSourceTable::source_count() const {
	size_t n = 0;
	for (const HudComponentType &t : types) {
		n += t.sources.size();
	}
	return n;
}

std::vector<std::string> hud_component_partitions(std::span<const LevelMount *const> p_mounts) {
	std::set<std::string> names;
	for (const LevelMount *mount : p_mounts) {
		if (mount == nullptr) {
			continue;
		}
		for (const std::string &n : mount->ebx_names()) {
			std::string key = asset_key(n);
			if (key.starts_with(kHudComponentPrefix)) {
				names.insert(std::move(key));
			}
		}
	}
	return std::vector<std::string>(names.begin(), names.end());
}

Result<HudSourceTable> read_hud_source_table(const EbxResolver &p_ui, std::span<const std::string> p_component_partitions, gameplay::WorkBudget *p_budget,
		std::string_view p_state_partition) {
	gameplay::WorkBudget local;
	gameplay::WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	BF3_TRY(budget.status());
	if (p_component_partitions.size() > kMaxHudComponentTypes) {
		return make_error("hud sources: " + std::to_string(p_component_partitions.size()) + " component partitions, more than " +
				std::to_string(kMaxHudComponentTypes));
	}
	BF3_TRY(budget.charge(p_component_partitions.size()));
	HudSourceTable out;
	out.version = kHudSourceTableVersion;
	out.state_partition = to_lower_ascii(p_state_partition);
	std::map<std::string, size_t> by_partition;
	for (const std::string &name : p_component_partitions) {
		BF3_TRY(budget.charge_bytes(name.size()));
		const EbxPartition *partition = p_ui.partition_by_name(name);
		if (partition == nullptr) {
			return make_error(name + ": component partition does not resolve");
		}
		const EbxInstance primary = partition->primary();
		BF3_TRY(budget.charge_type(primary.data()));
		HudComponentType t;
		t.type = std::string(primary.type_name());
		BF3_TRY(budget.charge_bytes(t.type.size()));
		t.partition = to_lower_ascii(name);
		const std::string_view update = text_of(primary.get("UpdateType"));
		BF3_TRY(budget.charge_bytes(update.size()));
		t.update_type = std::string(update);
		const std::optional<double> ups = detail::number(primary.get("UpdatesPerSecond"));
		t.updates_per_second = ups && std::isfinite(*ups) && std::fabs(*ups) < 1e9 ? int32_t(*ups) : 0;
		const std::optional<EbxValue> sources_value = primary.get("DataSources");
		const OptionalEbxArray sources = sources_value ? sources_value->as_array() : OptionalEbxArray();
		if (sources) {
			if (sources->size() > kMaxHudSourcesPerType) {
				return make_error(name + ": " + std::to_string(sources->size()) + " DataSources, more than " + std::to_string(kMaxHudSourcesPerType));
			}
			BF3_TRY(budget.charge(sources->size()));
			for (const EbxValue s : *sources) {
				const std::string_view text = s.as_str().value_or(std::string_view());
				BF3_TRY(budget.charge_bytes(text.size()));
				t.sources.emplace_back(text);
			}
		}
		t.sha1 = partition_sha1(p_ui, t.partition);
		by_partition.emplace(t.partition, out.types.size());
		out.types.push_back(std::move(t));
	}

	std::sort(out.types.begin(), out.types.end(), [](const HudComponentType &a, const HudComponentType &b) { return a.type < b.type; });
	for (size_t i = 1; i < out.types.size(); ++i) {
		if (out.types[i].type == out.types[i - 1].type) {
			return make_error("hud sources: type " + out.types[i].type + " is the primary of both " + out.types[i - 1].partition + " and " + out.types[i].partition);
		}
	}
	std::map<std::string, uint32_t> index_of;
	for (size_t i = 0; i < out.types.size(); ++i) {
		index_of.emplace(out.types[i].partition, uint32_t(i));
	}

	BF3_TRY(budget.charge(1));
	const EbxPartition *state = p_ui.partition_by_name(p_state_partition);
	if (state == nullptr) {
		return make_error(std::string(p_state_partition) + ": state partition not found");
	}
	const EbxInstance state_asset = state->primary();
	BF3_TRY(budget.charge_type(state_asset.data()));
	const std::optional<EbxValue> comps_value = state_asset.get("UIComponents");
	const OptionalEbxArray comps = comps_value ? comps_value->as_array() : OptionalEbxArray();
	if (!comps) {
		return make_error(out.state_partition + ": no UIComponents");
	}
	if (comps->size() > kMaxHudStateComponents) {
		return make_error(out.state_partition + ": " + std::to_string(comps->size()) + " components, more than " + std::to_string(kMaxHudStateComponents));
	}
	BF3_TRY(budget.charge(comps->size()));
	for (const EbxValue c : *comps) {
		const std::optional<EbxRef> r = c.as_ref();
		if (!r || r->is_null()) {
			return make_error(out.state_partition + ": a null UIComponents entry");
		}
		const std::optional<std::string> name = detail::ref_partition(p_ui, *state, *r);
		if (!name || name->empty() || !p_ui.resolve(*state, *r)) {
			return make_error(out.state_partition + ": a UIComponents entry does not resolve");
		}
		const auto it = index_of.find(*name);
		if (it == index_of.end()) {
			return make_error(out.state_partition + ": component " + *name + " is not among the " + std::to_string(out.types.size()) + " listed partitions");
		}
		out.state_components.push_back(it->second);
	}
	out.state_sha1 = partition_sha1(p_ui, out.state_partition);
	BF3_TRY(budget.status());
	return out;
}

std::string format_hud_source_table(const HudSourceTable &p_table) {
	std::vector<bool> in_state(p_table.types.size(), false);
	for (uint32_t i : p_table.state_components) {
		if (i < in_state.size()) {
			in_state[i] = true;
		}
	}
	std::string out = "# hud source table version " + std::to_string(p_table.version) + ", state " + p_table.state_partition + " " +
			p_table.state_sha1.to_string() + ", " + std::to_string(p_table.types.size()) + " types, " + std::to_string(p_table.state_components.size()) +
			" state components, " + std::to_string(p_table.source_count()) + " sources\n";
	out += "type\tpartition\tupdate_type\tupdates_per_second\tin_state\tcount\tsources\n";
	for (size_t i = 0; i < p_table.types.size(); ++i) {
		const HudComponentType &t = p_table.types[i];
		out += t.type + "\t" + t.partition + "\t" + t.update_type + "\t" + std::to_string(t.updates_per_second) + "\t" + (in_state[i] ? "1" : "0") + "\t" +
				std::to_string(t.sources.size()) + "\t";
		for (size_t s = 0; s < t.sources.size(); ++s) {
			out += (s ? "," : "") + t.sources[s];
		}
		out += "\n";
	}
	return out;
}

}

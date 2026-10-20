#include "ebx_commands.h"
#include "json_number.h"

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/thread_pool.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>
#include <unordered_set>

using namespace bf3::core;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

struct MemorySample {
	double working_set = 0, peak_working_set = 0, private_bytes = 0, peak_private_bytes = 0;
};

MemorySample memory_sample() {
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
	const double mib = 1024.0 * 1024.0;
	return { double(pmc.WorkingSetSize) / mib, double(pmc.PeakWorkingSetSize) / mib, double(pmc.PrivateUsage) / mib, double(pmc.PeakPagefileUsage) / mib };
}

std::wstring widen(std::string_view p_text) {
	const int need = MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(need), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), out.data(), need);
	return out;
}

std::string narrow(const std::wstring &p_text) {
	const int len = static_cast<int>(p_text.size());
	const int need = WideCharToMultiByte(CP_UTF8, 0, p_text.data(), len, nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(need), '\0');
	WideCharToMultiByte(CP_UTF8, 0, p_text.data(), len, out.data(), need, nullptr, nullptr);
	return out;
}

std::vector<std::string> split_commas(std::string_view p_text) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= p_text.size()) {
		const size_t comma = std::min(p_text.find(',', pos), p_text.size());
		out.emplace_back(p_text.substr(pos, comma - pos));
		pos = comma + 1;
	}
	return out;
}

void print_load_report(const EbxLibrary::LoadReport &p_report) {
	for (const EbxLibrary::SuperbundleReport &r : p_report.superbundles) {
		std::fprintf(stderr, "%s: +%zu partitions (%zu failed), db %zu\n", r.superbundle.c_str(), r.fresh, r.failed, r.total_after);
	}
	for (const std::string &e : p_report.errors) {
		std::fprintf(stderr, "  %s\n", e.c_str());
	}
}

std::unique_ptr<EbxLibrary> load_library(const EbxContext &p_ctx, const std::string &p_spec) {
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_ctx.install, expand_superbundles(*p_ctx.install, p_spec));
	if (!lib) {
		std::fprintf(stderr, "%s\n", lib.error().message.c_str());
		return nullptr;
	}
	print_load_report((*lib)->load_all());
	return std::move(*lib);
}

std::string rust_nonfinite(double p_value) {
	return std::isnan(p_value) ? "NaN" : (p_value < 0 ? "-inf" : "inf");
}

class JsonOut {
public:
	std::string text;

	void str(std::string_view p_text) {
		static const char kHex[] = "0123456789abcdef";
		text += '"';
		for (const char ch : p_text) {
			const uint8_t c = static_cast<uint8_t>(ch);
			switch (c) {
				case '"':
					text += "\\\"";
					break;
				case '\\':
					text += "\\\\";
					break;
				case '\b':
					text += "\\b";
					break;
				case '\f':
					text += "\\f";
					break;
				case '\n':
					text += "\\n";
					break;
				case '\r':
					text += "\\r";
					break;
				case '\t':
					text += "\\t";
					break;
				default:
					if (c < 0x20) {
						text += "\\u00";
						text += kHex[c >> 4];
						text += kHex[c & 15];
					} else {
						text += ch;
					}
			}
		}
		text += '"';
	}
	void raw(std::string_view p_text) { text += p_text; }
	void open(char p_bracket) {
		text += p_bracket;
		first_.push_back(true);
	}

	void item() {
		text += first_.back() ? "\n" : ",\n";
		first_.back() = false;
		text.append(first_.size() * 2, ' ');
	}
	void key(std::string_view p_key) {
		item();
		str(p_key);
		text += ": ";
	}
	void close(char p_bracket) {
		const bool empty = first_.back();
		first_.pop_back();
		if (!empty) {
			text += '\n';
			text.append(first_.size() * 2, ' ');
		}
		text += p_bracket;
	}
	template <class T>
	void integer(T p_value) {
		char buf[24];
		const auto res = std::to_chars(buf, buf + sizeof(buf), p_value);
		text.append(buf, res.ptr);
	}

private:
	std::vector<bool> first_;
};

class Dumper {
public:
	explicit Dumper(const EbxLibrary &p_lib) :
			lib_(p_lib) {}

	void partition(JsonOut &r_out, std::string_view p_name) {
		const EbxPartition *part = lib_.partition_by_name(p_name);
		const std::optional<EbxLibrary::Origin> origin = lib_.origin(p_name);
		r_out.open('{');
		r_out.key("bundle");
		r_out.str(origin ? origin->bundle : std::string());
		r_out.key("guid");
		r_out.str(part->guid().to_string());
		r_out.key("instances");
		r_out.open('[');
		for (size_t i = 0; i < part->instance_count(); ++i) {
			r_out.item();
			instance(r_out, *part->instance(i));
		}
		r_out.close(']');
		r_out.key("partition");
		r_out.str(lib_.name_of(part->guid()).value_or(p_name));
		r_out.key("primary_type");
		r_out.str(part->primary().type_name());
		r_out.key("superbundle");
		r_out.str(origin ? origin->superbundle : std::string());
		r_out.close('}');
	}

private:
	struct Item {
		enum class Kind { Text, True, Chain, Value };
		std::string_view key;
		Kind kind = Kind::Text;
		std::string text;
		std::optional<EbxValue> value;
	};

	void f32(JsonOut &r_out, float p_value) {
		if (!std::isfinite(p_value)) {
			r_out.str(rust_nonfinite(p_value));
			return;
		}
		r_out.raw(json_f32_text(p_value));
	}

	void ref(JsonOut &r_out, const EbxPartition &p_from, const EbxRef &p_ref) {
		switch (p_ref.kind) {
			case EbxRef::Kind::Null:
				r_out.raw("null");
				return;
			case EbxRef::Kind::Internal: {
				const std::optional<EbxInstance> inst = p_from.instance(p_ref.index);
				r_out.open('{');
				r_out.key("$ref");
				r_out.str("internal");
				if (inst) {
					r_out.key("instance");
					r_out.str(inst->guid().to_string());
					r_out.key("type");
					r_out.str(inst->type_name());
				} else {
					r_out.key("index");
					r_out.integer(p_ref.index);
					r_out.key("unresolved");
					r_out.raw("true");
				}
				r_out.close('}');
				return;
			}
			case EbxRef::Kind::External: {
				const std::optional<std::string_view> name = lib_.name_of(p_ref.partition);
				const std::optional<EbxInstance> inst = lib_.instance(p_ref.partition, p_ref.instance);
				r_out.open('{');
				r_out.key("$ref");
				r_out.str(name ? std::string(*name) : "<unloaded " + p_ref.partition.to_string() + ">");
				r_out.key("instance");
				r_out.str(p_ref.instance.to_string());
				r_out.key("type");
				if (inst) {
					r_out.str(inst->type_name());
				} else {
					r_out.raw("null");
				}
				r_out.close('}');
				return;
			}
		}
	}

	void value(JsonOut &r_out, const EbxPartition &p_part, const EbxValue &p_value) {
		using K = EbxValue::Kind;
		switch (p_value.kind()) {
			case K::Bool:
				r_out.raw(*p_value.as_bool() ? "true" : "false");
				return;
			case K::I8:
			case K::I16:
			case K::I32:
			case K::I64:
				r_out.integer(*p_value.as_i64());
				return;
			case K::U8:
			case K::U16:
			case K::U32:
			case K::U64:
				r_out.integer(*p_value.as_u64());
				return;
			case K::F32:
				f32(r_out, *p_value.as_f32());
				return;
			case K::F64: {
				const double d = *p_value.as_f64();
				if (std::isfinite(d)) {
					r_out.raw(json_f64_text(d));
				} else {
					r_out.str(rust_nonfinite(d));
				}
				return;
			}
			case K::Guid:
				r_out.str(p_value.as_guid()->to_string());
				return;
			case K::Sha1:
				r_out.str(p_value.as_sha1()->to_string());
				return;
			case K::String:
				if (p_value.is_null_string()) {
					r_out.raw("null");
				} else {
					r_out.str(*p_value.as_str());
				}
				return;
			case K::Enum:
				if (const std::optional<std::string_view> n = p_value.enum_name()) {
					r_out.str(*n);
				} else {
					r_out.integer(*p_value.enum_value());
				}
				return;
			case K::Ref:
				ref(r_out, p_part, *p_value.as_ref());
				return;
			case K::Struct: {
				const EbxStruct s = *p_value.as_struct();
				std::vector<Item> items;
				items.push_back(Item{ "$type", Item::Kind::Text, std::string(s.type_name()), {} });
				collect(items, s.fields());
				object(r_out, p_part, items, {});
				return;
			}
			case K::Array:
				r_out.open('[');
				for (const EbxValue element : *p_value.as_array()) {
					r_out.item();
					value(r_out, p_part, element);
				}
				r_out.close(']');
				return;
		}
	}

	void instance(JsonOut &r_out, const EbxInstance &p_inst) {
		std::vector<Item> items;
		items.push_back(Item{ "$guid", Item::Kind::Text, p_inst.guid().to_string(), {} });
		items.push_back(Item{ "$type", Item::Kind::Text, std::string(p_inst.type_name()), {} });
		if (p_inst.is_primary()) {
			items.push_back(Item{ "$primary", Item::Kind::True, {}, {} });
		}
		items.push_back(Item{ "$chain", Item::Kind::Chain, {}, {} });
		collect(items, p_inst.fields());
		object(r_out, p_inst.partition(), items, p_inst.type_chain());
	}

	static void collect(std::vector<Item> &r_items, const EbxStruct::FieldRange &p_fields) {
		for (const EbxField &f : p_fields) {
			r_items.push_back(Item{ f.name, Item::Kind::Value, {}, f.value });
		}
	}

	void object(JsonOut &r_out, const EbxPartition &p_part, std::vector<Item> &r_items, const std::vector<std::string_view> &p_chain) {
		std::stable_sort(r_items.begin(), r_items.end(), [](const Item &a, const Item &b) { return a.key < b.key; });
		r_out.open('{');
		for (size_t i = 0; i < r_items.size(); ++i) {
			if (i + 1 < r_items.size() && r_items[i + 1].key == r_items[i].key) {
				continue;
			}
			const Item &it = r_items[i];
			r_out.key(it.key);
			switch (it.kind) {
				case Item::Kind::Text:
					r_out.str(it.text);
					break;
				case Item::Kind::True:
					r_out.raw("true");
					break;
				case Item::Kind::Chain:
					r_out.open('[');
					for (const std::string_view t : p_chain) {
						r_out.item();
						r_out.str(t);
					}
					r_out.close(']');
					break;
				case Item::Kind::Value:
					value(r_out, p_part, *it.value);
					break;
			}
		}
		r_out.close('}');
	}

	const EbxLibrary &lib_;
};

void collect_refs(const EbxValue &p_value, std::vector<Guid> &r_out) {
	switch (p_value.kind()) {
		case EbxValue::Kind::Ref: {
			const EbxRef r = *p_value.as_ref();
			if (r.kind == EbxRef::Kind::External) {
				r_out.push_back(r.partition);
			}
			return;
		}
		case EbxValue::Kind::Struct:
			for (const EbxField &f : p_value.as_struct()->fields()) {
				collect_refs(f.value, r_out);
			}
			return;
		case EbxValue::Kind::Array:
			for (const EbxValue element : *p_value.as_array()) {
				collect_refs(element, r_out);
			}
			return;
		default:
			return;
	}
}

bool skip_type(std::string_view p_type) {
	static constexpr std::string_view kSkip[] = { "RigidMeshAsset", "SkinnedMeshAsset", "CompositeMeshAsset", "TextureAsset", "SoundWaveAsset",
		"SoundPatchAsset", "ShaderGraph", "SurfaceShaderBaseAsset", "MeshVariationDatabase" };
	return std::find(std::begin(kSkip), std::end(kSkip), p_type) != std::end(kSkip);
}

bool follows(std::string_view p_name, const std::vector<std::string> &p_follow) {
	const bool all = std::find(p_follow.begin(), p_follow.end(), "*") != p_follow.end();
	if ((p_name.rfind("fx/", 0) == 0 || p_name.rfind("sound/", 0) == 0) && !all) {
		return false;
	}
	return std::any_of(p_follow.begin(), p_follow.end(), [&](const std::string &f) { return f == "*" || p_name.find(f) != std::string_view::npos; });
}

std::string sanitize(std::string_view p_name) {
	std::string out;
	for (const char c : p_name) {
		if (c == '/' || c == '\\') {
			out += "__";
		} else {
			out += c;
		}
	}
	return out;
}

bool guard_allows(const EbxContext &p_ctx, const std::filesystem::path &p_path) {
	const PathGuard::Verdict verdict = p_ctx.reader->write_guard().check_write_target(p_path);
	if (verdict != PathGuard::Verdict::Allowed) {
		std::fprintf(stderr, "refusing to write %s: %s\n", narrow(p_path.wstring()).c_str(), PathGuard::verdict_name(verdict));
		return false;
	}
	return true;
}

bool write_file(const EbxContext &p_ctx, const std::filesystem::path &p_path, const std::string &p_text) {
	if (!guard_allows(p_ctx, p_path)) {
		return false;
	}
	FILE *f = nullptr;
	if (_wfopen_s(&f, bf3::core::to_extended_length(p_path).c_str(), L"wb") != 0 || f == nullptr) {
		std::fprintf(stderr, "cannot create %s\n", narrow(p_path.wstring()).c_str());
		return false;
	}
	const bool ok = std::fwrite(p_text.data(), 1, p_text.size(), f) == p_text.size() && std::fflush(f) == 0;
	std::fclose(f);
	return ok;
}

size_t walk_value(const EbxValue &p_value) {
	switch (p_value.kind()) {
		case EbxValue::Kind::Struct: {
			size_t n = 1;
			for (const EbxField &f : p_value.as_struct()->fields()) {
				n += walk_value(f.value);
			}
			return n;
		}
		case EbxValue::Kind::Array: {
			size_t n = 1;
			for (const EbxValue element : *p_value.as_array()) {
				n += walk_value(element);
			}
			return n;
		}
		default:
			return 1;
	}
}

}

int cmd_find(const EbxContext &p_ctx, const std::string &p_spec, const std::vector<std::string> &p_needles) {
	std::unique_ptr<EbxLibrary> lib = load_library(p_ctx, p_spec);
	if (!lib) {
		return 1;
	}
	std::vector<std::string> needles;
	for (const std::string &n : p_needles) {
		needles.push_back(to_lower_ascii(n));
	}
	std::map<std::string_view, std::string> rows;
	for (const EbxLibrary::Entry &e : lib->entries()) {
		if (std::any_of(needles.begin(), needles.end(), [&](const std::string &n) { return e.name.find(n) != std::string_view::npos; })) {
			const std::optional<EbxLibrary::Origin> origin = lib->origin(e.name);
			rows[e.name] = std::string(e.partition->primary().type_name()) + "\t" + (origin ? origin->superbundle : std::string()) + "\t" +
					(origin ? origin->bundle : std::string());
		}
	}
	std::string out;
	for (const auto &[name, row] : rows) {
		out += name;
		out += '\t';
		out += row;
		out += '\n';
	}
	std::fwrite(out.data(), 1, out.size(), stdout);
	return 0;
}

int cmd_types(const EbxContext &p_ctx, const std::string &p_spec, const std::vector<std::string> &p_types) {
	std::unique_ptr<EbxLibrary> lib = load_library(p_ctx, p_spec);
	if (!lib) {
		return 1;
	}
	const std::unordered_set<std::string_view> wanted(p_types.begin(), p_types.end());
	std::set<std::string> rows;
	for (const EbxLibrary::Entry &e : lib->entries()) {
		for (size_t i = 0; i < e.partition->instance_count(); ++i) {
			const EbxInstance inst = *e.partition->instance(i);
			if (wanted.contains(inst.type_name())) {
				rows.insert(std::string(inst.type_name()) + "\t" + std::string(e.name) + "\t" + inst.guid().to_string());
			}
		}
	}
	std::string out;
	for (const std::string &r : rows) {
		out += r;
		out += '\n';
	}
	std::fwrite(out.data(), 1, out.size(), stdout);
	return 0;
}

int cmd_dump(const EbxContext &p_ctx, const std::string &p_spec, const std::string &p_out_dir, size_t p_depth, const std::string &p_follow,
		const std::vector<std::string> &p_roots) {
	std::vector<std::string> follow;
	for (const std::string &f : split_commas(p_follow)) {
		follow.push_back(to_lower_ascii(f));
	}
	std::unique_ptr<EbxLibrary> lib = load_library(p_ctx, p_spec);
	if (!lib) {
		return 1;
	}
	const std::filesystem::path dir(widen(p_out_dir));
	if (!guard_allows(p_ctx, dir)) {
		return 1;
	}
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	int failures = 0;
	for (const std::string &root : p_roots) {
		const EbxPartition *part = lib->partition_by_name(root);
		if (part == nullptr) {
			std::fprintf(stderr, "missing root %s\n", root.c_str());
			continue;
		}

		std::unordered_set<Guid, GuidHash> seen;
		std::vector<std::pair<size_t, Guid>> order;
		std::deque<std::pair<size_t, Guid>> queue{ { size_t(0), part->guid() } };
		std::set<std::string> unresolved, not_followed;
		while (!queue.empty()) {
			const auto [d, g] = queue.front();
			queue.pop_front();
			if (!seen.insert(g).second) {
				continue;
			}
			const EbxPartition *p = lib->partition(g);
			if (p == nullptr) {
				unresolved.insert(g.to_string());
				continue;
			}
			const std::string_view pname = lib->name_of(g).value_or(std::string_view());
			const std::string_view primary_type = p->primary().type_name();
			if (d > 0 && (!follows(pname, follow) || skip_type(primary_type))) {
				not_followed.insert(std::string(pname) + " (" + std::string(primary_type) + ")");
				continue;
			}
			order.emplace_back(d, g);
			if (d >= p_depth || (d > 0 && skip_type(primary_type))) {
				continue;
			}
			std::vector<Guid> refs;
			for (size_t i = 0; i < p->instance_count(); ++i) {
				for (const EbxField &f : p->instance(i)->fields()) {
					collect_refs(f.value, refs);
				}
			}
			for (const Guid &r : refs) {
				if (!seen.contains(r)) {
					queue.emplace_back(d + 1, r);
				}
			}
		}
		JsonOut out;
		Dumper dumper(*lib);
		out.open('{');
		out.key("closure");
		out.open('[');
		for (const auto &[d, g] : order) {
			const EbxPartition *p = lib->partition(g);
			out.item();
			out.open('{');
			out.key("depth");
			out.integer(d);
			out.key("instances");
			out.integer(p->instance_count());
			out.key("partition");
			out.str(lib->name_of(g).value_or("?"));
			out.key("primary_type");
			out.str(p->primary().type_name());
			out.close('}');
		}
		out.close(']');
		out.key("depth_limit");
		out.integer(p_depth);
		out.key("follow_filter");
		out.open('[');
		for (const std::string &f : follow) {
			out.item();
			out.str(f);
		}
		out.close(']');
		out.key("partitions");
		out.open('[');
		for (const auto &[d, g] : order) {
			const EbxPartition *p = lib->partition(g);
			if (d == 0 || !skip_type(p->primary().type_name())) {
				out.item();
				dumper.partition(out, lib->name_of(g).value_or("?"));
			}
		}
		out.close(']');
		out.key("referenced_not_followed");
		out.open('[');
		for (const std::string &n : not_followed) {
			out.item();
			out.str(n);
		}
		out.close(']');
		out.key("root");
		out.str(root);
		out.key("unresolved_partitions");
		out.open('[');
		for (const std::string &n : unresolved) {
			out.item();
			out.str(n);
		}
		out.close(']');
		out.close('}');
		const std::filesystem::path path = dir / widen(sanitize(root) + ".json");
		if (!write_file(p_ctx, path, out.text)) {
			++failures;
			continue;
		}
		std::fprintf(stderr, "wrote %s (%zu partitions)\n", narrow(path.wstring()).c_str(), order.size());
	}
	return failures == 0 ? 0 : 1;
}

int cmd_ebxload(const EbxContext &p_ctx, const std::string &p_spec, bool p_walk, bool p_lookups) {
	const auto start = std::chrono::steady_clock::now();
	const std::vector<std::string> sbs = expand_superbundles(*p_ctx.install, p_spec);
	Result<std::unique_ptr<EbxLibrary>> opened = EbxLibrary::open(p_ctx.install, sbs);
	if (!opened) {
		std::fprintf(stderr, "%s\n", opened.error().message.c_str());
		return 1;
	}
	EbxLibrary &lib = **opened;
	const double mount_s = seconds_since(start);
	const MemorySample opened_memory = memory_sample();
	const EbxLibrary::LoadReport report = lib.load_all();
	const MemorySample loaded_memory = memory_sample();
	print_load_report(report);
	for (const std::string &e : report.errors) {
		std::printf("error\t%s\n", e.c_str());
	}
	size_t instances = 0, dup_instance_guids = 0, imports = 0;
	const std::vector<EbxLibrary::Entry> entries = lib.entries();
	for (const EbxLibrary::Entry &e : entries) {
		instances += e.partition->instance_count();
		dup_instance_guids += e.partition->duplicate_instance_guids();
		imports += e.partition->imports().size();
	}
	std::printf("superbundles\t%zu\tcatalog\t%zu\tpartitions\t%zu\tparsed\t%zu\tfailed\t%zu\tduplicate_guids\t%zu\n", sbs.size(), lib.catalog_size(),
			report.partitions, report.parsed, report.failed, report.duplicate_guids);
	std::printf("instances\t%zu\timports\t%zu\tduplicate_instance_guids\t%zu\tbytes\t%llu\n", instances, imports, dup_instance_guids,
			static_cast<unsigned long long>(report.bytes));
	std::printf("time\tmount+catalog %.3f s\tparse %.3f s\tinsert %.3f s\n", mount_s, report.parse_seconds, report.insert_seconds);

	EbxPartition::MemoryUse use;
	std::set<const void *> layouts;
	for (const EbxLibrary::Entry &e : entries) {
		const EbxPartition::MemoryUse m = e.partition->memory_use();
		use.bytes += m.bytes;
		use.strings += m.strings;
		use.instances += m.instances;
		use.slack += m.slack;
		use.blocks += m.blocks;
		if (layouts.insert(m.types_id).second) {
			use.types += m.types;
		}
	}
	const double mib = 1024.0 * 1024.0;
	std::printf("partition_heap\tbytes %.1f\tstrings %.1f\tinstances %.1f\tslack %.1f\tobjects %.1f MiB\tblocks %zu\ttypes %.1f MiB in %zu layouts\n",
			double(use.bytes) / mib, double(use.strings) / mib, double(use.instances) / mib, double(use.slack) / mib,
			double(entries.size() * sizeof(EbxPartition)) / mib, use.blocks, double(use.types) / mib, layouts.size());
	std::printf("memory\topened ws %.1f private %.1f\tloaded ws %.1f private %.1f\tpeak ws %.1f private %.1f MiB\n", opened_memory.working_set,
			opened_memory.private_bytes, loaded_memory.working_set, loaded_memory.private_bytes, loaded_memory.peak_working_set,
			loaded_memory.peak_private_bytes);
	if (p_walk) {
		const auto walk_start = std::chrono::steady_clock::now();
		std::atomic<size_t> values{ 0 };
		parallel_for(entries.size(), [&](size_t i) {
			const EbxPartition &p = *entries[i].partition;
			size_t n = 0;
			for (size_t k = 0; k < p.instance_count(); ++k) {
				for (const EbxField &f : p.instance(k)->fields()) {
					n += walk_value(f.value);
				}
			}
			values += n;
		});
		std::printf("walk\tvalues\t%zu\t%.3f s\n", values.load(), seconds_since(walk_start));
	}
	if (p_lookups) {

		for (int pass = 0; pass < 3; ++pass) {
			const auto lookup_start = std::chrono::steady_clock::now();
			size_t found = 0, asked = 0;
			for (const EbxLibrary::Entry &entry : entries) {
				const EbxPartition &p = *entry.partition;
				for (size_t k = 0; k < p.instance_count(); ++k) {
					const EbxStruct data = p.instance(k)->data();
					for (const EbxField &f : data.fields()) {
						++asked;
						found += data.get(f.name).has_value() ? 1 : 0;
					}
				}
			}
			std::printf("lookups\tpass %d\t%zu of %zu found\t%.3f s\n", pass + 1, found, asked, seconds_since(lookup_start));
		}
	}
	std::printf("total\t%.3f s\n", seconds_since(start));
	return report.failed == 0 ? 0 : 1;
}

}

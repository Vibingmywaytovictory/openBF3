#include "text_commands.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/ui/text_census.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/elapsed.h"

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace bf3dump {

namespace {

using bf3::core::seconds_since;

std::wstring widen(std::string_view p_text) {
	const int need = MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(need), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), out.data(), need);
	return out;
}

std::string hex8(uint32_t p_value) {
	char text[12];
	std::snprintf(text, sizeof(text), "%08x", p_value);
	return text;
}

std::string escaped(std::string_view p_text) {
	std::string out;
	out.reserve(p_text.size());
	for (const char c : p_text) {
		switch (c) {
			case '\\':
				out += "\\\\";
				break;
			case '\t':
				out += "\\t";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			default:
				out += c;
				break;
		}
	}
	return out;
}

std::optional<uint32_t> parse_hash(std::string_view p_key) {
	std::string_view digits = p_key;
	const bool prefixed = digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
	if (prefixed) {
		digits.remove_prefix(2);
	} else if (digits.size() != 8) {
		return std::nullopt;
	}
	if (digits.empty() || digits.size() > 8) {
		return std::nullopt;
	}
	uint32_t value = 0;
	const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
	if (res.ec != std::errc() || res.ptr != digits.data() + digits.size()) {
		return std::nullopt;
	}
	return value;
}

std::optional<TextDatabase> open_db(const EbxContext &p_ctx, TextDatabaseSource *r_source) {
	const auto start = std::chrono::steady_clock::now();
	Result<TextDatabase> db = open_text_database(p_ctx.install, {}, r_source);
	if (!db) {
		std::fprintf(stderr, "text database: %s\n", db.error().message.c_str());
		return std::nullopt;
	}
	std::fprintf(stderr, "text database: %zu entries in %.3f s\n", db->size(), seconds_since(start));
	return std::move(*db);
}

}

int cmd_text_lookup(const EbxContext &p_ctx, const std::vector<std::string> &p_keys) {
	const std::optional<TextDatabase> db = open_db(p_ctx, nullptr);
	if (!db) {
		return 1;
	}
	size_t absent = 0;
	for (const std::string &key : p_keys) {
		const std::optional<uint32_t> as_hash = parse_hash(key);
		const uint32_t hash = as_hash ? *as_hash : text_hash(key);
		const std::optional<std::string_view> text = db->find(hash);
		if (text) {
			std::printf("%s\t%s\t%s\n", key.c_str(), hex8(hash).c_str(), escaped(*text).c_str());
		} else {
			++absent;
			std::printf("%s\t%s\tabsent -> %s\n", key.c_str(), hex8(hash).c_str(), as_hash ? "-" : key.c_str());
		}
	}
	return absent == 0 ? 0 : 1;
}

int cmd_text_census(const EbxContext &p_ctx, const std::string &p_spec) {
	TextDatabaseSource source;
	const std::optional<TextDatabase> db = open_db(p_ctx, &source);
	if (!db) {
		return 1;
	}
	const TextDatabaseRef &ref = source.ref;
	std::printf("database\t%s\tname=%s\tlanguage=%s\tsuperbundle=%s\n", ref.partition.c_str(), ref.name.c_str(), ref.language.c_str(),
			source.superbundle.c_str());
	std::printf("binary\t%s\t%u B\tsha1=%s\n", ref.binary_chunk.to_string().c_str(), ref.binary_size, source.binary_sha1.to_string().c_str());
	std::printf("histogram\t%s\t%u B\tsha1=%s\tunits=%zu\tcount=%u\n", ref.histogram_chunk.to_string().c_str(), ref.histogram_size,
			source.histogram_sha1.to_string().c_str(), db->histogram_units(), db->histogram_count());
	std::printf("entries\t%zu\ttag=%s\tdecode_errors=%zu\tduplicate_hashes=%zu\n", db->size(), db->tag().c_str(), db->decode_errors(),
			db->duplicate_hashes());
	std::fflush(stdout);

	const auto start = std::chrono::steady_clock::now();
	std::vector<std::string> sbs;
	if (p_spec.empty()) {
		for (const std::string_view sb : research_census_superbundles()) {
			sbs.emplace_back(sb);
		}
	} else {
		sbs = expand_superbundles(*p_ctx.install, p_spec);
	}
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_ctx.install, sbs);
	if (!lib) {
		std::fprintf(stderr, "%s\n", lib.error().message.c_str());
		return 1;
	}
	std::vector<std::string> names;
	if (p_spec.empty()) {
		for (const std::string_view name : research_census_partitions()) {
			names.emplace_back(name);
		}
	} else {
		const EbxLibrary::LoadReport report = (*lib)->load_all();
		std::fprintf(stderr, "ebx: %zu partitions loaded, %zu failed\n", report.partitions, report.failed);
		for (const EbxLibrary::Entry &e : (*lib)->entries()) {
			names.emplace_back(e.name);
		}
	}
	const std::vector<std::string_view> views(names.begin(), names.end());
	const SidCensus census = sid_census(**lib, views, *db);
	std::printf("ebx_sids\tset=%s\tpartitions=%zu/%zu\tdistinct=%zu\tresolved=%zu\tmissing_marker=%zu\tabsent=%zu\t%.2f s\n",
			p_spec.empty() ? "research" : p_spec.c_str(), census.partitions, names.size(), census.sids.size(), census.resolved, census.missing_marker,
			census.absent, seconds_since(start));
	for (const std::string &name : census.missing_partitions) {
		std::printf("  missing partition\t%s\n", name.c_str());
	}
	for (const std::string &sid : census.sids) {
		if (!db->resolves(sid)) {
			std::printf("  unresolved\t%s\t%s\t%s\n", sid.c_str(), hex8(text_hash(sid)).c_str(), db->find_sid(sid) ? "<MISSING>" : "absent");
		}
	}
	const bool ok = db->decode_errors() == 0 && census.missing_partitions.empty();
	return ok ? 0 : 1;
}

int cmd_text_tsv(const EbxContext &p_ctx, const std::string &p_out) {
	const std::filesystem::path path(widen(p_out));
	const PathGuard::Verdict verdict = p_ctx.reader->write_guard().check_write_target(path);
	if (verdict != PathGuard::Verdict::Allowed) {
		std::fprintf(stderr, "refusing to write %s: %s\n", p_out.c_str(), PathGuard::verdict_name(verdict));
		return 1;
	}
	const std::optional<TextDatabase> db = open_db(p_ctx, nullptr);
	if (!db) {
		return 1;
	}
	std::string text;
	for (size_t i = 0; i < db->size(); ++i) {
		text += hex8(db->hash_at(i));
		text += '\t';
		text += escaped(db->text_at(i));
		text += '\n';
	}
	FILE *f = nullptr;
	if (_wfopen_s(&f, to_extended_length(path).c_str(), L"wb") != 0 || f == nullptr) {
		std::fprintf(stderr, "cannot create %s\n", p_out.c_str());
		return 1;
	}
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
	std::fclose(f);
	std::fprintf(stderr, "text --tsv: %zu entries -> %s\n", db->size(), p_out.c_str());
	return ok ? 0 : 1;
}

}

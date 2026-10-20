#include "bf3/core/ui/text_db.h"

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/payload.h"
#include "bf3/core/sha1_digest.h"
#include "../format_errors.h"

#include <algorithm>
#include <cstring>

namespace bf3::core::ui {

namespace {

constexpr size_t kBinaryHeader = 0x14;
constexpr size_t kHistogramHeader = 12;
constexpr size_t kEntryLen = 8;
constexpr size_t kOffsetBase = 8;

constexpr size_t kMaxChunkLen = size_t(256) << 20;

constexpr uint64_t kMaxDecodeFactor = 4;
constexpr uint32_t kReplacement = 0xFFFD;

uint16_t le16(const uint8_t *p_at) {
	return uint16_t(p_at[0] | (p_at[1] << 8));
}
uint32_t le32(const uint8_t *p_at) {
	return uint32_t(p_at[0]) | (uint32_t(p_at[1]) << 8) | (uint32_t(p_at[2]) << 16) | (uint32_t(p_at[3]) << 24);
}

Error format_error(const char *p_what, const std::string &p_problem) {
	return make_error(std::string(p_what) + ": " + p_problem);
}

void append_utf8(std::string &r_out, uint32_t p_cp) {
	if (p_cp < 0x80) {
		r_out += char(p_cp);
	} else if (p_cp < 0x800) {
		r_out += char(0xC0 | (p_cp >> 6));
		r_out += char(0x80 | (p_cp & 0x3F));
	} else if (p_cp < 0x10000) {
		r_out += char(0xE0 | (p_cp >> 12));
		r_out += char(0x80 | ((p_cp >> 6) & 0x3F));
		r_out += char(0x80 | (p_cp & 0x3F));
	} else {
		r_out += char(0xF0 | (p_cp >> 18));
		r_out += char(0x80 | ((p_cp >> 12) & 0x3F));
		r_out += char(0x80 | ((p_cp >> 6) & 0x3F));
		r_out += char(0x80 | (p_cp & 0x3F));
	}
}

class Utf16Sink {
public:
	Utf16Sink(std::string &r_out, size_t &r_errors) :
			out_(r_out), errors_(r_errors) {}

	void push(uint16_t p_unit) {
		if (high_ != 0) {
			if (p_unit >= 0xDC00 && p_unit <= 0xDFFF) {
				append_utf8(out_, 0x10000 + ((uint32_t(high_) - 0xD800) << 10) + (uint32_t(p_unit) - 0xDC00));
				high_ = 0;
				return;
			}
			lone();
			high_ = 0;
		}
		if (p_unit >= 0xD800 && p_unit <= 0xDBFF) {
			high_ = p_unit;
		} else if (p_unit >= 0xDC00 && p_unit <= 0xDFFF) {
			lone();
		} else {
			append_utf8(out_, p_unit);
		}
	}
	void finish() {
		if (high_ != 0) {
			lone();
			high_ = 0;
		}
	}

private:
	void lone() {
		append_utf8(out_, kReplacement);
		++errors_;
	}

	std::string &out_;
	size_t &errors_;
	uint16_t high_ = 0;
};

void decode_string(std::span<const uint8_t> p_encoded, std::span<const uint8_t> p_units, size_t p_unit_count, std::string &r_out,
		size_t &r_errors) {
	auto unit = [&](size_t p_index) { return le16(p_units.data() + 2 * p_index); };
	Utf16Sink sink(r_out, r_errors);
	for (size_t i = 0; i < p_encoded.size(); ++i) {
		const uint8_t b = p_encoded[i];
		if (b < 0x80) {
			sink.push(b);
			continue;
		}
		const uint16_t t = unit(b);
		if (t >= 0x80) {
			sink.push(t);
			continue;
		}

		if (i + 1 >= p_encoded.size()) {
			++r_errors;
			break;
		}
		const uint8_t n = p_encoded[++i];
		if (n < 0x80) {
			++r_errors;
			continue;
		}
		const size_t index = size_t(n - 0x80) + (size_t(t) << 7);
		if (index >= p_unit_count) {
			++r_errors;
			continue;
		}
		sink.push(unit(index));
	}
	sink.finish();
}

}

uint32_t text_hash(std::string_view p_sid) {
	uint32_t h = 0xFFFFFFFFu;
	for (const char c : p_sid) {
		h = h * 33u + uint8_t(c);
	}
	return h;
}

Result<TextDatabase> TextDatabase::parse(std::span<const uint8_t> p_binary, std::span<const uint8_t> p_histogram) {
	static const char *const kBin = "text database binary chunk";
	static const char *const kHist = "text database histogram chunk";

	const size_t hlen = p_histogram.size();
	if (hlen > kMaxChunkLen) {
		return format_error(kHist, "is larger than " + std::to_string(kMaxChunkLen) + " bytes");
	}
	if (hlen < kHistogramHeader) {
		return format_error(kHist, "is truncated: " + std::to_string(hlen) + " bytes");
	}
	if (le32(p_histogram.data()) != kTextHistogramMagic) {
		return format_error(kHist, "has bad magic " + detail::hex_prefixed(le32(p_histogram.data()), 8));
	}
	if (uint64_t(le32(p_histogram.data() + 4)) + kOffsetBase != hlen) {
		return format_error(kHist, "size field " + std::to_string(le32(p_histogram.data() + 4)) + " does not match its length " + std::to_string(hlen));
	}
	if ((hlen - kHistogramHeader) % 2 != 0) {
		return format_error(kHist, "has an odd number of code unit bytes");
	}
	const size_t units = (hlen - kHistogramHeader) / 2;
	if (units < kTextHistogramMinUnits) {
		return format_error(kHist, "holds " + std::to_string(units) + " code units, fewer than " + std::to_string(kTextHistogramMinUnits));
	}
	const std::span<const uint8_t> unit_bytes = p_histogram.subspan(kHistogramHeader);

	const size_t len = p_binary.size();
	if (len > kMaxChunkLen) {
		return format_error(kBin, "is larger than " + std::to_string(kMaxChunkLen) + " bytes");
	}
	if (len < kBinaryHeader + 1) {
		return format_error(kBin, "is truncated: " + std::to_string(len) + " bytes");
	}
	const uint8_t *b = p_binary.data();
	if (le32(b) != kTextBinaryMagic) {
		return format_error(kBin, "has bad magic " + detail::hex_prefixed(le32(b), 8));
	}
	if (uint64_t(le32(b + 4)) + kOffsetBase != len) {
		return format_error(kBin, "size field " + std::to_string(le32(b + 4)) + " does not match its length " + std::to_string(len));
	}
	const uint32_t count = le32(b + 8);
	const uint64_t table_at = uint64_t(le32(b + 12)) + kOffsetBase;
	const uint64_t strings_at = uint64_t(le32(b + 16)) + kOffsetBase;
	const uint64_t table_end = table_at + uint64_t(count) * kEntryLen;
	if (table_at <= kBinaryHeader || table_end > len) {
		return format_error(kBin, "entry table [" + std::to_string(table_at) + ", " + std::to_string(table_end) + ") is outside the chunk");
	}
	if (strings_at != table_end) {
		return format_error(kBin, "string table offset " + std::to_string(strings_at) + " does not follow the entry table end " + std::to_string(table_end));
	}

	const void *tag_nul = std::memchr(b + kBinaryHeader, 0, size_t(table_at) - kBinaryHeader);
	if (tag_nul == nullptr) {
		return format_error(kBin, "tag is not NUL-terminated before the entry table");
	}

	TextDatabase db;
	db.tag_.assign(reinterpret_cast<const char *>(b + kBinaryHeader), static_cast<const uint8_t *>(tag_nul) - (b + kBinaryHeader));
	db.histogram_count_ = le32(p_histogram.data() + 8);
	db.histogram_units_ = units;
	db.entries_.reserve(count);
	uint64_t decoded_budget = uint64_t(len) * kMaxDecodeFactor;
	for (uint32_t i = 0; i < count; ++i) {
		const uint8_t *e = b + table_at + uint64_t(i) * kEntryLen;
		const uint32_t hash = le32(e);
		const uint64_t at = strings_at + le32(e + 4);
		if (at >= len) {
			return format_error(kBin, "entry " + std::to_string(i) + " string offset " + std::to_string(at) + " is outside the chunk");
		}
		const void *nul = std::memchr(b + at, 0, size_t(len - at));
		if (nul == nullptr) {
			return format_error(kBin, "entry " + std::to_string(i) + " string is not NUL-terminated");
		}
		const size_t encoded_len = static_cast<const uint8_t *>(nul) - (b + at);
		if (encoded_len > decoded_budget) {
			return format_error(kBin, "entry strings overlap beyond " + std::to_string(kMaxDecodeFactor) + "x the chunk length");
		}
		decoded_budget -= encoded_len;
		const size_t text_at = db.text_.size();
		decode_string(std::span<const uint8_t>(b + at, encoded_len), unit_bytes, units, db.text_, db.decode_errors_);
		db.entries_.push_back(Entry{ hash, uint32_t(text_at), uint32_t(db.text_.size() - text_at) });
	}

	std::stable_sort(db.entries_.begin(), db.entries_.end(), [](const Entry &p_a, const Entry &p_b) { return p_a.hash < p_b.hash; });
	const auto last = std::unique(db.entries_.begin(), db.entries_.end(), [](const Entry &p_a, const Entry &p_b) { return p_a.hash == p_b.hash; });
	db.duplicate_hashes_ = size_t(db.entries_.end() - last);
	db.entries_.erase(last, db.entries_.end());
	return db;
}

std::string_view TextDatabase::text_at(size_t p_index) const {
	const Entry &e = entries_[p_index];
	return std::string_view(text_).substr(e.text_at, e.text_len);
}

std::optional<std::string_view> TextDatabase::find(uint32_t p_hash) const {
	const auto it = std::lower_bound(entries_.begin(), entries_.end(), p_hash, [](const Entry &p_e, uint32_t p_h) { return p_e.hash < p_h; });
	if (it == entries_.end() || it->hash != p_hash) {
		return std::nullopt;
	}
	return text_at(size_t(it - entries_.begin()));
}

bool TextDatabase::resolves(std::string_view p_sid) const {
	const std::optional<std::string_view> text = find_sid(p_sid);
	return text && *text != kTextMissingMarker;
}

std::string_view TextDatabase::translate(std::string_view p_sid) const {
	const std::optional<std::string_view> text = find_sid(p_sid);
	return (text && *text != kTextMissingMarker) ? *text : p_sid;
}

namespace {

constexpr std::string_view kLocalizationPartition = "localization/localization";
constexpr std::string_view kLocPrefix = "win32/loc/";

std::string loc_code(std::string_view p_language) {
	static const struct {
		const char *word;
		const char *code;
	} kCodes[] = {
		{ "english", "en" }, { "german", "de" }, { "spanish", "es" }, { "french", "fr" }, { "italian", "it" }, { "russian", "ru" },
		{ "japanese", "ja" }, { "czech", "cs" }, { "korean", "ko" }, { "polish", "pl" }, { "chinese", "zh" },
	};
	const std::string l = to_lower_ascii(p_language);
	for (const auto &c : kCodes) {
		if (l.find(c.word) != std::string::npos) {
			return c.code;
		}
	}
	return {};
}

std::optional<EbxInstance> text_database_instance(const EbxPartition &p_partition) {
	if (p_partition.primary().is_a("UITextDatabase")) {
		return p_partition.primary();
	}
	const std::vector<EbxInstance> all = p_partition.instances_of("UITextDatabase");
	if (all.empty()) {
		return std::nullopt;
	}
	return all.front();
}

Result<std::optional<TextDatabase>> load_if_installed(const Install &p_install, const TextDatabaseRef &p_ref, TextDatabaseSource *r_source) {
	std::vector<std::string> sites;
	const std::string code = loc_code(p_ref.language);
	if (!code.empty() && p_install.site("Win32/Loc/" + code) != nullptr) {
		sites.push_back(p_install.site("Win32/Loc/" + code)->name);
	}
	for (const SuperbundleSite &s : p_install.sites()) {
		if (to_lower_ascii(s.name).rfind(kLocPrefix, 0) == 0 && (s.base_toc || s.patch_toc) &&
				std::find(sites.begin(), sites.end(), s.name) == sites.end()) {
			sites.push_back(s.name);
		}
	}
	for (const std::string &sb : sites) {
		Result<std::optional<std::vector<uint8_t>>> binary = read_toc_chunk(p_install, sb, p_ref.binary_chunk);
		if (!binary) {
			return binary.error();
		}
		if (!*binary) {
			continue;
		}
		Result<std::optional<std::vector<uint8_t>>> histogram = read_toc_chunk(p_install, sb, p_ref.histogram_chunk);
		if (!histogram) {
			return histogram.error();
		}
		if (!*histogram) {
			return make_error(p_ref.partition + ": " + sb + " holds the binary chunk but not the histogram chunk " + p_ref.histogram_chunk.to_string());
		}
		const std::vector<uint8_t> &bin = **binary;
		const std::vector<uint8_t> &hist = **histogram;
		if (bin.size() != p_ref.binary_size) {
			return make_error(p_ref.partition + ": binary chunk decodes to " + std::to_string(bin.size()) + " bytes, BinaryChunkSize is " +
					std::to_string(p_ref.binary_size));
		}
		if (hist.size() != p_ref.histogram_size) {
			return make_error(p_ref.partition + ": histogram chunk decodes to " + std::to_string(hist.size()) + " bytes, HistogramChunkSize is " +
					std::to_string(p_ref.histogram_size));
		}
		Result<TextDatabase> db = TextDatabase::parse(bin, hist);
		if (!db) {
			return error_at(p_ref.partition, db.error());
		}
		if (r_source != nullptr) {
			r_source->ref = p_ref;
			r_source->superbundle = sb;
			r_source->binary_sha1 = sha1_of(bin);
			r_source->histogram_sha1 = sha1_of(hist);
		}
		return std::optional<TextDatabase>(std::move(*db));
	}
	return std::optional<TextDatabase>();
}

}

Result<TextDatabaseRef> text_database_ref(const EbxResolver &p_ebx, std::string_view p_partition) {
	const std::string key = to_lower_ascii(p_partition);
	const EbxPartition *partition = p_ebx.partition_by_name(key);
	if (partition == nullptr) {
		return make_error(key + ": no such EBX partition");
	}
	const std::optional<EbxInstance> inst = text_database_instance(*partition);
	if (!inst) {
		return make_error(key + ": no UITextDatabase instance");
	}
	TextDatabaseRef ref;
	ref.partition = key;
	const std::optional<EbxValue> name = inst->get("Name");
	const std::optional<EbxValue> language = inst->get("Language");
	const std::optional<EbxValue> bin = inst->get("BinaryChunk");
	const std::optional<EbxValue> bin_size = inst->get("BinaryChunkSize");
	const std::optional<EbxValue> hist = inst->get("HistogramChunk");
	const std::optional<EbxValue> hist_size = inst->get("HistogramChunkSize");
	if (!bin || !bin->as_guid() || !hist || !hist->as_guid() || !bin_size || !bin_size->as_u32() || !hist_size || !hist_size->as_u32()) {
		return make_error(key + ": UITextDatabase lacks BinaryChunk/HistogramChunk and their sizes");
	}
	if (name && name->as_str()) {
		ref.name = std::string(*name->as_str());
	}
	if (language && language->enum_name()) {
		ref.language = std::string(*language->enum_name());
	}
	ref.binary_chunk = *bin->as_guid();
	ref.binary_size = *bin_size->as_u32();
	ref.histogram_chunk = *hist->as_guid();
	ref.histogram_size = *hist_size->as_u32();
	return ref;
}

Result<std::vector<std::string>> localized_text_partitions(const EbxResolver &p_ebx) {
	const EbxPartition *partition = p_ebx.partition_by_name(kLocalizationPartition);
	if (partition == nullptr) {
		return make_error(std::string(kLocalizationPartition) + ": no such EBX partition");
	}
	const std::optional<EbxValue> texts = partition->primary().get("LocalizedTexts");
	if (!texts || !texts->as_array()) {
		return make_error(std::string(kLocalizationPartition) + ": no LocalizedTexts array");
	}
	std::vector<std::string> out;
	for (const EbxValue e : *texts->as_array()) {
		const std::optional<EbxRef> ref = e.as_ref();
		if (!ref || ref->is_null()) {
			continue;
		}
		const Guid owner = ref->kind == EbxRef::Kind::External ? ref->partition : partition->guid();
		const std::optional<std::string_view> name = p_ebx.name_of(owner);
		if (name) {
			out.emplace_back(*name);
		}
	}
	return out;
}

Result<std::optional<std::vector<uint8_t>>> read_toc_chunk(const Install &p_install, std::string_view p_superbundle, const Guid &p_id) {
	const SuperbundleSite *site = p_install.site(p_superbundle);
	if (site == nullptr) {
		return make_error("no superbundle `" + std::string(p_superbundle) + "`");
	}
	Result<std::shared_ptr<const TocChunkMap>> map = p_install.chunk_toc(*site);
	if (!map) {
		return map.error();
	}
	const auto it = (*map)->find(p_id);
	if (it == (*map)->end()) {
		return std::optional<std::vector<uint8_t>>();
	}
	const TocChunkSite &located = it->second;
	Result<std::vector<uint8_t>> stored = located.in_cas ? p_install.cas().read(located.sha1) : located.file->read_vec(located.offset, located.size);
	if (!stored) {
		return stored.error();
	}
	Result<std::vector<uint8_t>> decoded = decode_chunk(p_id, *stored);
	if (!decoded) {
		return error_at("chunk " + p_id.to_string() + " of " + site->name, decoded.error());
	}
	return std::optional<std::vector<uint8_t>>(std::move(*decoded));
}

Result<TextDatabase> load_text_database(const Install &p_install, const TextDatabaseRef &p_ref, TextDatabaseSource *r_source) {
	Result<std::optional<TextDatabase>> db = load_if_installed(p_install, p_ref, r_source);
	if (!db) {
		return db.error();
	}
	if (!*db) {
		return make_error(p_ref.partition + ": chunk " + p_ref.binary_chunk.to_string() + " is in no installed Win32/Loc superbundle");
	}
	return std::move(**db);
}

Result<TextDatabase> load_text_database(const Install &p_install, const EbxResolver &p_ebx, std::string_view p_partition, TextDatabaseSource *r_source) {
	if (!p_partition.empty()) {
		Result<TextDatabaseRef> ref = text_database_ref(p_ebx, p_partition);
		if (!ref) {
			return ref.error();
		}
		return load_text_database(p_install, *ref, r_source);
	}
	Result<std::vector<std::string>> names = localized_text_partitions(p_ebx);
	if (!names) {
		return names.error();
	}
	for (const std::string &name : *names) {
		Result<TextDatabaseRef> ref = text_database_ref(p_ebx, name);
		if (!ref) {
			return ref.error();
		}
		Result<std::optional<TextDatabase>> db = load_if_installed(p_install, *ref, r_source);
		if (!db) {
			return db.error();
		}
		if (*db) {
			return std::move(**db);
		}
	}
	return make_error(std::string(kLocalizationPartition) + ": none of its " + std::to_string(names->size()) + " text databases is installed");
}

Result<TextDatabase> open_text_database(std::shared_ptr<const Install> p_install, std::string_view p_partition, TextDatabaseSource *r_source) {
	const Install &install = *p_install;
	Result<std::unique_ptr<EbxLibrary>> globals = EbxLibrary::open(std::move(p_install), { "Win32/Globals" });
	if (!globals) {
		return globals.error();
	}
	return load_text_database(install, **globals, p_partition, r_source);
}

}

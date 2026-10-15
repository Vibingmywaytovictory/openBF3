#pragma once
// BF3's localised UI strings: the UITextDatabase (e.g. `localization/us_loc`, English). Clean-room
// implementation from research/topics/gap-r1-ui-hud-minimap-localisation.md (§4); plan §3.8, §7.8.
//
// Where the strings are: `localization/localization` (LocalizationAsset, Win32/Globals) lists the
// UITextDatabase partitions in LocalizedTexts. Each names two toc-level chunks (BinaryChunk,
// HistogramChunk, with their decoded sizes) of the chunk-only superbundle Win32/Loc/<lang>; the patch
// toc's copy of a chunk wins over the base toc's, as for every superbundle.
//
// Binary chunk (little-endian):
//   0x00 u32 magic 0x00039000 | u32 size (= chunk length - 8) | u32 count | u32 dataOff | u32 strOff
//   0x14 NUL-terminated tag ("Global"), padded up to dataOff + 8
//   dataOff + 8: count x (u32 hash, u32 offset), sorted by hash in retail data; ends at strOff + 8
//   strOff + 8 + offset: the entry's encoded string, NUL-terminated
// Histogram chunk:
//   0x00 u32 magic 0x00039001 | u32 size (= chunk length - 8) | u32 count, then u16 code units to the end
// String decoding, one byte b at a time: b < 0x80 is that ASCII character; else t = hist[b]: t >= 0x80
// is the UTF-16 code unit itself, otherwise t selects a page and the next byte n (>= 0x80) gives
// hist[(n - 0x80) + (t << 7)]. Decoded text is kept as UTF-8.
//
// Keys: text_hash(SID) over the SID's bytes exactly as written (SIDs are upper case):
// h = 0xFFFFFFFF; h = h * 33 + c (mod 2^32). Additive, unlike ebx_name_hash / asset_name_hash (xor).
// A missing SID shows as the SID itself (plan §2.1); some entries hold the literal "<MISSING>".

#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
class EbxResolver;
} // namespace bf3::core

namespace bf3::core::ui {

inline constexpr uint32_t kTextBinaryMagic = 0x00039000;
inline constexpr uint32_t kTextHistogramMagic = 0x00039001;
// Code units a histogram must hold: hist[b] is read for every byte 0x80..0xFF.
inline constexpr size_t kTextHistogramMinUnits = 256;
// What some retail entries hold instead of a translation.
inline constexpr std::string_view kTextMissingMarker = "<MISSING>";

// The SID hash of the text database (see the header comment).
uint32_t text_hash(std::string_view p_sid);

// One parsed text database: (hash, UTF-8 text) entries sorted by hash.
class TextDatabase {
public:
	// Validates both chunks in full (headers, entry table, every string offset and terminator) and
	// decodes every string. Malformed byte sequences inside a string do not fail the parse: they are
	// counted (decode_errors) and the string keeps what decoded (lone surrogates become U+FFFD).
	static Result<TextDatabase> parse(std::span<const uint8_t> p_binary, std::span<const uint8_t> p_histogram);

	TextDatabase() = default;

	const std::string &tag() const { return tag_; }
	size_t size() const { return entries_.size(); }
	bool empty() const { return entries_.empty(); }
	// Entry p_index in hash order.
	uint32_t hash_at(size_t p_index) const { return entries_[p_index].hash; }
	std::string_view text_at(size_t p_index) const;

	// Malformed sequences over all strings: a page byte at the end of a string or followed by a byte
	// below 0x80, a page index past the histogram, a lone surrogate.
	size_t decode_errors() const { return decode_errors_; }
	// Entries whose hash repeats an earlier entry's (the first in file order is kept).
	size_t duplicate_hashes() const { return duplicate_hashes_; }
	// Header fields kept for tools.
	uint32_t histogram_count() const { return histogram_count_; }
	size_t histogram_units() const { return histogram_units_; }

	std::optional<std::string_view> find(uint32_t p_hash) const;
	std::optional<std::string_view> find_sid(std::string_view p_sid) const { return find(text_hash(p_sid)); }
	// Present and not the "<MISSING>" marker.
	bool resolves(std::string_view p_sid) const;
	// The text of a SID, or the SID itself when the database has no usable entry for it.
	std::string_view translate(std::string_view p_sid) const;

private:
	struct Entry {
		uint32_t hash;
		uint32_t text_at; // into text_
		uint32_t text_len;
	};

	std::string tag_;
	std::string text_; // every decoded string, back to back
	std::vector<Entry> entries_;
	size_t decode_errors_ = 0;
	size_t duplicate_hashes_ = 0;
	uint32_t histogram_count_ = 0;
	size_t histogram_units_ = 0;
};

// What a UITextDatabase partition names (its primary instance's fields).
struct TextDatabaseRef {
	std::string partition; // lowercase partition name, e.g. "localization/us_loc"
	std::string name; // UITextDatabase.Name
	std::string language; // LanguageFormat enumerator name, e.g. "LanguageFormat_English"
	Guid binary_chunk;
	uint32_t binary_size = 0;
	Guid histogram_chunk;
	uint32_t histogram_size = 0;
};

// Reads the UITextDatabase fields of a partition.
Result<TextDatabaseRef> text_database_ref(const EbxResolver &p_ebx, std::string_view p_partition);
// The UITextDatabase partitions LocalizationAsset `localization/localization` lists, in its order.
Result<std::vector<std::string>> localized_text_partitions(const EbxResolver &p_ebx);

// Where a text database was read from.
struct TextDatabaseSource {
	TextDatabaseRef ref;
	std::string superbundle; // the Win32/Loc/<lang> superbundle whose toc holds the chunks
	Sha1 binary_sha1; // of the decoded chunks
	Sha1 histogram_sha1;
};

// Reads a toc-level chunk of a chunk superbundle (base toc, then patch toc; patch wins), decoded.
// nullopt when the superbundle's tocs do not list the chunk.
Result<std::optional<std::vector<uint8_t>>> read_toc_chunk(const Install &p_install, std::string_view p_superbundle, const Guid &p_id);

// Finds the chunks of p_ref in the Win32/Loc/<lang> superbundles (the one named after its language
// first), checks their decoded sizes and parses them.
Result<TextDatabase> load_text_database(const Install &p_install, const TextDatabaseRef &p_ref, TextDatabaseSource *r_source = nullptr);
// p_partition empty: the first database of `localization/localization` whose chunks are installed
// (`localization/us_loc` on an English install). p_ebx must serve Win32/Globals.
Result<TextDatabase> load_text_database(const Install &p_install, const EbxResolver &p_ebx, std::string_view p_partition = {},
		TextDatabaseSource *r_source = nullptr);
// The same over an EbxLibrary of Win32/Globals opened (lazily) for the call.
Result<TextDatabase> open_text_database(std::shared_ptr<const Install> p_install, std::string_view p_partition = {},
		TextDatabaseSource *r_source = nullptr);

} // namespace bf3::core::ui

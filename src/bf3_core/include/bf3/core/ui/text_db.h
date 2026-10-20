#pragma once

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
}

namespace bf3::core::ui {

inline constexpr uint32_t kTextBinaryMagic = 0x00039000;
inline constexpr uint32_t kTextHistogramMagic = 0x00039001;

inline constexpr size_t kTextHistogramMinUnits = 256;

inline constexpr std::string_view kTextMissingMarker = "<MISSING>";

uint32_t text_hash(std::string_view p_sid);

class TextDatabase {
public:

	static Result<TextDatabase> parse(std::span<const uint8_t> p_binary, std::span<const uint8_t> p_histogram);

	TextDatabase() = default;

	const std::string &tag() const { return tag_; }
	size_t size() const { return entries_.size(); }
	bool empty() const { return entries_.empty(); }

	uint32_t hash_at(size_t p_index) const { return entries_[p_index].hash; }
	std::string_view text_at(size_t p_index) const;

	size_t decode_errors() const { return decode_errors_; }

	size_t duplicate_hashes() const { return duplicate_hashes_; }

	uint32_t histogram_count() const { return histogram_count_; }
	size_t histogram_units() const { return histogram_units_; }

	std::optional<std::string_view> find(uint32_t p_hash) const;
	std::optional<std::string_view> find_sid(std::string_view p_sid) const { return find(text_hash(p_sid)); }

	bool resolves(std::string_view p_sid) const;

	std::string_view translate(std::string_view p_sid) const;

private:
	struct Entry {
		uint32_t hash;
		uint32_t text_at;
		uint32_t text_len;
	};

	std::string tag_;
	std::string text_;
	std::vector<Entry> entries_;
	size_t decode_errors_ = 0;
	size_t duplicate_hashes_ = 0;
	uint32_t histogram_count_ = 0;
	size_t histogram_units_ = 0;
};

struct TextDatabaseRef {
	std::string partition;
	std::string name;
	std::string language;
	Guid binary_chunk;
	uint32_t binary_size = 0;
	Guid histogram_chunk;
	uint32_t histogram_size = 0;
};

Result<TextDatabaseRef> text_database_ref(const EbxResolver &p_ebx, std::string_view p_partition);

Result<std::vector<std::string>> localized_text_partitions(const EbxResolver &p_ebx);

struct TextDatabaseSource {
	TextDatabaseRef ref;
	std::string superbundle;
	Sha1 binary_sha1;
	Sha1 histogram_sha1;
};

Result<std::optional<std::vector<uint8_t>>> read_toc_chunk(const Install &p_install, std::string_view p_superbundle, const Guid &p_id);

Result<TextDatabase> load_text_database(const Install &p_install, const TextDatabaseRef &p_ref, TextDatabaseSource *r_source = nullptr);

Result<TextDatabase> load_text_database(const Install &p_install, const EbxResolver &p_ebx, std::string_view p_partition = {},
		TextDatabaseSource *r_source = nullptr);

Result<TextDatabase> open_text_database(std::shared_ptr<const Install> p_install, std::string_view p_partition = {},
		TextDatabaseSource *r_source = nullptr);

}

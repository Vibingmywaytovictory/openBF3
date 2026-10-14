// Cache key derivation and the entry header codec (portable).
#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/entry_format.h"

#include <array>
#include <cstring>

namespace bf3::core {

namespace {

// Field tags of the canonical key encoding. Never renumber: keys must stay stable across builds.
enum KeyTag : uint8_t {
	kTagKind = 1,
	kTagSource = 2,
	kTagConverter = 3,
	kTagEngine = 4,
	kTagFormat = 5,
	kTagJolt = 6,
	kTagParams = 7,
};

constexpr char kKeyDomain[] = "openbf3.cache.key.v1";
constexpr char kEntryMagic[8] = { 'O', 'B', 'F', '3', 'C', 'A', 'C', 'H' };

void put_u32(Hasher &r_h, uint32_t p_v) {
	const uint8_t b[4] = { uint8_t(p_v), uint8_t(p_v >> 8), uint8_t(p_v >> 16), uint8_t(p_v >> 24) };
	r_h.update(b);
}

// tag | u32 length (LE) | bytes
void put_field(Hasher &r_h, uint8_t p_tag, const void *p_data, size_t p_size) {
	r_h.update(std::span<const uint8_t>(&p_tag, 1));
	put_u32(r_h, static_cast<uint32_t>(p_size));
	r_h.update(std::span<const uint8_t>(static_cast<const uint8_t *>(p_data), p_size));
}

void put_u32_field(Hasher &r_h, uint8_t p_tag, uint32_t p_v) {
	const uint8_t b[4] = { uint8_t(p_v), uint8_t(p_v >> 8), uint8_t(p_v >> 16), uint8_t(p_v >> 24) };
	put_field(r_h, p_tag, b, 4);
}

void store_u32(uint8_t *p_dst, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		p_dst[i] = uint8_t(p_v >> (8 * i));
	}
}

void store_u64(uint8_t *p_dst, uint64_t p_v) {
	for (int i = 0; i < 8; ++i) {
		p_dst[i] = uint8_t(p_v >> (8 * i));
	}
}

uint32_t load_u32(const uint8_t *p_src) {
	uint32_t v = 0;
	for (int i = 3; i >= 0; --i) {
		v = (v << 8) | p_src[i];
	}
	return v;
}

uint64_t load_u64(const uint8_t *p_src) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) {
		v = (v << 8) | p_src[i];
	}
	return v;
}

constexpr std::array<uint32_t, 256> make_crc_table() {
	std::array<uint32_t, 256> t{};
	for (uint32_t i = 0; i < 256; ++i) {
		uint32_t c = i;
		for (int k = 0; k < 8; ++k) {
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		}
		t[i] = c;
	}
	return t;
}

constexpr std::array<uint32_t, 256> kCrcTable = make_crc_table();

} // namespace

uint32_t crc32(std::span<const uint8_t> p_bytes, uint32_t p_seed) {
	uint32_t c = ~p_seed;
	for (uint8_t b : p_bytes) {
		c = kCrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
	}
	return ~c;
}

bool is_valid_cache_kind(std::string_view p_kind) {
	if (p_kind.empty() || p_kind.size() > kCacheKindMaxLength) {
		return false;
	}
	for (char c : p_kind) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

std::filesystem::path CacheKey::relative_path() const {
	const std::string h = digest.hex();
	return std::filesystem::path("v" + std::to_string(kCacheContainerVersion)) / kind / h.substr(0, 2) / (h + ".bin");
}

std::optional<CacheKey> make_cache_key(const CacheKeyInputs &p_inputs) {
	if (!is_valid_cache_kind(p_inputs.kind) || p_inputs.sources.size() > kCacheKeyMaxSources ||
			p_inputs.engine.size() > kCacheKeyFieldMaxLength || p_inputs.jolt_version.size() > kCacheKeyFieldMaxLength ||
			p_inputs.params.size() > kCacheKeyFieldMaxLength) {
		return std::nullopt;
	}
	Hasher h(HashAlgorithm::Sha256);
	h.update(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(kKeyDomain), sizeof kKeyDomain)); // includes the NUL
	put_field(h, kTagKind, p_inputs.kind.data(), p_inputs.kind.size());
	put_u32(h, static_cast<uint32_t>(p_inputs.sources.size()));
	for (const Sha1Digest &s : p_inputs.sources) {
		put_field(h, kTagSource, s.bytes.data(), s.bytes.size());
	}
	put_u32_field(h, kTagConverter, p_inputs.converter_version);
	put_field(h, kTagEngine, p_inputs.engine.data(), p_inputs.engine.size());
	put_u32_field(h, kTagFormat, p_inputs.format_version);
	// Optional fields are absent (not empty) when unused, so adding one later keeps old keys.
	if (!p_inputs.jolt_version.empty()) {
		put_field(h, kTagJolt, p_inputs.jolt_version.data(), p_inputs.jolt_version.size());
	}
	if (!p_inputs.params.empty()) {
		put_field(h, kTagParams, p_inputs.params.data(), p_inputs.params.size());
	}
	CacheKey key;
	key.kind = p_inputs.kind;
	if (!h.finish(key.digest.bytes)) {
		return std::nullopt;
	}
	return key;
}

void encode_entry_header(const CacheEntryHeader &p_header, std::span<uint8_t, kCacheEntryHeaderSize> r_out) {
	uint8_t *o = r_out.data();
	std::memset(o, 0, kCacheEntryHeaderSize);
	std::memcpy(o, kEntryMagic, 8);
	store_u32(o + 8, kCacheContainerVersion);
	store_u32(o + 12, static_cast<uint32_t>(kCacheEntryHeaderSize));
	std::memcpy(o + 16, p_header.key.bytes.data(), 32);
	store_u64(o + 48, p_header.payload_size);
	std::memcpy(o + 56, p_header.payload_sha256.bytes.data(), 32);
	const size_t n = p_header.kind.size() < kCacheKindMaxLength ? p_header.kind.size() : kCacheKindMaxLength;
	std::memcpy(o + 88, p_header.kind.data(), n);
	store_u32(o + 120, 0);
	store_u32(o + 124, crc32(std::span<const uint8_t>(o, 124)));
}

EntryHeaderCheck decode_entry_header(std::span<const uint8_t> p_file, CacheEntryHeader &r_header) {
	return decode_entry_head(p_file, p_file.size(), r_header);
}

EntryHeaderCheck decode_entry_head(std::span<const uint8_t> p_head, uint64_t p_file_size, CacheEntryHeader &r_header) {
	if (p_head.size() < kCacheEntryHeaderSize || p_file_size < kCacheEntryHeaderSize) {
		return EntryHeaderCheck::TooSmall;
	}
	const uint8_t *p = p_head.data();
	if (std::memcmp(p, kEntryMagic, 8) != 0) {
		return EntryHeaderCheck::BadMagic;
	}
	if (load_u32(p + 8) != kCacheContainerVersion || load_u32(p + 12) != kCacheEntryHeaderSize) {
		return EntryHeaderCheck::BadVersion;
	}
	if (load_u32(p + 124) != crc32(std::span<const uint8_t>(p, 124))) {
		return EntryHeaderCheck::BadCrc;
	}
	std::memcpy(r_header.key.bytes.data(), p + 16, 32);
	r_header.payload_size = load_u64(p + 48);
	std::memcpy(r_header.payload_sha256.bytes.data(), p + 56, 32);
	size_t kind_len = 0;
	while (kind_len < kCacheKindMaxLength && p[88 + kind_len] != 0) {
		++kind_len;
	}
	r_header.kind.assign(reinterpret_cast<const char *>(p + 88), kind_len);
	if (!is_valid_cache_kind(r_header.kind)) {
		return EntryHeaderCheck::BadKind;
	}
	// Overflow-safe: payload_size is untrusted.
	if (r_header.payload_size > kCacheMaxPayload || r_header.payload_size != p_file_size - kCacheEntryHeaderSize) {
		return EntryHeaderCheck::SizeMismatch;
	}
	return EntryHeaderCheck::Ok;
}

const char *entry_header_check_name(EntryHeaderCheck p_check) {
	switch (p_check) {
		case EntryHeaderCheck::Ok:
			return "ok";
		case EntryHeaderCheck::TooSmall:
			return "too small";
		case EntryHeaderCheck::BadMagic:
			return "bad magic";
		case EntryHeaderCheck::BadVersion:
			return "bad version";
		case EntryHeaderCheck::BadCrc:
			return "bad header crc";
		case EntryHeaderCheck::BadKind:
			return "bad kind";
		case EntryHeaderCheck::KeyMismatch:
			return "key mismatch";
		case EntryHeaderCheck::SizeMismatch:
			return "size mismatch";
	}
	return "?";
}

} // namespace bf3::core

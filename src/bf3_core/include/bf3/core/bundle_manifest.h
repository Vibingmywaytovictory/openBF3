#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::core {

constexpr uint32_t kBinaryManifestMagic = 0x970D1C13u;
constexpr uint32_t kBinaryManifestAlphaMagic = 0x0D1CEDB8u;

struct Payload {
	enum class Kind : uint8_t {
		Cas,
		Inline,
		Bundle,
	};
	Kind kind = Kind::Cas;
	uint64_t offset = 0;
	std::shared_ptr<const std::vector<uint8_t>> inline_data;
};

struct EbxEntry {
	std::string name;
	Sha1 sha1;
	uint64_t stored_size = 0;
	uint64_t original_size = 0;
	Payload payload;
	bool is_framed() const { return stored_size != original_size; }
};

struct ResEntry {
	std::string name;
	Sha1 sha1;
	uint64_t stored_size = 0;
	uint64_t original_size = 0;
	uint32_t res_type = 0;
	std::array<uint8_t, 16> res_meta{};
	Payload payload;
	bool is_framed() const { return stored_size != original_size; }
};

struct ChunkRange {
	uint32_t start = 0;
	uint32_t end = 0;
	uint32_t logical_offset = 0;
	bool operator==(const ChunkRange &) const = default;
};

struct ChunkMeta {
	uint32_t h32 = 0;
	std::optional<uint32_t> first_mip;
	bool operator==(const ChunkMeta &) const = default;
};

struct ChunkEntry {
	Guid id;
	Sha1 sha1;
	uint64_t stored_size = 0;
	std::optional<ChunkRange> range;
	bool tail_only = false;
	std::optional<ChunkMeta> meta;
	Payload payload;
	bool is_framed() const { return (id.bytes[15] & 1) != 0; }
};

struct BundleManifest {
	std::optional<std::string> path;
	std::vector<EbxEntry> ebx;
	std::vector<ResEntry> res;
	std::vector<ChunkEntry> chunks;

	std::vector<ChunkMeta> chunk_meta;
};

Result<BundleManifest> parse_cas_manifest(std::span<const uint8_t> p_element);

Result<uint64_t> binary_manifest_len(std::span<const uint8_t> p_prefix);

Result<BundleManifest> parse_binary_manifest(std::span<const uint8_t> p_head, uint64_t p_bundle_len);

}

#pragma once
// Bundle manifests: the ebx/res/chunk entries of one bundle, from a CAS superbundle (DbObject)
// or a non-CAS superbundle (big-endian binary, payloads inline after the manifest).
// Ported from IW4L superbundle_bf3 (manifest.rs).

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

constexpr uint32_t kBinaryManifestMagic = 0x970D1C13u; // big-endian u32
constexpr uint32_t kBinaryManifestAlphaMagic = 0x0D1CEDB8u;

// Where an entry's stored bytes are.
struct Payload {
	enum class Kind : uint8_t {
		Cas, // by sha1 in the merged cas.cat
		Inline, // CAS manifest `idata`
		Bundle, // binary bundle: at `offset` from the bundle start
	};
	Kind kind = Kind::Cas;
	uint64_t offset = 0;
	std::shared_ptr<const std::vector<uint8_t>> inline_data;
};

struct EbxEntry {
	std::string name;
	Sha1 sha1; // of the stored bytes
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
	uint32_t h32 = 0; // asset_name_hash of the owning asset
	std::optional<uint32_t> first_mip;
	bool operator==(const ChunkMeta &) const = default;
};

struct ChunkEntry {
	Guid id;
	Sha1 sha1; // of the WHOLE stored payload, also when the bundle holds only its tail
	uint64_t stored_size = 0;
	std::optional<ChunkRange> range;
	bool tail_only = false; // binary bundle ranged chunk: holds full_stored[start..end] only
	std::optional<ChunkMeta> meta; // CAS manifests only (index-parallel there)
	Payload payload;
	bool is_framed() const { return (id.bytes[15] & 1) != 0; }
};

struct BundleManifest {
	std::optional<std::string> path;
	std::vector<EbxEntry> ebx;
	std::vector<ResEntry> res;
	std::vector<ChunkEntry> chunks;
	// A binary manifest stores these in an order unrelated to its chunk table; only h32 ties one
	// to a chunk (through the owning asset's name).
	std::vector<ChunkMeta> chunk_meta;
};

// One CAS bundle: a DbObject object starting at byte 0 of p_element.
Result<BundleManifest> parse_cas_manifest(std::span<const uint8_t> p_element);

// Binary bundle: total manifest length (4 + big-endian metaSize) from the first 4 bytes.
Result<uint64_t> binary_manifest_len(std::span<const uint8_t> p_prefix);
// p_head = the first binary_manifest_len() bytes of the bundle, p_bundle_len its whole length.
Result<BundleManifest> parse_binary_manifest(std::span<const uint8_t> p_head, uint64_t p_bundle_len);

} // namespace bf3::core

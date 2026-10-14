#pragma once
// Small container formats: the obfuscation header, layout.toc, superbundle .toc, package.mft,
// cas.cat and the EBX partition header. Ported from IW4L superbundle_bf3 (obfuscation.rs,
// layout.rs, toc.rs, package.rs, catalog.rs, ebx.rs).

#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

// --- Obfuscation header (layout.toc, every .toc, cas.cat, initfs; never .sb/.cas) ------------
//   0x000 magic 00 D1 CE xx | 0x008 RSA signature | 0x128 raw key[0x101] | 0x22C payload
//   key[j] = raw[0x128 + j] ^ 0x7B;  plain[i] = raw[0x22C + i] ^ key[i % 257]
// No 00 D1 CE prefix: plain file. Magic 03: signed only (drop the header, no XOR). An all-zero
// key or the raw mark "2e33ae4783feead2" at key[0x10..0x20] also means no XOR.
constexpr size_t kObfuscationHeaderLen = 0x22C;

enum class Obfuscation {
	Plain,
	Xor,
	SignedXor,
	Signed,
};

Result<Obfuscation> obfuscation_of(std::span<const uint8_t> p_raw);
// Removes the header and the XOR in place. A plain file is left unchanged.
Status deobfuscate(std::vector<uint8_t> &r_bytes);

// --- layout.toc ----------------------------------------------------------------------------------
struct SuperbundleDecl {
	std::string name; // "Win32/Levels/MP_001/MP_001"
	bool delta = false;
};

struct Layout {
	std::vector<SuperbundleDecl> superbundles;
	std::vector<std::string> filesystems;
	std::optional<int64_t> head;
	std::optional<int64_t> base;
};

Result<Layout> parse_layout(std::span<const uint8_t> p_plain);

// --- superbundle .toc -------------------------------------------------------------------------
struct TocBundle {
	std::string id; // "Win32/levels/mp_001/mp_001" (case varies)
	uint64_t offset = 0;
	uint64_t size = 0;
	std::optional<Sha1> checksum;
	bool base = false; // patch toc of a delta superbundle: unchanged, offset/size in the BASE .sb
	bool delta = false; // patch toc of a delta superbundle: a run list in the patch .sb
};

struct TocChunk {
	Guid id;
	bool in_cas = false; // true: sha1 is set; false: offset/size in the .sb next to the toc
	Sha1 sha1;
	uint64_t offset = 0;
	uint64_t size = 0;
};

struct Toc {
	std::optional<std::string> name;
	bool cas = false; // key absent = non-CAS (binary bundles)
	std::vector<TocBundle> bundles;
	std::vector<TocChunk> chunks;
};

Result<Toc> parse_toc(std::span<const uint8_t> p_plain);

// --- package.mft (Update/<pkg>/package.mft) -----------------------------------------------------
// Text lines "Key Value", split at the LAST whitespace; Authoritative/HasContent carry no value.
struct PackageManifest {
	std::optional<std::string> name;
	bool authoritative = false;
	bool has_content = false;
	std::optional<int64_t> version;
	std::optional<int64_t> mount_order;
	std::optional<int64_t> require_version;
};

PackageManifest parse_package_manifest(std::string_view p_text);

// --- cas.cat / cas_NN.cas -----------------------------------------------------------------------
// "NyanNyanNyanNyan" + Entry[32]: sha1[20] | u32 offset | u32 size | u32 archive (all LE).
// offset points at the payload; 32 bytes before it: u32 0xF00FCEFA, sha1[20], u32 size, u32 0.
constexpr uint32_t kCasEntryMagic = 0xF00FCEFAu;
constexpr uint64_t kCasEntryHeaderLen = 32;

struct CatalogEntry {
	Sha1 sha1;
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t archive = 0;
};

Result<std::vector<CatalogEntry>> parse_catalog(std::span<const uint8_t> p_plain);
std::string cas_archive_name(uint32_t p_archive); // "cas_01.cas"
Status check_cas_entry_header(std::span<const uint8_t> p_header, const CatalogEntry &p_entry);

// --- EBX partition header (enough to index partitions; the EBX reader is separate) -------------
constexpr size_t kEbxHeadLen = 0x50;

struct EbxHead {
	uint32_t meta_size = 0;
	uint32_t payload_size = 0;
	Guid partition;
	Guid primary_instance;
	uint64_t file_len() const { return uint64_t(meta_size) + payload_size; }
};

Result<Guid> ebx_partition(std::span<const uint8_t> p_head);
Result<EbxHead> parse_ebx_head(std::span<const uint8_t> p_head);

} // namespace bf3::core

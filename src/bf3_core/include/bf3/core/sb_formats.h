#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

constexpr size_t kObfuscationHeaderLen = 0x22C;

enum class Obfuscation {
	Plain,
	Xor,
	SignedXor,
	Signed,
};

Result<Obfuscation> obfuscation_of(std::span<const uint8_t> p_raw);

Status deobfuscate(std::vector<uint8_t> &r_bytes);

struct SuperbundleDecl {
	std::string name;
	bool delta = false;
};

struct Layout {
	std::vector<SuperbundleDecl> superbundles;
	std::vector<std::string> filesystems;
	std::optional<int64_t> head;
	std::optional<int64_t> base;
};

Result<Layout> parse_layout(std::span<const uint8_t> p_plain);

struct TocBundle {
	std::string id;
	uint64_t offset = 0;
	uint64_t size = 0;
	std::optional<Sha1> checksum;
	bool base = false;
	bool delta = false;
};

struct TocChunk {
	Guid id;
	bool in_cas = false;
	Sha1 sha1;
	uint64_t offset = 0;
	uint64_t size = 0;
};

struct Toc {
	std::optional<std::string> name;
	bool cas = false;
	std::vector<TocBundle> bundles;
	std::vector<TocChunk> chunks;
};

Result<Toc> parse_toc(std::span<const uint8_t> p_plain);

struct PackageManifest {
	std::optional<std::string> name;
	bool authoritative = false;
	bool has_content = false;
	std::optional<int64_t> version;
	std::optional<int64_t> mount_order;
	std::optional<int64_t> require_version;
};

PackageManifest parse_package_manifest(std::string_view p_text);

constexpr uint32_t kCasEntryMagic = 0xF00FCEFAu;
constexpr uint64_t kCasEntryHeaderLen = 32;

struct CatalogEntry {
	Sha1 sha1;
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t archive = 0;
};

Result<std::vector<CatalogEntry>> parse_catalog(std::span<const uint8_t> p_plain);
std::string cas_archive_name(uint32_t p_archive);
Status check_cas_entry_header(std::span<const uint8_t> p_header, const CatalogEntry &p_entry);

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

}

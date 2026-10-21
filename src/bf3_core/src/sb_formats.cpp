#include "bf3/core/sb_formats.h"

#include "bf3/core/db_object.h"
#include "bf3/core/util/text.h"
#include "db_record.h"
#include "format_errors.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bf3::core {

using detail::Record;

namespace {

constexpr size_t kKeyAt = 0x128;
constexpr size_t kKeyLen = 0x101;
constexpr uint8_t kKeySeed = 0x7B;
constexpr char kPlainKeyMark[] = "2e33ae4783feead2";

uint32_t le32(const uint8_t *p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

}

Result<Obfuscation> obfuscation_of(std::span<const uint8_t> p_raw) {
	if (p_raw.size() < 4 || p_raw[0] != 0x00 || p_raw[1] != 0xD1 || p_raw[2] != 0xCE) {
		return Obfuscation::Plain;
	}
	switch (p_raw[3]) {
		case 0x00:
			return Obfuscation::Xor;
		case 0x01:
			return Obfuscation::SignedXor;
		case 0x03:
			return Obfuscation::Signed;
		default:
			return detail::bad_magic("obfuscation header", le32(p_raw.data()));
	}
}

Status deobfuscate(std::vector<uint8_t> &r_bytes) {
	Result<Obfuscation> kind = obfuscation_of(r_bytes);
	if (!kind) {
		return kind.error();
	}
	if (*kind == Obfuscation::Plain) {
		return {};
	}
	BF3_TRY(detail::need("obfuscation header", r_bytes.size(), kObfuscationHeaderLen));
	uint8_t key[kKeyLen];
	bool any_key = false;
	for (size_t j = 0; j < kKeyLen; ++j) {
		key[j] = r_bytes[kKeyAt + j] ^ kKeySeed;
		any_key |= key[j] != 0;
	}
	const bool xored = *kind != Obfuscation::Signed && std::memcmp(&r_bytes[kKeyAt + 0x10], kPlainKeyMark, 16) != 0 && any_key;
	uint8_t *data = r_bytes.data();
	const size_t len = r_bytes.size() - kObfuscationHeaderLen;
	std::memmove(data, data + kObfuscationHeaderLen, len);
	r_bytes.resize(len);
	if (xored) {
		for (size_t i = 0; i < len; i += kKeyLen) {
			const size_t n = std::min(kKeyLen, len - i);
			for (size_t k = 0; k < n; ++k) {
				data[i + k] ^= key[k];
			}
		}
	}
	return {};
}

Result<Layout> parse_layout(std::span<const uint8_t> p_plain) {
	Result<DbDocument> doc = DbDocument::parse(p_plain);
	if (!doc) {
		return doc.error();
	}
	Result<Record> layout = Record::make("layout", doc->root());
	if (!layout) {
		return layout.error();
	}
	Layout out;
	Result<DbRef> sbs = layout->list("superBundles");
	if (!sbs) {
		return sbs.error();
	}
	for (DbRef e = sbs->first_child(); e.valid(); e = e.next()) {
		Result<Record> entry = Record::make("layout superbundle", e);
		if (!entry) {
			return entry.error();
		}
		Result<std::string_view> name = entry->str("name");
		if (!name) {
			return name.error();
		}
		Result<bool> delta = entry->flag("delta");
		if (!delta) {
			return delta.error();
		}
		out.superbundles.push_back({ std::string(*name), *delta });
	}
	Result<DbRef> fs = layout->list("fs");
	if (!fs) {
		return fs.error();
	}
	for (DbRef e = fs->first_child(); e.valid(); e = e.next()) {
		const std::optional<std::string_view> s = e.as_str();
		if (!s) {
			return detail::field_type("layout", "fs");
		}
		out.filesystems.emplace_back(*s);
	}
	Result<std::optional<int64_t>> head = layout->opt_i64("head");
	if (!head) {
		return head.error();
	}
	Result<std::optional<int64_t>> base = layout->opt_i64("base");
	if (!base) {
		return base.error();
	}
	out.head = *head;
	out.base = *base;
	return out;
}

Result<Toc> parse_toc(std::span<const uint8_t> p_plain) {
	Result<DbDocument> doc = DbDocument::parse(p_plain);
	if (!doc) {
		return doc.error();
	}
	Result<Record> toc = Record::make("toc", doc->root());
	if (!toc) {
		return toc.error();
	}
	Toc out;
	Result<DbRef> bundles = toc->list("bundles");
	if (!bundles) {
		return bundles.error();
	}
	out.bundles.reserve(bundles->child_count());
	for (DbRef e = bundles->first_child(); e.valid(); e = e.next()) {
		Result<Record> entry = Record::make("toc bundle", e);
		if (!entry) {
			return entry.error();
		}
		TocBundle b;
		Result<std::string_view> id = entry->str("id");
		if (!id) {
			return id.error();
		}
		b.id = std::string(*id);
		Result<uint64_t> offset = entry->u64("offset");
		if (!offset) {
			return offset.error();
		}
		Result<uint64_t> size = entry->u64("size");
		if (!size) {
			return size.error();
		}
		Result<std::optional<Sha1>> checksum = entry->opt_sha1("checksum");
		if (!checksum) {
			return checksum.error();
		}
		Result<bool> base = entry->flag("base");
		if (!base) {
			return base.error();
		}
		Result<bool> delta = entry->flag("delta");
		if (!delta) {
			return delta.error();
		}
		b.offset = *offset;
		b.size = *size;
		b.checksum = *checksum;
		b.base = *base;
		b.delta = *delta;
		out.bundles.push_back(std::move(b));
	}
	Result<DbRef> chunks = toc->list("chunks");
	if (!chunks) {
		return chunks.error();
	}
	out.chunks.reserve(chunks->child_count());
	for (DbRef e = chunks->first_child(); e.valid(); e = e.next()) {
		Result<Record> entry = Record::make("toc chunk", e);
		if (!entry) {
			return entry.error();
		}
		TocChunk c;
		Result<std::optional<Sha1>> sha1 = entry->opt_sha1("sha1");
		if (!sha1) {
			return sha1.error();
		}
		if (sha1->has_value()) {
			c.in_cas = true;
			c.sha1 = **sha1;
		} else {
			Result<uint64_t> offset = entry->u64("offset");
			if (!offset) {
				return offset.error();
			}
			Result<uint64_t> size = entry->u64("size");
			if (!size) {
				return size.error();
			}
			c.offset = *offset;
			c.size = *size;
		}
		Result<Guid> id = entry->guid("id");
		if (!id) {
			return id.error();
		}
		c.id = *id;
		out.chunks.push_back(c);
	}
	Result<std::optional<std::string_view>> name = toc->opt_str("name");
	if (!name) {
		return name.error();
	}
	if (name->has_value()) {
		out.name = std::string(**name);
	}
	Result<bool> cas = toc->flag("cas");
	if (!cas) {
		return cas.error();
	}
	out.cas = *cas;
	return out;
}

namespace {

bool is_space(char p_c) {
	return p_c == ' ' || p_c == '\t' || p_c == '\r' || p_c == '\n' || p_c == '\v' || p_c == '\f';
}

std::optional<int64_t> parse_i64(std::optional<std::string_view> p_value) {
	if (!p_value || p_value->empty()) {
		return std::nullopt;
	}
	std::string_view s = *p_value;
	bool negative = false;
	if (s.front() == '+' || s.front() == '-') {
		negative = s.front() == '-';
		s.remove_prefix(1);
	}
	if (s.empty() || s.size() > 18) {
		return std::nullopt;
	}
	int64_t v = 0;
	for (char c : s) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		v = v * 10 + (c - '0');
	}
	return negative ? -v : v;
}

}

PackageManifest parse_package_manifest(std::string_view p_text) {
	PackageManifest m;
	size_t pos = 0;
	while (pos < p_text.size()) {
		size_t eol = p_text.find('\n', pos);
		if (eol == std::string_view::npos) {
			eol = p_text.size();
		}
		const std::string_view line = trim(p_text.substr(pos, eol - pos));
		pos = eol + 1;
		if (line.empty()) {
			continue;
		}
		std::string_view key = line;
		std::optional<std::string_view> value;
		for (size_t i = line.size(); i-- > 0;) {
			if (is_space(line[i])) {
				key = trim(line.substr(0, i));
				value = trim(line.substr(i + 1));
				break;
			}
		}
		const std::string k = to_lower_ascii(key);
		if (k == "authoritative" && !value) {
			m.authoritative = true;
		} else if (k == "hascontent" && !value) {
			m.has_content = true;
		} else if (k == "name") {
			if (value) {
				m.name = std::string(*value);
			} else {
				m.name.reset();
			}
		} else if (k == "version") {
			m.version = parse_i64(value);
		} else if (k == "mountorder") {
			m.mount_order = parse_i64(value);
		} else if (k == "requireversion") {
			m.require_version = parse_i64(value);
		}
	}
	return m;
}

Result<std::vector<CatalogEntry>> parse_catalog(std::span<const uint8_t> p_plain) {
	static constexpr char kMagic[] = "NyanNyanNyanNyan";
	BF3_TRY(detail::need("CAS catalog", p_plain.size(), 16));
	if (std::memcmp(p_plain.data(), kMagic, 16) != 0) {
		return detail::bad_magic("CAS catalog", le32(p_plain.data()));
	}
	const std::span<const uint8_t> body = p_plain.subspan(16);
	if (body.size() % 32 != 0) {
		return detail::layout_error("CAS catalog", "entries are not 32 bytes each");
	}
	std::vector<CatalogEntry> out(body.size() / 32);
	for (size_t i = 0; i < out.size(); ++i) {
		const uint8_t *e = body.data() + i * 32;
		out[i].sha1 = Sha1::from_bytes(e);
		out[i].offset = le32(e + 20);
		out[i].size = le32(e + 24);
		out[i].archive = le32(e + 28);
	}
	return out;
}

std::string cas_archive_name(uint32_t p_archive) {
	char name[32];
	std::snprintf(name, sizeof(name), "cas_%02u.cas", p_archive);
	return name;
}

Status check_cas_entry_header(std::span<const uint8_t> p_header, const CatalogEntry &p_entry) {
	BF3_TRY(detail::need("CAS entry header", p_header.size(), kCasEntryHeaderLen));
	const uint32_t magic = le32(p_header.data());
	if (magic != kCasEntryMagic) {
		return detail::bad_magic("CAS entry header", magic);
	}
	const uint32_t size = le32(p_header.data() + 24);
	if (std::memcmp(p_header.data() + 4, p_entry.sha1.bytes.data(), 20) != 0 || size != p_entry.size) {
		return detail::layout_error("CAS entry header", "does not match its catalog entry");
	}
	return {};
}

namespace {

constexpr size_t kPartitionAt = 0x30;
constexpr uint8_t kEbxMagic[4] = { 0xCE, 0xD1, 0xB2, 0x0F };

Status check_ebx_magic(std::span<const uint8_t> p_head) {
	if (std::memcmp(p_head.data(), kEbxMagic, 4) != 0) {
		return detail::bad_magic("EBX partition", le32(p_head.data()));
	}
	return {};
}

}

Result<Guid> ebx_partition(std::span<const uint8_t> p_head) {
	BF3_TRY(detail::need("EBX header", p_head.size(), kPartitionAt + 16));
	BF3_TRY(check_ebx_magic(p_head));
	return Guid::from_bytes(p_head.data() + kPartitionAt);
}

Result<EbxHead> parse_ebx_head(std::span<const uint8_t> p_head) {
	BF3_TRY(detail::need("EBX header", p_head.size(), kEbxHeadLen));
	BF3_TRY(check_ebx_magic(p_head));
	EbxHead h;
	h.meta_size = le32(p_head.data() + 4);
	h.payload_size = le32(p_head.data() + 8);
	h.partition = Guid::from_bytes(p_head.data() + kPartitionAt);
	h.primary_instance = Guid::from_bytes(p_head.data() + kPartitionAt + 16);
	return h;
}

}

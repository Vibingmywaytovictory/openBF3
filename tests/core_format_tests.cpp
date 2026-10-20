#include <doctest/doctest.h>

#include "bf3/core/bundle_manifest.h"
#include "bf3/core/db_object.h"
#include "bf3/core/delta_bundle.h"
#include "bf3/core/ids.h"
#include "bf3/core/payload.h"
#include "bf3/core/res_types.h"
#include "bf3/core/sb_formats.h"
#include "bf3/core/sha1_digest.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace bf3::core;

namespace {

using Bytes = std::vector<uint8_t>;

std::span<const uint8_t> span_of(const char *p_text) {
	return { reinterpret_cast<const uint8_t *>(p_text), std::strlen(p_text) };
}

Bytes from_hex(std::string_view p_hex) {
	Bytes out;
	for (size_t i = 0; i + 1 < p_hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoi(std::string(p_hex.substr(i, 2)), nullptr, 16)));
	}
	return out;
}

void uleb(Bytes &r_out, uint64_t p_value) {
	do {
		uint8_t b = p_value & 0x7F;
		p_value >>= 7;
		r_out.push_back(b | (p_value ? 0x80 : 0));
	} while (p_value);
}

void tag(Bytes &r_out, uint8_t p_type, const char *p_name) {
	if (p_name == nullptr) {
		r_out.push_back(p_type | 0x80);
		return;
	}
	r_out.push_back(p_type);
	r_out.insert(r_out.end(), p_name, p_name + std::strlen(p_name) + 1);
}

void container(Bytes &r_out, uint8_t p_type, const char *p_name, const Bytes &p_body) {
	tag(r_out, p_type, p_name);
	uleb(r_out, p_body.size() + 1);
	r_out.insert(r_out.end(), p_body.begin(), p_body.end());
	r_out.push_back(0);
}

void str(Bytes &r_out, const char *p_name, const char *p_value) {
	tag(r_out, 0x07, p_name);
	uleb(r_out, std::strlen(p_value) + 1);
	r_out.insert(r_out.end(), p_value, p_value + std::strlen(p_value) + 1);
}

void i32(Bytes &r_out, const char *p_name, int32_t p_value) {
	tag(r_out, 0x08, p_name);
	const uint8_t *b = reinterpret_cast<const uint8_t *>(&p_value);
	r_out.insert(r_out.end(), b, b + 4);
}

void boolean(Bytes &r_out, const char *p_name, bool p_value) {
	tag(r_out, 0x06, p_name);
	r_out.push_back(p_value ? 1 : 0);
}

void raw(Bytes &r_out, uint8_t p_type, const char *p_name, const Bytes &p_bytes) {
	tag(r_out, p_type, p_name);
	r_out.insert(r_out.end(), p_bytes.begin(), p_bytes.end());
}

void blob(Bytes &r_out, const char *p_name, const Bytes &p_bytes) {
	tag(r_out, 0x13, p_name);
	uleb(r_out, p_bytes.size());
	r_out.insert(r_out.end(), p_bytes.begin(), p_bytes.end());
}

void be32(Bytes &r_out, uint32_t p_v) {
	r_out.push_back(uint8_t(p_v >> 24));
	r_out.push_back(uint8_t(p_v >> 16));
	r_out.push_back(uint8_t(p_v >> 8));
	r_out.push_back(uint8_t(p_v));
}

void be64(Bytes &r_out, uint64_t p_v) {
	be32(r_out, uint32_t(p_v >> 32));
	be32(r_out, uint32_t(p_v));
}

class VectorSource final : public ByteSource {
public:
	explicit VectorSource(Bytes p_bytes) :
			bytes_(std::move(p_bytes)) {}
	Status read_exact_at(uint64_t p_offset, std::span<uint8_t> p_dst) const override {
		if (p_offset + p_dst.size() > bytes_.size()) {
			return make_error("short read");
		}
		std::memcpy(p_dst.data(), bytes_.data() + p_offset, p_dst.size());
		return {};
	}

private:
	Bytes bytes_;
};

}

TEST_SUITE("containers") {
	TEST_CASE("ids: guid text, big-endian fields, name hash") {
		const std::optional<Guid> g = Guid::parse("dcef8164-6ff8-420a-0d6e-fd006ded4591");
		REQUIRE(g.has_value());
		CHECK(g->bytes == Guid::from_bytes(from_hex("6481efdcf86f0a420d6efd006ded4591").data()).bytes);
		CHECK(g->to_string() == "dcef8164-6ff8-420a-0d6e-fd006ded4591");
		CHECK(Guid::parse("{DCEF8164-6FF8-420A-0D6E-FD006DED4591}") == g);
		CHECK(Guid::from_big_endian_fields(from_hex("dcef81646ff8420a0d6efd006ded4591").data()) == *g);
		CHECK(!Guid::parse("dcef8164-6ff8-420a-0d6e-fd006ded459").has_value());
		CHECK(!Guid::parse("dcef8164x6ff8-420a-0d6e-fd006ded4591").has_value());
		CHECK(chunk_is_framed(*g));
		CHECK(asset_name_hash("") == 5381u);
		CHECK(asset_name_hash("a") == ((5381u * 33u) ^ uint32_t('a')));
		CHECK(asset_name_hash("Levels/MP_001/MP_001") == asset_name_hash("levels/mp_001/mp_001"));
		CHECK(bundle_key("Win32/Levels/MP_001/Conquest_Large") == "levels/mp_001/conquest_large");
		CHECK(bundle_key("/win32\\UI\\Flow") == "ui/flow");
		CHECK(asset_key("Weapons\\M16A4\\M16A4") == "weapons/m16a4/m16a4");
		CHECK(res_type_name(0x5C4954A6u) == "DxTexture");
		CHECK(res_type_name(0x12345678u) == "0x12345678");
	}

	TEST_CASE("sha1: FIPS 180 vectors") {
		CHECK(sha1_of({}).to_string() == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
		CHECK(sha1_of(span_of("abc")).to_string() == "a9993e364706816aba3e25717850c26c9cd0d89d");
		CHECK(sha1_of(span_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).to_string() ==
				"84983e441c3bd26ebaae4aa1f95129e5e54670f1");
		const Bytes million(1000000, 'a');
		CHECK(sha1_of(million).to_string() == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
		CHECK(Sha1::parse("A9993E364706816ABA3E25717850C26C9CD0D89D") == sha1_of(span_of("abc")));
	}

	TEST_CASE("dbobject: object, list, scalars, strict framing") {
		const Bytes guid = from_hex("6481efdcf86f0a420d6efd006ded4591");
		Bytes items;
		boolean(items, nullptr, true);
		str(items, nullptr, "fs0");
		Bytes body;
		str(body, "name", "Win32/Globals");
		i32(body, "Count", -7);
		raw(body, 0x0F, "id", guid);
		blob(body, "meta", Bytes(16, 0xAB));
		container(body, 0x01, "list", items);
		Bytes doc;
		container(doc, 0x02, nullptr, body);

		Result<DbDocument> parsed = DbDocument::parse(doc);
		REQUIRE(parsed.ok());
		const DbRef root = parsed->root();
		CHECK(root.is_object());
		CHECK(root.child_count() == 5);
		CHECK(root.get("NAME").as_str() == std::optional<std::string_view>("Win32/Globals"));
		CHECK(root.get("count").as_i64() == std::optional<int64_t>(-7));
		CHECK(root.get("id").as_guid() == Guid::parse("dcef8164-6ff8-420a-0d6e-fd006ded4591"));
		CHECK(root.get("meta").as_blob()->size() == 16);
		CHECK(!root.get("missing").valid());
		const DbRef list = root.get("list");
		REQUIRE(list.is_list());
		CHECK(list.first_child().as_bool() == std::optional<bool>(true));
		CHECK(list.first_child().next().as_str() == std::optional<std::string_view>("fs0"));
		CHECK(!list.first_child().next().next().valid());

		Bytes trailing = doc;
		trailing.push_back(0);
		CHECK(!DbDocument::parse(trailing).ok());
		Bytes cut(doc.begin(), doc.end() - 1);
		CHECK(!DbDocument::parse(cut).ok());
		Bytes unknown;
		tag(unknown, 0x05, nullptr);
		Result<DbDocument> bad = DbDocument::parse(unknown);
		REQUIRE(!bad.ok());
		CHECK(bad.error().message == "unsupported DbObject tag 0x85 at byte 0");
		const Bytes end_marker{ 0x00 };
		CHECK(!DbDocument::parse(end_marker).ok());
	}

	TEST_CASE("obfuscation: xor, signed, zero key, plain") {
		const Bytes plain = [] {
			Bytes p(1000);
			for (size_t i = 0; i < p.size(); ++i) {
				p[i] = static_cast<uint8_t>(i * 7 + 3);
			}
			return p;
		}();
		auto make = [&](uint8_t p_magic, bool p_zero_key) {
			Bytes raw(kObfuscationHeaderLen, 0);
			raw[1] = 0xD1;
			raw[2] = 0xCE;
			raw[3] = p_magic;
			uint8_t key[0x101];
			for (size_t j = 0; j < 0x101; ++j) {
				key[j] = p_zero_key ? 0 : static_cast<uint8_t>(j * 13 + 1);
				raw[0x128 + j] = key[j] ^ 0x7B;
			}
			for (size_t i = 0; i < plain.size(); ++i) {
				raw.push_back(plain[i] ^ ((p_magic == 0x03) ? 0 : key[i % 0x101]));
			}
			return raw;
		};
		for (uint8_t magic : { uint8_t(0x00), uint8_t(0x01), uint8_t(0x03) }) {
			Bytes b = make(magic, false);
			REQUIRE(deobfuscate(b).ok());
			CHECK(b == plain);
		}
		Bytes zero = make(0x01, true);
		REQUIRE(deobfuscate(zero).ok());
		CHECK(zero == plain);
		Bytes untouched = plain;
		REQUIRE(deobfuscate(untouched).ok());
		CHECK(untouched == plain);
		Bytes bad = make(0x02, false);
		CHECK(!deobfuscate(bad).ok());
		Bytes short_header{ 0x00, 0xD1, 0xCE, 0x01, 0x00 };
		CHECK(!deobfuscate(short_header).ok());
	}

	TEST_CASE("package.mft: split at the last whitespace") {
		const PackageManifest m = parse_package_manifest("Name patch\r\nAuthoritative\r\nVersion 7\r\n");
		CHECK(m.name == std::optional<std::string>("patch"));
		CHECK(m.authoritative);
		CHECK(m.version == std::optional<int64_t>(7));
		const PackageManifest x = parse_package_manifest("Name   xpack2\nMountOrder 0\nRequireVersion 4\nHasContent\n");
		CHECK(x.name == std::optional<std::string>("xpack2"));
		CHECK(x.mount_order == std::optional<int64_t>(0));
		CHECK(x.require_version == std::optional<int64_t>(4));
		CHECK(x.has_content);
		CHECK(!x.authoritative);
	}

	TEST_CASE("payload: zlib, verbatim and fallback blocks") {
		Bytes stored;
		const Bytes hello = from_hex("789ccb48cdc9c90700062c0215");
		be32(stored, 5);
		be32(stored, static_cast<uint32_t>(hello.size()));
		stored.insert(stored.end(), hello.begin(), hello.end());
		Bytes verbatim(kMaxBlockLen, 0x5A);
		be32(stored, kMaxBlockLen);
		be32(stored, kMaxBlockLen);
		stored.insert(stored.end(), verbatim.begin(), verbatim.end());
		be32(stored, 4);
		be32(stored, 4);
		stored.insert(stored.end(), { 'a', 'b', 'c', 'd' });
		Result<std::vector<Block>> blocks = payload_blocks(stored);
		REQUIRE(blocks.ok());
		CHECK(blocks->size() == 3);
		CHECK((*blocks)[1].is_verbatim());
		CHECK((*blocks)[2].decoded_at == 5 + kMaxBlockLen);
		Result<std::vector<uint8_t>> decoded = decode_blocks(stored);
		REQUIRE(decoded.ok());
		REQUIRE(decoded->size() == 5 + kMaxBlockLen + 4);
		CHECK(std::memcmp(decoded->data(), "hello", 5) == 0);
		CHECK((*decoded)[5] == 0x5A);
		CHECK(std::memcmp(decoded->data() + 5 + kMaxBlockLen, "abcd", 4) == 0);
		CHECK(decoded_len(stored).value() == decoded->size());

		Bytes wrong_len;
		be32(wrong_len, 6);
		be32(wrong_len, static_cast<uint32_t>(hello.size()));
		wrong_len.insert(wrong_len.end(), hello.begin(), hello.end());
		CHECK(!decode_blocks(wrong_len).ok());
		Bytes too_large;
		be32(too_large, kMaxBlockLen + 1);
		be32(too_large, 0);
		CHECK(!payload_blocks(too_large).ok());
		Bytes cut(stored.begin(), stored.begin() + 10);
		CHECK(!payload_blocks(cut).ok());

		Bytes inflated;
		for (int i = 0; i < 16385; ++i) {
			be32(inflated, kMaxBlockLen);
			be32(inflated, 0);
		}
		CHECK(decoded_len(inflated).value() == uint64_t(16385) * kMaxBlockLen);
		CHECK(!decode_blocks(inflated).ok());

		const Guid framed = *Guid::parse("00000000-0000-0000-0000-000000000001");
		const uint32_t second = static_cast<uint32_t>(8 + hello.size());
		Result<std::vector<uint8_t>> tail = decode_chunk_range(framed, stored, ChunkRange{ second, static_cast<uint32_t>(stored.size()), 5 });
		REQUIRE(tail.ok());
		CHECK(tail->size() == kMaxBlockLen + 4);
		CHECK(!decode_chunk_range(framed, stored, ChunkRange{ second, static_cast<uint32_t>(stored.size()), 4 }).ok());
		CHECK(!decode_chunk_range(framed, stored, ChunkRange{ second + 1, static_cast<uint32_t>(stored.size()), 5 }).ok());
		const Guid unframed = *Guid::parse("00000000-0000-0000-0000-000000000002");
		CHECK(decode_chunk(unframed, stored)->size() == stored.size());
	}

	TEST_CASE("delta bundle: patch and base runs, absolute offsets") {

		Bytes table;
		be32(table, 3 * 16);
		be32(table, kDeltaMagic);
		be64(table, 0);
		be32(table, 3);
		be32(table, 0);
		be64(table, 0);
		be32(table, 4);
		be32(table, 1);
		be64(table, 2);
		be32(table, 2);
		be32(table, 0);
		be64(table, 0);
		Bytes patch_sb(5, 0xEE);
		patch_sb.insert(patch_sb.end(), table.begin(), table.end());
		patch_sb.insert(patch_sb.end(), { 'P', 'Q', 'R', 'S', 'T' });
		const Bytes base_sb{ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
		const uint64_t entry_size = patch_sb.size() - 5;

		CHECK(delta_table_len(table).value() == 16 + 48);
		Result<DeltaBundle> delta = DeltaBundle::parse(table, 5, entry_size);
		REQUIRE(delta.ok());
		CHECK(delta->len() == 9);
		CHECK(delta->runs().size() == 3);
		const VectorSource patch(patch_sb), base(base_sb);
		Bytes all(9);
		REQUIRE(delta->read_at(patch, base, 0, all).ok());
		CHECK(std::string(all.begin(), all.end()) == "PQRcdefST");
		Bytes mid(4);
		REQUIRE(delta->read_at(patch, base, 2, mid).ok());
		CHECK(std::string(mid.begin(), mid.end()) == "Rcde");
		Bytes past(2);
		CHECK(!delta->read_at(patch, base, 8, past).ok());
		CHECK(!DeltaBundle::parse(table, 5, entry_size - 1).ok());
		Bytes bad_magic = table;
		bad_magic[4] = 0;
		CHECK(!delta_table_len(bad_magic).ok());
	}

	TEST_CASE("binary manifest: tables, names, 16-aligned payloads, swapped chunk ids") {

		const uint32_t ebx = 1, res = 1, chunks = 1, total = 3;
		Bytes tables;
		for (uint8_t i = 0; i < total; ++i) {
			tables.insert(tables.end(), 20, static_cast<uint8_t>(0x10 + i));
		}
		be32(tables, 0);
		be32(tables, 100);
		be32(tables, 100);
		be32(tables, 4);
		be32(tables, 30);
		be32(tables, 64);
		be32(tables, 0x5C4954A6u);
		tables.insert(tables.end(), 16, 0x77);
		const Bytes id_be = from_hex("dcef81646ff8420a0d6efd006ded4591");
		tables.insert(tables.end(), id_be.begin(), id_be.end());
		be32(tables, 16);
		be32(tables, 48);
		be32(tables, 4096);
		const char names[] = "e/a\0r/b\0";
		const uint32_t strings_rel = 0x24 - 4 + static_cast<uint32_t>(tables.size());
		Bytes head;
		be32(head, 0);
		be32(head, kBinaryManifestMagic);
		be32(head, total);
		be32(head, ebx);
		be32(head, res);
		be32(head, chunks);
		be32(head, strings_rel);
		be32(head, 0);
		be32(head, 0);
		head.insert(head.end(), tables.begin(), tables.end());
		head.insert(head.end(), names, names + sizeof(names) - 1);
		const uint32_t meta = static_cast<uint32_t>(head.size() - 4);
		head[0] = uint8_t(meta >> 24);
		head[1] = uint8_t(meta >> 16);
		head[2] = uint8_t(meta >> 8);
		head[3] = uint8_t(meta);
		CHECK(binary_manifest_len(head).value() == head.size());

		const auto align = [](uint64_t p_at) { return (p_at + 15) / 16 * 16; };
		const uint64_t ebx_at = align(head.size());
		const uint64_t res_at = align(ebx_at + 100);
		const uint64_t chunk_at = align(res_at + 30);
		const uint64_t bundle_len = chunk_at + 32;
		Result<BundleManifest> m = parse_binary_manifest(head, bundle_len);
		REQUIRE(m.ok());
		REQUIRE(m->ebx.size() == 1);
		REQUIRE(m->res.size() == 1);
		REQUIRE(m->chunks.size() == 1);
		CHECK(m->ebx[0].name == "e/a");
		CHECK(m->ebx[0].payload.kind == Payload::Kind::Bundle);
		CHECK(m->ebx[0].payload.offset == ebx_at);
		CHECK(m->res[0].name == "r/b");
		CHECK(m->res[0].res_type == 0x5C4954A6u);
		CHECK(m->res[0].res_meta[15] == 0x77);
		CHECK(m->res[0].payload.offset == res_at);
		CHECK(m->res[0].is_framed());
		CHECK(m->chunks[0].id == *Guid::parse("dcef8164-6ff8-420a-0d6e-fd006ded4591"));
		CHECK(m->chunks[0].stored_size == 32);
		CHECK(m->chunks[0].tail_only);
		CHECK(m->chunks[0].range->logical_offset == 4096);
		CHECK(m->chunks[0].payload.offset == chunk_at);
		CHECK(m->chunks[0].sha1.bytes[0] == 0x12);
		CHECK(!parse_binary_manifest(head, bundle_len - 1).ok());
	}

	TEST_CASE("cas manifest: entries, idata, ranges, chunkMeta") {
		const Bytes sha(20, 0x42);
		Bytes ebx_entry;
		str(ebx_entry, "name", "levels/x/x");
		raw(ebx_entry, 0x10, "sha1", sha);
		i32(ebx_entry, "size", 96);
		i32(ebx_entry, "originalSize", 96);
		Bytes ebx_list;
		container(ebx_list, 0x02, nullptr, ebx_entry);
		Bytes res_entry;
		str(res_entry, "name", "levels/x/tex");
		raw(res_entry, 0x10, "sha1", sha);
		i32(res_entry, "size", 3);
		i32(res_entry, "originalSize", 128);
		i32(res_entry, "resType", static_cast<int32_t>(0xC417BBD3u));
		blob(res_entry, "resMeta", Bytes(16, 1));
		blob(res_entry, "idata", Bytes{ 1, 2, 3 });
		Bytes res_list;
		container(res_list, 0x02, nullptr, res_entry);
		Bytes chunk_entry;
		raw(chunk_entry, 0x0F, "id", from_hex("6481efdcf86f0a420d6efd006ded4591"));
		raw(chunk_entry, 0x10, "sha1", sha);
		i32(chunk_entry, "size", 5000);
		i32(chunk_entry, "rangeStart", 1000);
		i32(chunk_entry, "logicalOffset", 65536);
		Bytes chunk_list;
		container(chunk_list, 0x02, nullptr, chunk_entry);
		Bytes meta_inner;
		i32(meta_inner, "firstMip", 2);
		Bytes meta_entry;
		i32(meta_entry, "h32", static_cast<int32_t>(asset_name_hash("levels/x/tex")));
		container(meta_entry, 0x02, "meta", meta_inner);
		Bytes meta_list;
		container(meta_list, 0x02, nullptr, meta_entry);
		Bytes body;
		str(body, "path", "win32/levels/x/x");
		container(body, 0x01, "ebx", ebx_list);
		container(body, 0x01, "res", res_list);
		container(body, 0x01, "chunks", chunk_list);
		container(body, 0x01, "chunkMeta", meta_list);
		Bytes bundle;
		container(bundle, 0x02, nullptr, body);

		Result<BundleManifest> m = parse_cas_manifest(bundle);
		REQUIRE(m.ok());
		CHECK(m->path == std::optional<std::string>("win32/levels/x/x"));
		REQUIRE(m->ebx.size() == 1);
		CHECK(m->ebx[0].payload.kind == Payload::Kind::Cas);
		CHECK(!m->ebx[0].is_framed());
		REQUIRE(m->res.size() == 1);
		CHECK(m->res[0].res_type == 0xC417BBD3u);
		CHECK(m->res[0].payload.kind == Payload::Kind::Inline);
		CHECK(m->res[0].payload.inline_data->size() == 3);
		CHECK(m->res[0].original_size == 128);
		REQUIRE(m->chunks.size() == 1);
		CHECK(m->chunks[0].range->start == 1000);
		CHECK(m->chunks[0].range->end == 5000);
		CHECK(m->chunks[0].range->logical_offset == 65536);
		CHECK(!m->chunks[0].tail_only);
		REQUIRE(m->chunks[0].meta.has_value());
		CHECK(m->chunks[0].meta->first_mip == std::optional<uint32_t>(2));
		CHECK(m->chunk_meta.size() == 1);

		Bytes bad_idata;
		Bytes bad_entry;
		str(bad_entry, "name", "r");
		raw(bad_entry, 0x10, "sha1", sha);
		i32(bad_entry, "size", 4);
		blob(bad_entry, "idata", Bytes{ 1, 2, 3 });
		Bytes bad_list;
		container(bad_list, 0x02, nullptr, bad_entry);
		Bytes bad_body;
		container(bad_body, 0x01, "res", bad_list);
		container(bad_idata, 0x02, nullptr, bad_body);
		CHECK(!parse_cas_manifest(bad_idata).ok());
	}

	TEST_CASE("thread pool: every index once, results in order") {
		std::atomic<int> sum{ 0 };
		parallel_for(1000, [&](size_t i) { sum += static_cast<int>(i); });
		CHECK(sum == 999 * 1000 / 2);
		const std::vector<size_t> squares = parallel_map<size_t>(257, [](size_t i) { return i * i; });
		REQUIRE(squares.size() == 257);
		CHECK(squares[256] == 256u * 256u);
		CHECK(parallel_map<int>(0, [](size_t) { return 1; }).empty());
	}
}

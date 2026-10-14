// Ported from IW4L superbundle_bf3 (manifest.rs), Apache-2.0.
#include "bf3/core/bundle_manifest.h"

#include "bf3/core/db_object.h"
#include "db_record.h"
#include "format_errors.h"

#include <cstring>

namespace bf3::core {

using detail::Record;

namespace {

constexpr size_t kBinaryHeaderLen = 0x24;
constexpr uint64_t kBinaryPayloadAlign = 16;

uint32_t be32(const uint8_t *p_bytes, size_t p_at) {
	const uint8_t *p = p_bytes + p_at;
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// `size`/`idata` of a CAS manifest entry -> (stored size, payload).
Status cas_payload(const Record &p_entry, uint64_t &r_stored, Payload &r_payload) {
	Result<std::optional<uint64_t>> size = p_entry.opt_u64("size");
	if (!size) {
		return size.error();
	}
	Result<std::optional<std::span<const uint8_t>>> idata = p_entry.opt_blob("idata");
	if (!idata) {
		return idata.error();
	}
	if (idata->has_value()) {
		const std::span<const uint8_t> bytes = **idata;
		if (size->has_value() && **size != bytes.size()) {
			return detail::layout_error("CAS bundle entry", "inline data length differs from its size");
		}
		r_stored = bytes.size();
		r_payload.kind = Payload::Kind::Inline;
		r_payload.inline_data = std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end());
		return {};
	}
	Result<uint64_t> required = p_entry.u64("size");
	if (!required) {
		return required.error();
	}
	r_stored = *required;
	r_payload.kind = Payload::Kind::Cas;
	return {};
}

Status res_meta(const std::optional<std::span<const uint8_t>> &p_blob, std::array<uint8_t, 16> &r_meta) {
	r_meta = {};
	if (!p_blob) {
		return {};
	}
	if (p_blob->size() != 16) {
		return detail::layout_error("res entry", "resMeta is not 16 bytes");
	}
	std::memcpy(r_meta.data(), p_blob->data(), 16);
	return {};
}

Result<std::vector<ChunkMeta>> chunk_metas(DbRef p_list, size_t p_chunk_count) {
	const size_t n = p_list.valid() ? p_list.child_count() : 0;
	if (n != 0 && n != p_chunk_count) {
		return detail::layout_error("chunkMeta", "has a different length than the chunk list");
	}
	std::vector<ChunkMeta> out;
	out.reserve(n);
	for (DbRef v = p_list.first_child(); v.valid(); v = v.next()) {
		Result<Record> entry = Record::make("chunkMeta entry", v);
		if (!entry) {
			return entry.error();
		}
		ChunkMeta meta;
		const DbRef m = entry->field("meta");
		if (m.valid()) {
			Result<Record> mr = Record::make("chunkMeta meta", m);
			if (!mr) {
				return mr.error();
			}
			Result<std::optional<uint32_t>> first_mip = mr->opt_u32("firstMip");
			if (!first_mip) {
				return first_mip.error();
			}
			meta.first_mip = *first_mip;
		}
		Result<std::optional<uint32_t>> h32 = entry->hash32("h32");
		if (!h32) {
			return h32.error();
		}
		meta.h32 = h32->value_or(0);
		out.push_back(meta);
	}
	return out;
}

} // namespace

Result<BundleManifest> parse_cas_manifest(std::span<const uint8_t> p_element) {
	size_t end = 0;
	Result<DbDocument> doc = DbDocument::parse_element(p_element, 0, end);
	if (!doc) {
		return doc.error();
	}
	if (end > p_element.size()) {
		return detail::truncated("CAS bundle manifest", end, p_element.size());
	}
	Result<Record> bundle = Record::make("CAS bundle manifest", doc->root());
	if (!bundle) {
		return bundle.error();
	}
	BundleManifest out;

	Result<DbRef> ebx_list = bundle->list("ebx");
	if (!ebx_list) {
		return ebx_list.error();
	}
	out.ebx.reserve(ebx_list->child_count());
	for (DbRef v = ebx_list->first_child(); v.valid(); v = v.next()) {
		Result<Record> entry = Record::make("CAS ebx entry", v);
		if (!entry) {
			return entry.error();
		}
		EbxEntry e;
		BF3_TRY(cas_payload(*entry, e.stored_size, e.payload));
		Result<std::string_view> name = entry->str("name");
		if (!name) {
			return name.error();
		}
		Result<Sha1> sha1 = entry->sha1("sha1");
		if (!sha1) {
			return sha1.error();
		}
		Result<std::optional<uint64_t>> original = entry->opt_u64("originalSize");
		if (!original) {
			return original.error();
		}
		e.name = std::string(*name);
		e.sha1 = *sha1;
		e.original_size = original->value_or(e.stored_size);
		out.ebx.push_back(std::move(e));
	}

	Result<DbRef> res_list = bundle->list("res");
	if (!res_list) {
		return res_list.error();
	}
	out.res.reserve(res_list->child_count());
	for (DbRef v = res_list->first_child(); v.valid(); v = v.next()) {
		Result<Record> entry = Record::make("CAS res entry", v);
		if (!entry) {
			return entry.error();
		}
		ResEntry e;
		BF3_TRY(cas_payload(*entry, e.stored_size, e.payload));
		Result<std::string_view> name = entry->str("name");
		if (!name) {
			return name.error();
		}
		Result<Sha1> sha1 = entry->sha1("sha1");
		if (!sha1) {
			return sha1.error();
		}
		Result<std::optional<uint64_t>> original = entry->opt_u64("originalSize");
		if (!original) {
			return original.error();
		}
		Result<std::optional<uint32_t>> res_type = entry->hash32("resType");
		if (!res_type) {
			return res_type.error();
		}
		Result<std::optional<std::span<const uint8_t>>> meta = entry->opt_blob("resMeta");
		if (!meta) {
			return meta.error();
		}
		BF3_TRY(res_meta(*meta, e.res_meta));
		e.name = std::string(*name);
		e.sha1 = *sha1;
		e.original_size = original->value_or(e.stored_size);
		e.res_type = res_type->value_or(0);
		out.res.push_back(std::move(e));
	}

	Result<DbRef> chunk_list = bundle->list("chunks");
	if (!chunk_list) {
		return chunk_list.error();
	}
	const size_t chunk_count = chunk_list->child_count();
	Result<DbRef> meta_list = bundle->list("chunkMeta");
	if (!meta_list) {
		return meta_list.error();
	}
	Result<std::vector<ChunkMeta>> metas = chunk_metas(*meta_list, chunk_count);
	if (!metas) {
		return metas.error();
	}
	out.chunks.reserve(chunk_count);
	size_t at = 0;
	for (DbRef v = chunk_list->first_child(); v.valid(); v = v.next(), ++at) {
		Result<Record> entry = Record::make("CAS chunk entry", v);
		if (!entry) {
			return entry.error();
		}
		ChunkEntry e;
		BF3_TRY(cas_payload(*entry, e.stored_size, e.payload));
		Result<std::optional<uint32_t>> start = entry->opt_u32("rangeStart");
		if (!start) {
			return start.error();
		}
		if (start->has_value()) {
			ChunkRange r;
			r.start = **start;
			Result<std::optional<uint32_t>> end_v = entry->opt_u32("rangeEnd");
			if (!end_v) {
				return end_v.error();
			}
			r.end = end_v->value_or(e.stored_size > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(e.stored_size));
			Result<std::optional<uint32_t>> logical = entry->opt_u32("logicalOffset");
			if (!logical) {
				return logical.error();
			}
			r.logical_offset = logical->value_or(0);
			e.range = r;
		}
		Result<Guid> id = entry->guid("id");
		if (!id) {
			return id.error();
		}
		Result<Sha1> sha1 = entry->sha1("sha1");
		if (!sha1) {
			return sha1.error();
		}
		e.id = *id;
		e.sha1 = *sha1;
		e.tail_only = false;
		if (at < metas->size()) {
			e.meta = (*metas)[at];
		}
		out.chunks.push_back(std::move(e));
	}

	Result<std::optional<std::string_view>> path = bundle->opt_str("path");
	if (!path) {
		return path.error();
	}
	if (path->has_value()) {
		out.path = std::string(**path);
	}
	out.chunk_meta = std::move(*metas);
	return out;
}

Result<uint64_t> binary_manifest_len(std::span<const uint8_t> p_prefix) {
	BF3_TRY(detail::need("binary bundle manifest", p_prefix.size(), 4));
	return 4 + uint64_t(be32(p_prefix.data(), 0));
}

Result<BundleManifest> parse_binary_manifest(std::span<const uint8_t> p_head, uint64_t p_bundle_len) {
	const char *what = "binary bundle manifest";
	BF3_TRY(detail::need(what, p_head.size(), kBinaryHeaderLen));
	Result<uint64_t> meta_end_r = binary_manifest_len(p_head);
	if (!meta_end_r) {
		return meta_end_r.error();
	}
	const uint64_t meta_end = *meta_end_r;
	BF3_TRY(detail::need(what, p_head.size(), static_cast<size_t>(meta_end)));
	const std::span<const uint8_t> head = p_head.first(static_cast<size_t>(meta_end));
	const uint8_t *h = head.data();
	const uint32_t magic = be32(h, 4);
	if (magic != kBinaryManifestMagic && magic != kBinaryManifestAlphaMagic) {
		return detail::bad_magic(what, magic);
	}
	const uint64_t total = be32(h, 0x08);
	const uint64_t ebx_count = be32(h, 0x0C);
	const uint64_t res_count = be32(h, 0x10);
	const uint64_t chunk_count = be32(h, 0x14);
	const uint64_t strings_at = 4 + uint64_t(be32(h, 0x18));
	const uint64_t chunk_meta_at = 4 + uint64_t(be32(h, 0x1C));
	const uint64_t chunk_meta_len = be32(h, 0x20);
	if (ebx_count + res_count + chunk_count != total) { // u32 counts: no u64 overflow
		return detail::layout_error(what, "entry counts do not add up");
	}
	const uint64_t named = ebx_count + res_count;
	const uint64_t tables_len = total * 20 + named * 12 + res_count * 20 + chunk_count * 28 + kBinaryHeaderLen;
	BF3_TRY(detail::need("binary bundle manifest tables", head.size(), static_cast<size_t>(tables_len)));
	if (strings_at > head.size()) {
		return detail::layout_error(what, "string block starts past the manifest");
	}

	size_t at = kBinaryHeaderLen;
	const size_t sha1s_at = at;
	at += static_cast<size_t>(total) * 20;
	const size_t records_at = at;
	at += static_cast<size_t>(named) * 12;
	const size_t res_types_at = at;
	at += static_cast<size_t>(res_count) * 4;
	const size_t res_metas_at = at;
	at += static_cast<size_t>(res_count) * 16;
	const size_t chunk_records_at = at;

	BundleManifest out;
	if (chunk_meta_len != 0) {
		const uint64_t section_end = chunk_meta_at + chunk_meta_len;
		if (section_end > head.size()) {
			return detail::layout_error(what, "chunkMeta runs past the manifest");
		}
		size_t end = 0;
		Result<DbDocument> doc = DbDocument::parse_element(head.first(static_cast<size_t>(section_end)), static_cast<size_t>(chunk_meta_at), end);
		if (!doc) {
			return doc.error();
		}
		if (end != section_end) {
			return detail::layout_error(what, "chunkMeta does not fill its section");
		}
		const DbRef list = doc->root();
		if (!list.is_list()) {
			return detail::field_type(what, "chunkMeta");
		}
		Result<std::vector<ChunkMeta>> metas = chunk_metas(list, static_cast<size_t>(chunk_count));
		if (!metas) {
			return metas.error();
		}
		out.chunk_meta = std::move(*metas);
	}

	uint64_t cursor = meta_end;
	auto place = [&cursor](uint64_t p_len) {
		const uint64_t offset = (cursor + kBinaryPayloadAlign - 1) / kBinaryPayloadAlign * kBinaryPayloadAlign;
		cursor = offset + p_len;
		return offset;
	};

	out.ebx.reserve(static_cast<size_t>(ebx_count));
	out.res.reserve(static_cast<size_t>(res_count));
	for (size_t index = 0; index < named; ++index) {
		const size_t record = records_at + index * 12;
		const uint64_t name_offset = be32(h, record);
		const uint64_t stored_size = be32(h, record + 4);
		const uint64_t original_size = be32(h, record + 8);
		// Name from the string block.
		const uint64_t start = strings_at + name_offset;
		if (start >= head.size()) {
			return detail::layout_error(what, "name offset is past the string block");
		}
		const void *nul = std::memchr(h + start, 0, head.size() - static_cast<size_t>(start));
		if (nul == nullptr) {
			return detail::layout_error(what, "name has no terminator");
		}
		const size_t len = static_cast<size_t>(static_cast<const uint8_t *>(nul) - (h + start));
		if (!detail::valid_utf8(head.subspan(static_cast<size_t>(start), len))) {
			return detail::bad_utf8("binary bundle entry name");
		}
		std::string name(reinterpret_cast<const char *>(h + start), len);
		Payload payload;
		payload.kind = Payload::Kind::Bundle;
		payload.offset = place(stored_size);
		const Sha1 sha1 = Sha1::from_bytes(h + sha1s_at + index * 20);
		if (index < ebx_count) {
			EbxEntry e;
			e.name = std::move(name);
			e.sha1 = sha1;
			e.stored_size = stored_size;
			e.original_size = original_size;
			e.payload = std::move(payload);
			out.ebx.push_back(std::move(e));
		} else {
			const size_t res_index = index - static_cast<size_t>(ebx_count);
			ResEntry e;
			e.name = std::move(name);
			e.sha1 = sha1;
			e.stored_size = stored_size;
			e.original_size = original_size;
			e.res_type = be32(h, res_types_at + res_index * 4);
			std::memcpy(e.res_meta.data(), h + res_metas_at + res_index * 16, 16);
			e.payload = std::move(payload);
			out.res.push_back(std::move(e));
		}
	}

	out.chunks.reserve(static_cast<size_t>(chunk_count));
	for (size_t index = 0; index < chunk_count; ++index) {
		const size_t record = chunk_records_at + index * 28;
		ChunkRange range;
		range.start = be32(h, record + 16);
		range.end = be32(h, record + 20);
		range.logical_offset = be32(h, record + 24);
		if (range.end < range.start) {
			return detail::layout_error("binary bundle chunk", "range ends before it starts");
		}
		ChunkEntry e;
		e.id = Guid::from_big_endian_fields(h + record);
		e.sha1 = Sha1::from_bytes(h + sha1s_at + (static_cast<size_t>(named) + index) * 20);
		e.stored_size = uint64_t(range.end - range.start);
		const bool ranged = range.start != 0 || range.logical_offset != 0;
		if (ranged) {
			e.range = range;
		}
		e.tail_only = ranged;
		e.payload.kind = Payload::Kind::Bundle;
		e.payload.offset = place(e.stored_size);
		out.chunks.push_back(std::move(e));
	}

	if (cursor > p_bundle_len) {
		return detail::truncated("binary bundle payloads", cursor, p_bundle_len);
	}
	return out;
}

} // namespace bf3::core

// EA GenericData archive reader. Clean-room implementation from research/probes/ant-animation-probe.md
// (§3.2) and research/topics/03-animation.md (§1.3); see gd_archive.h for the format summary.
#include "bf3/core/ant/gd_archive.h"

#include <cstring>

namespace bf3::core::ant {

namespace {

constexpr uint32_t kBlobHeader = 16; // id[7], endian, u32 size, u32 relocation table offset
constexpr uint32_t kLayoutHeader = 32;
constexpr uint32_t kEntryHeader = 32;
constexpr uint32_t kDataHeader = 32;
constexpr int32_t kMaxSlots = 1 << 16;

uint16_t le16(const uint8_t *p_at) {
	return uint16_t(p_at[0] | (p_at[1] << 8));
}
uint32_t le32(const uint8_t *p_at) {
	return uint32_t(p_at[0]) | (uint32_t(p_at[1]) << 8) | (uint32_t(p_at[2]) << 16) | (uint32_t(p_at[3]) << 24);
}
uint64_t le64(const uint8_t *p_at) {
	return uint64_t(le32(p_at)) | (uint64_t(le32(p_at + 4)) << 32);
}

// [p_at, p_at + p_bytes) inside [p_begin, p_end), without overflow.
bool in_range(uint64_t p_at, uint64_t p_bytes, uint64_t p_begin, uint64_t p_end) {
	return p_at >= p_begin && p_at <= p_end && p_bytes <= p_end - p_at;
}

// A NUL-terminated string starting at p_at and ending before p_end.
bool c_string(std::span<const uint8_t> p_bytes, uint64_t p_at, uint64_t p_end, std::string_view &r_out) {
	if (p_end > p_bytes.size() || p_at >= p_end) {
		return false;
	}
	const uint8_t *begin = p_bytes.data() + p_at;
	const void *nul = std::memchr(begin, 0, size_t(p_end - p_at));
	if (nul == nullptr) {
		return false;
	}
	r_out = std::string_view(reinterpret_cast<const char *>(begin), size_t(static_cast<const uint8_t *>(nul) - begin));
	return true;
}

uint32_t primitive_size(uint32_t p_hash) {
	switch (static_cast<GdPrimitive>(p_hash)) {
		case GdPrimitive::Bool:
		case GdPrimitive::Int8:
		case GdPrimitive::UInt8:
			return 1;
		case GdPrimitive::Int16:
		case GdPrimitive::UInt16:
			return 2;
		case GdPrimitive::Int32:
		case GdPrimitive::UInt32:
		case GdPrimitive::Float:
			return 4;
		case GdPrimitive::Int64:
		case GdPrimitive::UInt64:
		case GdPrimitive::Double:
		case GdPrimitive::DataRef:
			return 8;
		case GdPrimitive::Vector2:
		case GdPrimitive::Vector3:
		case GdPrimitive::Vector4:
		case GdPrimitive::Quaternion:
		case GdPrimitive::Guid:
		case GdPrimitive::String:
			return 16;
		case GdPrimitive::Matrix44:
			return 64;
	}
	return 0;
}

bool is_integer(uint32_t p_hash) {
	return p_hash >= uint32_t(GdPrimitive::Bool) && p_hash <= uint32_t(GdPrimitive::UInt64);
}

int64_t decode_integer(uint32_t p_hash, const uint8_t *p_at) {
	switch (static_cast<GdPrimitive>(p_hash)) {
		case GdPrimitive::Bool:
			return p_at[0] != 0 ? 1 : 0;
		case GdPrimitive::Int8:
			return int8_t(p_at[0]);
		case GdPrimitive::UInt8:
			return p_at[0];
		case GdPrimitive::Int16:
			return int16_t(le16(p_at));
		case GdPrimitive::UInt16:
			return le16(p_at);
		case GdPrimitive::Int32:
			return int32_t(le32(p_at));
		case GdPrimitive::UInt32:
			return le32(p_at);
		case GdPrimitive::Int64:
		case GdPrimitive::UInt64:
			return int64_t(le64(p_at));
		default:
			return 0;
	}
}

bool decode_number(uint32_t p_hash, const uint8_t *p_at, float &r_value) {
	if (p_hash == uint32_t(GdPrimitive::Float)) {
		const uint32_t bits = le32(p_at);
		std::memcpy(&r_value, &bits, 4);
		return true;
	}
	if (p_hash == uint32_t(GdPrimitive::Double)) {
		const uint64_t bits = le64(p_at);
		double d;
		std::memcpy(&d, &bits, 8);
		r_value = float(d);
		return true;
	}
	if (is_integer(p_hash)) {
		r_value = float(decode_integer(p_hash, p_at));
		return true;
	}
	return false;
}

} // namespace

bool GdGuid::is_null() const {
	for (uint8_t b : bytes) {
		if (b != 0) {
			return false;
		}
	}
	return true;
}

uint32_t GdLayout::stride() const {
	const uint32_t a = alignment == 0 ? 1 : alignment;
	return (data_size + a - 1) / a * a;
}

const GdField *GdLayout::find(std::string_view p_name) const {
	for (const GdField &f : fields) {
		if (f.name == p_name) {
			return &f;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// GdArchive

const GdLayout *GdArchive::layout_by_hash(uint32_t p_hash) const {
	const auto it = by_hash_.find(p_hash);
	return it == by_hash_.end() ? nullptr : &layouts_[it->second];
}

GdStruct GdArchive::object(size_t p_index) const {
	if (p_index >= objects_.size()) {
		return {};
	}
	const GdObject &o = objects_[p_index];
	return GdStruct(this, o.blob_begin, o.blob_end, o.instance, o.layout);
}

GdStruct GdArchive::data_at(uint32_t p_blob_begin, uint32_t p_blob_end, uint64_t p_offset) const {
	const uint64_t at = uint64_t(p_blob_begin) + p_offset;
	if (!in_range(at, kDataHeader, p_blob_begin, p_blob_end)) {
		return {};
	}
	const uint8_t *h = bytes_.data() + at;
	if (le32(h) != kDataMagic) {
		return {};
	}
	const GdLayout *layout = layout_by_hash(le32(h + 16));
	const uint64_t instance = at + le16(h + 28);
	if (layout == nullptr || !in_range(instance, layout->data_size, p_blob_begin, p_blob_end)) {
		return {};
	}
	return GdStruct(this, p_blob_begin, p_blob_end, uint32_t(instance), layout);
}

bool GdArchive::parse_reflection(uint32_t p_begin, uint32_t p_end, std::string &r_error) {
	const uint8_t *d = bytes_.data();
	if (!in_range(p_begin, 8, p_begin, p_end)) {
		r_error = "GD.REFL: truncated";
		return false;
	}
	const uint64_t count = le64(d + p_begin);
	if (count > (p_end - p_begin - 8) / 8) {
		r_error = "GD.REFL: layout count exceeds the blob";
		return false;
	}
	layouts_.clear();
	layouts_.resize(size_t(count));
	std::unordered_map<uint64_t, uint32_t> by_offset;
	std::vector<std::vector<uint64_t>> field_layout_offsets;
	field_layout_offsets.resize(size_t(count));
	for (uint32_t i = 0; i < count; ++i) {
		const uint64_t rel = le64(d + p_begin + 8 + 8ull * i);
		const uint64_t at = uint64_t(p_begin) + rel;
		if (!in_range(at, kLayoutHeader, p_begin, p_end)) {
			r_error = "GD.REFL: layout header out of range";
			return false;
		}
		const uint8_t *h = d + at;
		GdLayout &l = layouts_[i];
		l.min_slot = int32_t(le32(h));
		l.max_slot = int32_t(le32(h + 4));
		l.data_size = le32(h + 8);
		l.alignment = le32(h + 12);
		const uint32_t table_offset = le32(h + 16);
		const uint32_t table_size = le32(h + 20);
		l.recorded = h[24] != 0;
		l.native = h[25] != 0;
		l.hash = le32(h + 28);
		const int64_t slots = int64_t(l.max_slot) - int64_t(l.min_slot) + 1;
		if (slots < 0 || slots > kMaxSlots || l.alignment > 4096) {
			r_error = "GD.REFL: bad slot range or alignment";
			return false;
		}
		if (!in_range(at + kLayoutHeader, uint64_t(slots) * kEntryHeader, p_begin, p_end)) {
			r_error = "GD.REFL: entry table out of range";
			return false;
		}
		const uint64_t table = at + table_offset;
		if (!in_range(table, table_size, p_begin, p_end) || table_size == 0) {
			r_error = "GD.REFL: string table out of range";
			return false;
		}
		// The table starts with an empty string; the type name follows it.
		if (!c_string(bytes_, table + 1, table + table_size, l.name)) {
			r_error = "GD.REFL: bad type name";
			return false;
		}
		l.fields.resize(size_t(slots));
		field_layout_offsets[i].resize(size_t(slots));
		for (int64_t s = 0; s < slots; ++s) {
			const uint8_t *e = d + at + kLayoutHeader + uint64_t(s) * kEntryHeader;
			GdField &f = l.fields[size_t(s)];
			f.slot = int32_t(l.min_slot + s);
			f.layout_hash = le32(e);
			f.element_size = le32(e + 4);
			f.offset = le32(e + 8);
			const uint32_t name_offset = le32(e + 12);
			f.count = le16(e + 16);
			f.flags = le16(e + 18);
			f.element_align = le16(e + 20);
			field_layout_offsets[i][size_t(s)] = le64(e + 24);
			if (name_offset >= table_size || !c_string(bytes_, table + name_offset, table + table_size, f.name)) {
				r_error = "GD.REFL: bad field name";
				return false;
			}
			if (f.layout_hash != 0 && (uint64_t(f.offset) + uint64_t(f.element_size) * (f.count == 0 ? 1 : f.count) > l.data_size)) {
				r_error = "GD.REFL: field outside its layout";
				return false;
			}
		}
		by_offset.emplace(rel, i);
		by_hash_.emplace(l.hash, i);
	}
	// Resolve element layouts (vector storage is final now, so pointers stay valid).
	for (size_t i = 0; i < layouts_.size(); ++i) {
		for (size_t s = 0; s < layouts_[i].fields.size(); ++s) {
			GdField &f = layouts_[i].fields[s];
			if (f.layout_hash == 0) {
				continue;
			}
			const auto it = by_offset.find(field_layout_offsets[i][s]);
			if (it != by_offset.end() && layouts_[it->second].hash == f.layout_hash) {
				f.layout = &layouts_[it->second];
			} else {
				f.layout = layout_by_hash(f.layout_hash);
			}
		}
	}
	return true;
}

bool GdArchive::parse(std::span<const uint8_t> p_bytes, std::string &r_error) {
	bytes_ = p_bytes;
	layouts_.clear();
	by_hash_.clear();
	objects_.clear();
	if (p_bytes.size() > 0xFFFFFFF0u) {
		r_error = "GD archive larger than 4 GiB";
		return false;
	}
	const uint8_t *d = p_bytes.data();
	const uint32_t total = uint32_t(p_bytes.size());
	if (total < 16 || std::memcmp(d, "GD.STRM", 7) != 0) {
		r_error = "no GD.STRM blob";
		return false;
	}
	if (d[7] != 'l') {
		r_error = std::string("GD.STRM endian '") + char(d[7]) + "' is not supported (BF3 PC is 'l')";
		return false;
	}
	const uint32_t stream_size = le32(d + 8);
	if (stream_size < 16 || stream_size > total) {
		r_error = "GD.STRM size out of range";
		return false;
	}
	largest_block_ = le32(d + 12);
	uint32_t at = 16;
	bool have_reflection = false;
	while (at < stream_size) {
		if (!in_range(at, kBlobHeader, 0, stream_size)) {
			r_error = "truncated blob header";
			return false;
		}
		const uint8_t *h = d + at;
		const uint32_t size = le32(h + 8);
		if (size < kBlobHeader || !in_range(at, size, 0, stream_size)) {
			r_error = "blob size out of range";
			return false;
		}
		if (h[7] != 'l') {
			r_error = "blob endian is not 'l'";
			return false;
		}
		const uint32_t begin = at + kBlobHeader;
		const uint32_t end = at + size;
		if (std::memcmp(h, "GD.REFL", 7) == 0) {
			if (have_reflection) {
				r_error = "second GD.REFL blob";
				return false;
			}
			if (!parse_reflection(begin, end, r_error)) {
				return false;
			}
			have_reflection = true;
		} else if (std::memcmp(h, "GD.DATA", 7) == 0) {
			if (!have_reflection) {
				r_error = "GD.DATA before GD.REFL";
				return false;
			}
			GdStruct s = data_at(begin, end, 0);
			if (!s.valid()) {
				r_error = "GD.DATA: bad LayoutData header (magic, layout hash or instance range)";
				return false;
			}
			objects_.push_back(GdObject{ begin, end, s.at_, s.layout_ });
		} else {
			r_error = "unsupported blob '" + std::string(reinterpret_cast<const char *>(h), 7) + "'";
			return false;
		}
		at = end;
	}
	if (!have_reflection) {
		r_error = "no GD.REFL blob";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// GdStruct

std::string_view GdStruct::type_name() const {
	return layout_ ? layout_->name : std::string_view();
}

bool GdStruct::find(std::string_view p_name, GdStruct &r_owner, const GdField *&r_field) const {
	GdStruct cur = *this;
	for (int depth = 0; depth < 16 && cur.valid(); ++depth) {
		if (const GdField *f = cur.layout_->find(p_name)) {
			r_owner = cur;
			r_field = f;
			return true;
		}
		cur = cur.base();
	}
	return false;
}

GdStruct GdStruct::base() const {
	// Only this layout's own "__base" slot (no name lookup through the chain, which would recurse).
	const GdField *f = valid() ? layout_->find("__base") : nullptr;
	uint32_t at;
	if (f == nullptr || f->is_array() || f->layout_hash != uint32_t(GdPrimitive::DataRef) || !field_at(*f, 0, 8, at)) {
		return {};
	}
	const uint64_t ptr = le64(archive_->bytes().data() + at);
	return ptr == 0 ? GdStruct() : archive_->data_at(blob_begin_, blob_end_, ptr);
}

bool GdStruct::has(std::string_view p_name) const {
	GdStruct owner;
	const GdField *f = nullptr;
	return find(p_name, owner, f);
}

bool GdStruct::field_at(const GdField &p_field, uint32_t p_index, uint32_t p_bytes, uint32_t &r_at) const {
	if (!valid() || p_field.layout_hash == 0) {
		return false;
	}
	const uint32_t count = p_field.count == 0 ? 1 : p_field.count;
	if (p_index >= count || p_bytes > p_field.element_size) {
		return false;
	}
	const uint64_t at = uint64_t(at_) + p_field.offset + uint64_t(p_index) * p_field.element_size;
	if (!in_range(at, p_bytes, blob_begin_, blob_end_)) {
		return false;
	}
	r_at = uint32_t(at);
	return true;
}

bool GdStruct::read_int_at(const GdField &p_field, uint32_t p_index, int64_t &r_value) const {
	uint32_t at;
	if (p_field.is_array() || !is_integer(p_field.layout_hash) || !field_at(p_field, p_index, primitive_size(p_field.layout_hash), at)) {
		return false;
	}
	r_value = decode_integer(p_field.layout_hash, archive_->bytes().data() + at);
	return true;
}

bool GdStruct::read_float_at(const GdField &p_field, uint32_t p_index, float &r_value) const {
	uint32_t at;
	if (p_field.is_array() || !field_at(p_field, p_index, primitive_size(p_field.layout_hash), at)) {
		return false;
	}
	return decode_number(p_field.layout_hash, archive_->bytes().data() + at, r_value);
}

bool GdStruct::read_int(std::string_view p_name, int64_t &r_value) const {
	GdStruct owner;
	const GdField *f = nullptr;
	return find(p_name, owner, f) && owner.read_int_at(*f, 0, r_value);
}

bool GdStruct::read_u32(std::string_view p_name, uint32_t &r_value) const {
	int64_t v;
	if (!read_int(p_name, v) || v < 0 || v > int64_t(UINT32_MAX)) {
		return false;
	}
	r_value = uint32_t(v);
	return true;
}

bool GdStruct::read_float(std::string_view p_name, float &r_value) const {
	GdStruct owner;
	const GdField *f = nullptr;
	return find(p_name, owner, f) && owner.read_float_at(*f, 0, r_value);
}

bool GdStruct::read_bool(std::string_view p_name, bool &r_value) const {
	int64_t v;
	if (!read_int(p_name, v)) {
		return false;
	}
	r_value = v != 0;
	return true;
}

bool GdStruct::read_string(std::string_view p_name, std::string_view &r_value) const {
	GdStruct owner;
	const GdField *f = nullptr;
	uint32_t at;
	if (!find(p_name, owner, f) || f->is_array() || f->layout_hash != uint32_t(GdPrimitive::String) || !owner.field_at(*f, 0, 16, at)) {
		return false;
	}
	const uint8_t *s = archive_->bytes().data() + at;
	const uint32_t count = le32(s + 4);
	const uint64_t ptr = le64(s + 8);
	if (count == 0) {
		r_value = {};
		return true;
	}
	const uint64_t begin = uint64_t(owner.blob_begin_) + ptr;
	if (ptr > owner.blob_end_ || !in_range(begin, count, owner.blob_begin_, owner.blob_end_)) {
		return false;
	}
	const char *c = reinterpret_cast<const char *>(archive_->bytes().data() + begin);
	const void *nul = std::memchr(c, 0, count);
	r_value = std::string_view(c, nul ? size_t(static_cast<const char *>(nul) - c) : count);
	return true;
}

bool GdStruct::read_guid(std::string_view p_name, GdGuid &r_value) const {
	GdStruct owner;
	const GdField *f = nullptr;
	uint32_t at;
	if (!find(p_name, owner, f) || f->is_array() || f->layout_hash != uint32_t(GdPrimitive::Guid) || !owner.field_at(*f, 0, 16, at)) {
		return false;
	}
	std::memcpy(r_value.bytes, archive_->bytes().data() + at, 16);
	return true;
}

bool GdStruct::read_vec4(std::string_view p_name, float r_value[4]) const {
	GdStruct owner;
	const GdField *f = nullptr;
	if (!find(p_name, owner, f) || f->is_array()) {
		return false;
	}
	const uint32_t h = f->layout_hash;
	const uint32_t n = h == uint32_t(GdPrimitive::Vector2) ? 2 : h == uint32_t(GdPrimitive::Vector3) ? 3 : (h == uint32_t(GdPrimitive::Vector4) || h == uint32_t(GdPrimitive::Quaternion)) ? 4 : 0;
	uint32_t at;
	if (n == 0 || !owner.field_at(*f, 0, 4 * n, at)) {
		return false;
	}
	for (uint32_t i = 0; i < 4; ++i) {
		r_value[i] = 0.0f;
		if (i < n) {
			decode_number(uint32_t(GdPrimitive::Float), archive_->bytes().data() + at + 4 * i, r_value[i]);
		}
	}
	return true;
}

GdArray GdStruct::array(std::string_view p_name) const {
	GdStruct owner;
	const GdField *f = nullptr;
	uint32_t at;
	if (!find(p_name, owner, f) || !f->is_array() || !owner.field_at(*f, 0, 16, at)) {
		return {};
	}
	const uint8_t *c = archive_->bytes().data() + at;
	GdArray a;
	a.archive_ = archive_;
	a.blob_begin_ = owner.blob_begin_;
	a.blob_end_ = owner.blob_end_;
	a.hash_ = f->layout_hash;
	a.element_ = f->layout;
	if (a.element_ == nullptr) {
		return {};
	}
	// Element stride = align(dataSize, alignment) of the element layout (Guid 16, String 16, DataRef 8, ...).
	a.stride_ = a.element_->stride();
	if (f->is_primitive() && a.stride_ < primitive_size(f->layout_hash)) {
		return {};
	}
	const uint32_t count = le32(c + 4);
	const uint64_t ptr = le64(c + 8);
	if (count == 0) {
		a.at_ = owner.blob_begin_;
		return a;
	}
	if (a.stride_ == 0 || ptr > owner.blob_end_) {
		return {};
	}
	const uint64_t begin = uint64_t(owner.blob_begin_) + ptr;
	if (!in_range(begin, uint64_t(count) * a.stride_, owner.blob_begin_, owner.blob_end_)) {
		return {};
	}
	a.at_ = uint32_t(begin);
	a.count_ = count;
	return a;
}

GdStruct GdStruct::ref(std::string_view p_name) const {
	GdStruct owner;
	const GdField *f = nullptr;
	uint32_t at;
	if (!find(p_name, owner, f) || f->is_array() || f->layout_hash != uint32_t(GdPrimitive::DataRef) || !owner.field_at(*f, 0, 8, at)) {
		return {};
	}
	const uint64_t ptr = le64(archive_->bytes().data() + at);
	if (ptr == 0) {
		return {};
	}
	return archive_->data_at(owner.blob_begin_, owner.blob_end_, ptr);
}

GdStruct GdStruct::embedded(std::string_view p_name) const {
	GdStruct owner;
	const GdField *f = nullptr;
	uint32_t at;
	if (!find(p_name, owner, f) || f->is_array() || f->layout == nullptr || !owner.field_at(*f, 0, f->layout->data_size, at)) {
		return {};
	}
	return GdStruct(archive_, owner.blob_begin_, owner.blob_end_, at, f->layout);
}

std::string_view GdStruct::name() const {
	std::string_view n;
	return read_string("__name", n) ? n : std::string_view();
}

GdGuid GdStruct::guid() const {
	GdGuid g;
	read_guid("__guid", g);
	return g;
}

// ---------------------------------------------------------------------------------------------
// GdArray

std::span<const uint8_t> GdArray::raw() const {
	if (count_ == 0) {
		return {};
	}
	return archive_->bytes().subspan(at_, size_t(count_) * stride_);
}

bool GdArray::read_int(uint32_t p_index, int64_t &r_value) const {
	if (p_index >= count_ || !is_integer(hash_)) {
		return false;
	}
	r_value = decode_integer(hash_, archive_->bytes().data() + at_ + size_t(p_index) * stride_);
	return true;
}

bool GdArray::read_float(uint32_t p_index, float &r_value) const {
	if (p_index >= count_) {
		return false;
	}
	return decode_number(hash_, archive_->bytes().data() + at_ + size_t(p_index) * stride_, r_value);
}

bool GdArray::read_guid(uint32_t p_index, GdGuid &r_value) const {
	if (p_index >= count_ || hash_ != uint32_t(GdPrimitive::Guid)) {
		return false;
	}
	std::memcpy(r_value.bytes, archive_->bytes().data() + at_ + size_t(p_index) * stride_, 16);
	return true;
}

bool GdArray::read_string(uint32_t p_index, std::string_view &r_value) const {
	if (p_index >= count_ || hash_ != uint32_t(GdPrimitive::String)) {
		return false;
	}
	const uint8_t *s = archive_->bytes().data() + at_ + size_t(p_index) * stride_;
	const uint32_t count = le32(s + 4);
	const uint64_t ptr = le64(s + 8);
	if (count == 0) {
		r_value = {};
		return true;
	}
	const uint64_t begin = uint64_t(blob_begin_) + ptr;
	if (ptr > blob_end_ || !in_range(begin, count, blob_begin_, blob_end_)) {
		return false;
	}
	const char *c = reinterpret_cast<const char *>(archive_->bytes().data() + begin);
	const void *nul = std::memchr(c, 0, count);
	r_value = std::string_view(c, nul ? size_t(static_cast<const char *>(nul) - c) : count);
	return true;
}

GdStruct GdArray::element(uint32_t p_index) const {
	if (p_index >= count_ || element_ == nullptr) {
		return {};
	}
	const uint32_t at = at_ + p_index * stride_;
	if (hash_ == uint32_t(GdPrimitive::DataRef)) {
		return archive_->data_at(blob_begin_, blob_end_, le64(archive_->bytes().data() + at));
	}
	if (!in_range(at, element_->data_size, blob_begin_, blob_end_)) {
		return {};
	}
	return GdStruct(archive_, blob_begin_, blob_end_, at, element_);
}

namespace {
template <typename T>
bool copy_primitive(const GdArray &p_array, uint32_t p_hash_a, uint32_t p_hash_b, std::vector<T> &r_out) {
	if (p_array.size() != 0 && p_array.element_hash() != p_hash_a && p_array.element_hash() != p_hash_b) {
		return false;
	}
	if (p_array.size() != 0 && p_array.stride() != sizeof(T)) {
		return false;
	}
	const std::span<const uint8_t> raw = p_array.raw();
	r_out.resize(p_array.size());
	if (!raw.empty()) {
		std::memcpy(r_out.data(), raw.data(), raw.size()); // little-endian host (x64)
	}
	return true;
}
} // namespace

bool GdArray::copy_u8(std::vector<uint8_t> &r_out) const {
	return copy_primitive(*this, uint32_t(GdPrimitive::UInt8), uint32_t(GdPrimitive::Int8), r_out);
}
bool GdArray::copy_i16(std::vector<int16_t> &r_out) const {
	return copy_primitive(*this, uint32_t(GdPrimitive::Int16), uint32_t(GdPrimitive::UInt16), r_out);
}
bool GdArray::copy_u16(std::vector<uint16_t> &r_out) const {
	return copy_primitive(*this, uint32_t(GdPrimitive::UInt16), uint32_t(GdPrimitive::Int16), r_out);
}
bool GdArray::copy_f32(std::vector<float> &r_out) const {
	return copy_primitive(*this, uint32_t(GdPrimitive::Float), uint32_t(GdPrimitive::Float), r_out);
}

bool gd_is_a(const GdStruct &p_struct, std::string_view p_type_name) {
	GdStruct cur = p_struct;
	for (int depth = 0; depth < 16 && cur.valid(); ++depth) {
		if (cur.type_name() == p_type_name) {
			return true;
		}
		cur = cur.base();
	}
	return false;
}

} // namespace bf3::core::ant

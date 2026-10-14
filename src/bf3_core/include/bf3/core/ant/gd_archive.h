#pragma once
// EA GenericData ("GD") archive reader: the self-describing container inside Ant AssetBanks.
//
// Clean-room implementation from research/probes/ant-animation-probe.md (§3.2) and
// research/topics/03-animation.md (§1.3). No code from Rime, IceBloc or AssetBankPlugin was used.
//
// Layout of a GD.STRM (all little-endian on BF3 PC; the 'l' endian byte is checked):
//   blob header   char[7] id, char endian, u32 size (incl. header), u32 relocation table offset
//   GD.STRM       header (12 bytes) + u32 sizeOfLargestBlock, then one GD.REFL blob, then N GD.DATA blobs
//   GD.REFL       u64 count, u64[count] offsets of LayoutHeaders (relative to the blob payload)
//   LayoutHeader  i32 minSlot, maxSlot; u32 dataSize, alignment, stringTableOffset, stringTableSize;
//                 u8 recorded, native; pad2; u32 hash; then 32-byte EntryHeaders (one per slot)
//   EntryHeader   u32 layoutHash, elementSize, offset, nameOffset; u16 count, flags, elementAlign; i16 rle;
//                 u64 layoutOffset (relative to the REFL payload)
//   GD.DATA       LayoutData { u32 magic 0x030E6205, pad; u64 allocator; u64 layout (hash in low 32 bits);
//                 i32 refCount; u16 dataOffset; u8 mutable } and the instance at payload + dataOffset
// Strings and dynamic arrays are {u32 capacity, u32 count, u64 offset}; DataRef is a u64 offset to another
// LayoutData; all offsets inside a GD.DATA blob are relative to that blob's payload (byte 16 of the blob).
//
// The bytes are untrusted: every offset and length read from them is bounds-checked. A malformed archive
// makes parse() fail with a message; accessors on a parsed archive return false / empty values instead of
// reading outside the blob they belong to.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::ant {

// Primitive layout hashes (the archive also carries a layout for each of them).
enum class GdPrimitive : uint32_t {
	Bool = 1,
	Int8 = 2,
	UInt8 = 3,
	Int16 = 4,
	UInt16 = 5,
	Int32 = 6,
	UInt32 = 7,
	Int64 = 8,
	UInt64 = 9,
	Float = 10,
	Vector2 = 11,
	Vector3 = 12,
	Vector4 = 13,
	Quaternion = 14,
	Matrix44 = 15,
	Guid = 16,
	String = 17,
	DataRef = 18,
	Double = 19,
};

// A 16-byte Ant object id. The first 4 bytes are a big-endian reference index; bit 0x80000 marks an object of
// the static package (s_BasicAssets), otherwise the target lives in the referencing package.
struct GdGuid {
	static constexpr uint32_t kStaticBit = 0x80000;
	uint8_t bytes[16] = {};

	uint32_t index() const {
		return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
	}
	bool is_static() const { return (index() & kStaticBit) != 0; }
	bool is_null() const;
};

struct GdLayout;

struct GdField {
	std::string_view name;
	int32_t slot = 0;
	uint32_t layout_hash = 0; // 1..19 primitive, 0 = empty ("Invalid") slot, else a struct layout hash
	uint32_t element_size = 0; // per element; 16 for a dynamic array container
	uint32_t offset = 0; // from the start of the instance
	uint16_t count = 1; // > 1: fixed-size array of element_size-byte elements
	uint16_t flags = 0; // 1 = dynamic array; 2/4/8 = force-align
	uint16_t element_align = 0;
	const GdLayout *layout = nullptr; // element layout (resolved through layoutOffset, else the hash)

	bool is_array() const { return (flags & 1) != 0; }
	bool is_primitive() const { return layout_hash >= 1 && layout_hash <= 19; }
};

struct GdLayout {
	std::string_view name;
	uint32_t hash = 0;
	int32_t min_slot = 0;
	int32_t max_slot = -1;
	uint32_t data_size = 0;
	uint32_t alignment = 1;
	bool recorded = false;
	bool native = false;
	std::vector<GdField> fields; // slot order (min_slot first)

	// Element stride inside a dynamic array: align(data_size, alignment).
	uint32_t stride() const;
	const GdField *find(std::string_view p_name) const;
};

class GdArchive;
class GdArray;

// A view of one instance (a whole object or an embedded struct) inside a GD.DATA blob.
class GdStruct {
public:
	GdStruct() = default;

	bool valid() const { return layout_ != nullptr; }
	const GdLayout *layout() const { return layout_; }
	std::string_view type_name() const;

	// Field lookup by name. Searches this layout, then the "__base" chain.
	bool find(std::string_view p_name, GdStruct &r_owner, const GdField *&r_field) const;
	bool has(std::string_view p_name) const;

	// Typed reads. They fail (return false) when the field is missing, has an incompatible type, or lies
	// outside the blob.
	bool read_int(std::string_view p_name, int64_t &r_value) const; // Bool, Int8..UInt64
	bool read_u32(std::string_view p_name, uint32_t &r_value) const; // any integer that fits
	bool read_float(std::string_view p_name, float &r_value) const; // Float, Double, or an integer
	bool read_bool(std::string_view p_name, bool &r_value) const;
	bool read_string(std::string_view p_name, std::string_view &r_value) const;
	bool read_guid(std::string_view p_name, GdGuid &r_value) const;
	// Vector2/3/4/Quaternion: reads up to 4 floats (missing components stay 0).
	bool read_vec4(std::string_view p_name, float r_value[4]) const;

	GdArray array(std::string_view p_name) const; // dynamic array (empty view if missing)
	GdStruct ref(std::string_view p_name) const; // DataRef target (invalid if null or missing)
	GdStruct embedded(std::string_view p_name) const; // struct-typed field
	GdStruct base() const; // the base-class part ("__base" DataRef of this layout; invalid if none)

	std::string_view name() const; // "__name" ("" if absent)
	GdGuid guid() const; // "__guid" (all zero if absent)

	// Same, but on a known field of this struct (no name lookup).
	bool read_int_at(const GdField &p_field, uint32_t p_index, int64_t &r_value) const;
	bool read_float_at(const GdField &p_field, uint32_t p_index, float &r_value) const;

private:
	friend class GdArchive;
	friend class GdArray;
	GdStruct(const GdArchive *p_archive, uint32_t p_blob_begin, uint32_t p_blob_end, uint32_t p_at, const GdLayout *p_layout) :
			archive_(p_archive), blob_begin_(p_blob_begin), blob_end_(p_blob_end), at_(p_at), layout_(p_layout) {}
	// Absolute offset of element p_index of a field, after checking that p_bytes fit in the blob.
	bool field_at(const GdField &p_field, uint32_t p_index, uint32_t p_bytes, uint32_t &r_at) const;

	const GdArchive *archive_ = nullptr;
	uint32_t blob_begin_ = 0; // payload start (offsets inside the blob are relative to it)
	uint32_t blob_end_ = 0;
	uint32_t at_ = 0;
	const GdLayout *layout_ = nullptr;
};

// A dynamic array {u32 capacity, u32 count, u64 offset} of primitives or structs.
class GdArray {
public:
	GdArray() = default;

	bool valid() const { return element_ != nullptr; }
	uint32_t size() const { return count_; }
	bool empty() const { return count_ == 0; }
	uint32_t stride() const { return stride_; }
	uint32_t element_hash() const { return hash_; }
	const GdLayout *element_layout() const { return element_; }

	// The contiguous element bytes (size() * stride()), already bounds-checked.
	std::span<const uint8_t> raw() const;

	bool read_int(uint32_t p_index, int64_t &r_value) const;
	bool read_float(uint32_t p_index, float &r_value) const;
	bool read_guid(uint32_t p_index, GdGuid &r_value) const;
	bool read_string(uint32_t p_index, std::string_view &r_value) const;
	GdStruct element(uint32_t p_index) const; // struct elements (and Vector*/DataRef targets)

	// Copies a primitive array; fails if the element type does not match T's size and signedness class.
	bool copy_u8(std::vector<uint8_t> &r_out) const;
	bool copy_i16(std::vector<int16_t> &r_out) const;
	bool copy_u16(std::vector<uint16_t> &r_out) const;
	bool copy_f32(std::vector<float> &r_out) const;

private:
	friend class GdStruct;
	const GdArchive *archive_ = nullptr;
	uint32_t blob_begin_ = 0;
	uint32_t blob_end_ = 0;
	uint32_t at_ = 0; // absolute offset of element 0
	uint32_t count_ = 0;
	uint32_t stride_ = 0;
	uint32_t hash_ = 0;
	const GdLayout *element_ = nullptr;
};

struct GdObject {
	uint32_t blob_begin = 0; // payload start
	uint32_t blob_end = 0;
	uint32_t instance = 0;
	const GdLayout *layout = nullptr;
};

class GdArchive {
public:
	static constexpr uint32_t kDataMagic = 0x030E6205;

	// Views (GdStruct, GdArray) point at the archive, so it is neither copied nor moved.
	GdArchive() = default;
	GdArchive(const GdArchive &) = delete;
	GdArchive &operator=(const GdArchive &) = delete;

	// p_bytes start at the "GD.STRM" blob and must outlive the archive (views point into them).
	bool parse(std::span<const uint8_t> p_bytes, std::string &r_error);

	std::span<const uint8_t> bytes() const { return bytes_; }
	const std::vector<GdLayout> &layouts() const { return layouts_; }
	const GdLayout *layout_by_hash(uint32_t p_hash) const;
	size_t object_count() const { return objects_.size(); }
	const GdObject &object_info(size_t p_index) const { return objects_[p_index]; }
	GdStruct object(size_t p_index) const;
	uint32_t largest_block() const { return largest_block_; }

	// Follows a DataRef offset inside a blob to its LayoutData (internal; bounds-checked).
	GdStruct data_at(uint32_t p_blob_begin, uint32_t p_blob_end, uint64_t p_offset) const;

private:
	bool parse_reflection(uint32_t p_begin, uint32_t p_end, std::string &r_error);

	std::span<const uint8_t> bytes_;
	std::vector<GdLayout> layouts_;
	std::unordered_map<uint32_t, uint32_t> by_hash_;
	std::vector<GdObject> objects_;
	uint32_t largest_block_ = 0;
};

// True if p_struct's type is p_type_name or derives from it. The base type is only known from the data
// ("__base" is a DataRef to the base part), so this walks the instance's base chain (at most 16 levels).
bool gd_is_a(const GdStruct &p_struct, std::string_view p_type_name);

} // namespace bf3::core::ant

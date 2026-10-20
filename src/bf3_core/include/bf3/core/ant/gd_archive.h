#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::ant {

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
	uint32_t layout_hash = 0;
	uint32_t element_size = 0;
	uint32_t offset = 0;
	uint16_t count = 1;
	uint16_t flags = 0;
	uint16_t element_align = 0;
	const GdLayout *layout = nullptr;

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
	std::vector<GdField> fields;

	uint32_t stride() const;
	const GdField *find(std::string_view p_name) const;
};

class GdArchive;
class GdArray;

class GdStruct {
public:
	GdStruct() = default;

	bool valid() const { return layout_ != nullptr; }
	const GdLayout *layout() const { return layout_; }
	std::string_view type_name() const;

	bool find(std::string_view p_name, GdStruct &r_owner, const GdField *&r_field) const;
	bool has(std::string_view p_name) const;

	bool read_int(std::string_view p_name, int64_t &r_value) const;
	bool read_u32(std::string_view p_name, uint32_t &r_value) const;
	bool read_float(std::string_view p_name, float &r_value) const;
	bool read_bool(std::string_view p_name, bool &r_value) const;
	bool read_string(std::string_view p_name, std::string_view &r_value) const;
	bool read_guid(std::string_view p_name, GdGuid &r_value) const;

	bool read_vec4(std::string_view p_name, float r_value[4]) const;

	GdArray array(std::string_view p_name) const;
	GdStruct ref(std::string_view p_name) const;
	GdStruct embedded(std::string_view p_name) const;
	GdStruct base() const;

	std::string_view name() const;
	GdGuid guid() const;

	bool read_int_at(const GdField &p_field, uint32_t p_index, int64_t &r_value) const;
	bool read_float_at(const GdField &p_field, uint32_t p_index, float &r_value) const;

private:
	friend class GdArchive;
	friend class GdArray;
	GdStruct(const GdArchive *p_archive, uint32_t p_blob_begin, uint32_t p_blob_end, uint32_t p_at, const GdLayout *p_layout) :
			archive_(p_archive), blob_begin_(p_blob_begin), blob_end_(p_blob_end), at_(p_at), layout_(p_layout) {}

	bool field_at(const GdField &p_field, uint32_t p_index, uint32_t p_bytes, uint32_t &r_at) const;

	const GdArchive *archive_ = nullptr;
	uint32_t blob_begin_ = 0;
	uint32_t blob_end_ = 0;
	uint32_t at_ = 0;
	const GdLayout *layout_ = nullptr;
};

class GdArray {
public:
	GdArray() = default;

	bool valid() const { return element_ != nullptr; }
	uint32_t size() const { return count_; }
	bool empty() const { return count_ == 0; }
	uint32_t stride() const { return stride_; }
	uint32_t element_hash() const { return hash_; }
	const GdLayout *element_layout() const { return element_; }

	std::span<const uint8_t> raw() const;

	bool read_int(uint32_t p_index, int64_t &r_value) const;
	bool read_float(uint32_t p_index, float &r_value) const;
	bool read_guid(uint32_t p_index, GdGuid &r_value) const;
	bool read_string(uint32_t p_index, std::string_view &r_value) const;
	GdStruct element(uint32_t p_index) const;

	bool copy_u8(std::vector<uint8_t> &r_out) const;
	bool copy_i16(std::vector<int16_t> &r_out) const;
	bool copy_u16(std::vector<uint16_t> &r_out) const;
	bool copy_f32(std::vector<float> &r_out) const;

private:
	friend class GdStruct;
	const GdArchive *archive_ = nullptr;
	uint32_t blob_begin_ = 0;
	uint32_t blob_end_ = 0;
	uint32_t at_ = 0;
	uint32_t count_ = 0;
	uint32_t stride_ = 0;
	uint32_t hash_ = 0;
	const GdLayout *element_ = nullptr;
};

struct GdObject {
	uint32_t blob_begin = 0;
	uint32_t blob_end = 0;
	uint32_t instance = 0;
	const GdLayout *layout = nullptr;
};

class GdArchive {
public:
	static constexpr uint32_t kDataMagic = 0x030E6205;

	GdArchive() = default;
	GdArchive(const GdArchive &) = delete;
	GdArchive &operator=(const GdArchive &) = delete;

	bool parse(std::span<const uint8_t> p_bytes, std::string &r_error);

	std::span<const uint8_t> bytes() const { return bytes_; }
	const std::vector<GdLayout> &layouts() const { return layouts_; }
	const GdLayout *layout_by_hash(uint32_t p_hash) const;
	size_t object_count() const { return objects_.size(); }
	const GdObject &object_info(size_t p_index) const { return objects_[p_index]; }
	GdStruct object(size_t p_index) const;
	uint32_t largest_block() const { return largest_block_; }

	GdStruct data_at(uint32_t p_blob_begin, uint32_t p_blob_end, uint64_t p_offset) const;

private:
	bool parse_reflection(uint32_t p_begin, uint32_t p_end, std::string &r_error);

	std::span<const uint8_t> bytes_;
	std::vector<GdLayout> layouts_;
	std::unordered_map<uint32_t, uint32_t> by_hash_;
	std::vector<GdObject> objects_;
	uint32_t largest_block_ = 0;
};

bool gd_is_a(const GdStruct &p_struct, std::string_view p_type_name);

}

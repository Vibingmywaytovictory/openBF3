#pragma once
// EBX partitions (Frostbite 2, PC, little-endian), read through their own type descriptors.
// Ported from IW4L ebx_bf3 (partition.rs, value.rs, error.rs, lib.rs), Apache-2.0.
//
// File map:
//   0x00 header 80 B | imports ×32 | name table | field descs ×16 | align16 | complex descs ×16 |
//   instance repeaters ×12 | align16 | array repeaters ×12 | align16 == metaSize |
//   string table | instance payload | array section to EOF (EOF == metaSize + payloadSize)
// A partition is validated in full when parsed (every reference, array index and string offset),
// so decoding a value afterwards never meets a bad one. Values are decoded on access from the
// kept bytes; nothing is compiled in about the types (field offsets come from the file).
// Type codes: 0 Void ('$' base / enumerator), 2 ValueType, 3 Class, 4 Array, 7 CString, 8 Enum,
// 9 FileRef, 10 Boolean, 11..18 Int8..UInt64, 19 Float32, 20 Float64, 21 Guid, 22 Sha1. FB2 has no
// boxed values, TypeRef or ResourceRef; other codes are rejected. Array-of-enum elements carry no
// enum type, so they decode to the value only.

#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

// The hash EBX uses for type, field and enumerator names and connection field ids:
// h = 5381; h = (h * 33) ^ byte, case-sensitive (unlike asset_name_hash).
uint32_t ebx_name_hash(std::string_view p_name);

namespace ebx_code {
inline constexpr uint8_t kVoid = 0;
inline constexpr uint8_t kValueType = 2;
inline constexpr uint8_t kClass = 3;
inline constexpr uint8_t kArray = 4;
inline constexpr uint8_t kCString = 7;
inline constexpr uint8_t kEnum = 8;
inline constexpr uint8_t kFileRef = 9;
inline constexpr uint8_t kBoolean = 10;
inline constexpr uint8_t kInt8 = 11;
inline constexpr uint8_t kUInt8 = 12;
inline constexpr uint8_t kInt16 = 13;
inline constexpr uint8_t kUInt16 = 14;
inline constexpr uint8_t kInt32 = 15;
inline constexpr uint8_t kUInt32 = 16;
inline constexpr uint8_t kInt64 = 17;
inline constexpr uint8_t kUInt64 = 18;
inline constexpr uint8_t kFloat32 = 19;
inline constexpr uint8_t kFloat64 = 20;
inline constexpr uint8_t kGuid = 21;
inline constexpr uint8_t kSha1 = 22;
} // namespace ebx_code

struct EbxImport {
	Guid partition;
	Guid instance;
	bool operator==(const EbxImport &) const = default;
};

struct EbxRef {
	enum class Kind : uint8_t { Null, Internal, External };
	Kind kind = Kind::Null;
	uint32_t index = 0; // Internal: instance index in the partition the reference was read from
	Guid partition; // External
	Guid instance; // External

	static EbxRef null() { return {}; }
	static EbxRef internal(uint32_t p_index) { return { Kind::Internal, p_index, {}, {} }; }
	static EbxRef external(const Guid &p_partition, const Guid &p_instance) { return { Kind::External, 0, p_partition, p_instance }; }
	bool is_null() const { return kind == Kind::Null; }
	bool operator==(const EbxRef &) const = default;
};

class EbxPartition;
class EbxValue;
class EbxFieldIterator;
class EbxArrayIterator;

// An inline struct (ValueType) or the data of an instance: a complex at a byte offset.
class EbxStruct {
public:
	EbxStruct() = default;
	EbxStruct(const EbxPartition *p_partition, uint32_t p_complex, uint32_t p_offset) :
			partition_(p_partition), complex_(p_complex), offset_(p_offset) {}

	const EbxPartition &partition() const { return *partition_; }
	uint32_t complex() const { return complex_; }
	uint32_t offset() const { return offset_; }
	std::string_view type_name() const;
	// The type first, then each base type up to the root.
	std::vector<std::string_view> type_chain() const;
	bool is_a(std::string_view p_type_name) const;
	// The most derived field of that name.
	std::optional<EbxValue> get(std::string_view p_field) const;
	// Dotted field path with optional array indices, e.g. "Transform.trans.x" or "Items[3].Name".
	std::optional<EbxValue> get_path(std::string_view p_path) const;

	// Every field, base types first, in declaration order (the '$' base fields themselves skipped).
	struct FieldRange;
	FieldRange fields() const;

	std::optional<std::array<float, 3>> vec3() const;
	// A LinearTransform as its rows right, up, forward, trans.
	std::optional<std::array<std::array<float, 3>, 4>> transform() const;

	bool operator==(const EbxStruct &p_other) const {
		return partition_ == p_other.partition_ && complex_ == p_other.complex_ && offset_ == p_other.offset_;
	}

private:
	const EbxPartition *partition_ = nullptr;
	uint32_t complex_ = 0;
	uint32_t offset_ = 0;
};

class EbxArray {
public:
	EbxArray() = default;
	EbxArray(const EbxPartition *p_partition, uint32_t p_member, uint32_t p_start, uint32_t p_len, uint32_t p_stride) :
			partition_(p_partition), member_(p_member), start_(p_start), len_(p_len), stride_(p_stride) {}

	size_t size() const { return len_; }
	bool empty() const { return len_ == 0; }
	std::optional<EbxValue> get(size_t p_index) const;
	EbxArrayIterator begin() const;
	EbxArrayIterator end() const;

	bool operator==(const EbxArray &p_other) const {
		return partition_ == p_other.partition_ && member_ == p_other.member_ && start_ == p_other.start_ && len_ == p_other.len_;
	}

private:
	const EbxPartition *partition_ = nullptr;
	uint32_t member_ = 0;
	uint32_t start_ = 0;
	uint32_t len_ = 0;
	uint32_t stride_ = 0;
};

// What EbxValue::as_array() returns: std::optional<EbxArray> semantics, except that dereferencing
// a temporary yields the EbxArray by value. With std::optional, `*value.as_array()` on the
// temporary returns EbxArray&& into storage that C++20 destroys at the end of a range-for's
// range-init, so `for (EbxValue e : *value.as_array())` would iterate a dead object; here the
// copy is bound to the loop's range and lives as long as the loop. Empty: dereferences to an
// empty EbxArray (never undefined).
class OptionalEbxArray {
public:
	constexpr OptionalEbxArray() = default;
	constexpr OptionalEbxArray(std::nullopt_t) {}
	OptionalEbxArray(const EbxArray &p_array) :
			array_(p_array), has_(true) {}

	bool has_value() const { return has_; }
	explicit operator bool() const { return has_; }
	const EbxArray &operator*() const & { return array_; }
	EbxArray operator*() const && { return array_; }
	const EbxArray *operator->() const { return &array_; }
	const EbxArray &value() const & { return array_; }
	EbxArray value() const && { return array_; }
	std::optional<EbxArray> to_optional() const { return has_ ? std::optional<EbxArray>(array_) : std::nullopt; }

	bool operator==(std::nullopt_t) const { return !has_; }
	bool operator==(const OptionalEbxArray &p_other) const { return has_ == p_other.has_ && (!has_ || array_ == p_other.array_); }

private:
	EbxArray array_;
	bool has_ = false;
};

// One decoded field value. Strings and names point into the partition (valid while it lives).
class EbxValue {
public:
	enum class Kind : uint8_t { Bool, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Guid, Sha1, String, Enum, Ref, Struct, Array };

	static EbxValue make_bool(bool p_v);
	static EbxValue make_int(Kind p_kind, int64_t p_v); // I8..I64
	static EbxValue make_u64(Kind p_kind, uint64_t p_v); // U8..U64
	static EbxValue make_f32(float p_v);
	static EbxValue make_f64(double p_v);
	static EbxValue make_guid(const Guid &p_v);
	static EbxValue make_sha1(const Sha1 &p_v);
	static EbxValue make_string(std::optional<std::string_view> p_v); // nullopt = null string
	static EbxValue make_enum(int32_t p_value, std::optional<std::string_view> p_name);
	static EbxValue make_ref(const EbxRef &p_v);
	static EbxValue make_struct(const EbxStruct &p_v);
	static EbxValue make_array(const EbxArray &p_v);

	Kind kind() const { return kind_; }

	std::optional<bool> as_bool() const;
	// Any integer or enum value that fits.
	std::optional<int64_t> as_i64() const;
	std::optional<uint64_t> as_u64() const;
	std::optional<int32_t> as_i32() const;
	std::optional<uint32_t> as_u32() const;
	std::optional<float> as_f32() const;
	std::optional<double> as_f64() const;
	// A null string reads as empty.
	std::optional<std::string_view> as_str() const;
	bool is_null_string() const { return kind_ == Kind::String && !has_text_; }
	std::optional<std::string_view> enum_name() const;
	std::optional<int32_t> enum_value() const;
	std::optional<Guid> as_guid() const;
	std::optional<Sha1> as_sha1() const;
	std::optional<EbxRef> as_ref() const;
	std::optional<EbxStruct> as_struct() const;
	OptionalEbxArray as_array() const; // `for (EbxValue e : *v.as_array())` is safe; see OptionalEbxArray
	bool is_null() const;

	std::optional<EbxValue> get(std::string_view p_field) const;
	std::optional<EbxValue> get_path(std::string_view p_path) const;
	std::optional<std::array<float, 3>> as_vec3() const;
	std::optional<std::array<std::array<float, 3>, 4>> as_transform() const;

private:
	Kind kind_ = Kind::Bool;
	bool has_text_ = false; // String: not null; Enum: has a name
	union {
		int64_t i;
		uint64_t u;
		float f32;
		double f64;
	} num_{};
	std::string_view text_;
	Guid guid_;
	Sha1 sha1_;
	EbxRef ref_;
	EbxStruct struct_;
	EbxArray array_;
};

class EbxInstance {
public:
	EbxInstance() = default;
	EbxInstance(const EbxPartition *p_partition, uint32_t p_index) :
			partition_(p_partition), index_(p_index) {}

	const EbxPartition &partition() const { return *partition_; }
	uint32_t index() const { return index_; }
	Guid guid() const;
	bool is_primary() const;
	EbxStruct data() const;
	std::string_view type_name() const;
	std::vector<std::string_view> type_chain() const { return data().type_chain(); }
	bool is_a(std::string_view p_type_name) const { return data().is_a(p_type_name); }
	std::optional<EbxValue> get(std::string_view p_field) const { return data().get(p_field); }
	std::optional<EbxValue> get_path(std::string_view p_path) const { return data().get_path(p_path); }
	EbxStruct::FieldRange fields() const;

	bool operator==(const EbxInstance &p_other) const { return partition_ == p_other.partition_ && index_ == p_other.index_; }

private:
	const EbxPartition *partition_ = nullptr;
	uint32_t index_ = 0;
};

struct EbxField {
	std::string_view name;
	EbxValue value;
};

class EbxFieldIterator {
public:
	using iterator_category = std::input_iterator_tag;
	using value_type = EbxField;
	using difference_type = std::ptrdiff_t;
	using pointer = const EbxField *;
	using reference = const EbxField &;

	EbxFieldIterator() = default; // end
	EbxFieldIterator(const EbxPartition *p_partition, uint32_t p_complex, uint32_t p_offset);

	const EbxField &operator*() const { return current_; }
	const EbxField *operator->() const { return &current_; }
	EbxFieldIterator &operator++() {
		advance();
		return *this;
	}
	bool operator==(const EbxFieldIterator &p_other) const { return done_ == p_other.done_ && (done_ || (field_ == p_other.field_ && depth_ == p_other.depth_)); }

private:
	void advance();

	static constexpr size_t kChain = 32;
	const EbxPartition *partition_ = nullptr;
	std::array<std::pair<uint32_t, uint32_t>, kChain> chain_{}; // (complex, offset), derived first
	size_t depth_ = 0;
	uint32_t field_ = 0;
	uint32_t field_end_ = 0;
	uint32_t at_ = 0;
	bool done_ = true;
	EbxField current_;
};

struct EbxStruct::FieldRange {
	EbxFieldIterator first;
	EbxFieldIterator begin() const { return first; }
	EbxFieldIterator end() const { return {}; }
};

class EbxArrayIterator {
public:
	using iterator_category = std::input_iterator_tag;
	using value_type = EbxValue;
	using difference_type = std::ptrdiff_t;
	using pointer = const EbxValue *;
	using reference = EbxValue;

	// Holds the (small, trivially copyable) array view by value: an iterator stays valid after the
	// EbxArray it came from is gone (only the partition must live).
	EbxArrayIterator() = default;
	EbxArrayIterator(const EbxArray &p_array, size_t p_index) :
			array_(p_array), index_(p_index) {}
	EbxValue operator*() const { return *array_.get(index_); }
	EbxArrayIterator &operator++() {
		++index_;
		return *this;
	}
	EbxArrayIterator operator++(int) {
		EbxArrayIterator old = *this;
		++index_;
		return old;
	}
	bool operator==(const EbxArrayIterator &p_other) const { return index_ == p_other.index_; }

private:
	EbxArray array_;
	size_t index_ = 0;
};

// One EBX partition, validated in full when parsed. Keeps its bytes; move-only. Instances, structs,
// arrays and values point at the partition, so keep it at a stable address while they are used
// (EbxDb / EbxLibrary hold partitions by unique_ptr).
class EbxPartition {
public:
	static Result<EbxPartition> parse(std::vector<uint8_t> p_bytes);

	EbxPartition(EbxPartition &&) noexcept = default;
	EbxPartition &operator=(EbxPartition &&) noexcept = default;
	EbxPartition(const EbxPartition &) = delete;
	EbxPartition &operator=(const EbxPartition &) = delete;

	const Guid &guid() const { return guid_; }
	Guid primary_guid() const { return instances_[primary_].guid; }
	EbxInstance primary() const { return EbxInstance(this, primary_); }
	std::span<const EbxImport> imports() const { return imports_; }
	size_t instance_count() const { return instances_.size(); }
	std::optional<EbxInstance> instance(size_t p_index) const;
	// Instances whose type is p_type_name or derives from it.
	std::vector<EbxInstance> instances_of(std::string_view p_type_name) const;
	std::optional<EbxInstance> instance_by_guid(const Guid &p_guid) const;
	// Follows a reference that stays inside this partition (external ones need an EbxResolver).
	std::optional<EbxInstance> resolve(const EbxRef &p_ref) const;
	size_t byte_size() const { return bytes_.size(); }
	size_t type_count() const { return complexes_.size(); }
	size_t field_descriptor_count() const { return fields_.size(); }
	size_t array_count() const { return arrays_.size(); }
	// Instances sharing a GUID with another instance (lookup by GUID then picks one of them).
	size_t duplicate_instance_guids() const;

	// --- descriptor access used by the value types ---
	struct Field {
		uint32_t name_start;
		uint32_t name_end;
		uint32_t hash;
		uint8_t code;
		uint16_t reference;
		int32_t offset;
	};
	struct Complex {
		uint32_t name_start;
		uint32_t name_end;
		uint32_t hash;
		uint32_t field_start;
		uint32_t field_end;
		uint8_t code;
		uint32_t size;
		std::optional<uint32_t> base;
	};
	struct InstanceEntry {
		Guid guid;
		uint32_t complex;
		uint32_t offset;
	};
	struct ArrayEntry {
		uint32_t start;
		uint32_t len;
		uint32_t complex;
	};

	const std::vector<Field> &field_descs() const { return fields_; }
	const std::vector<Complex> &complex_descs() const { return complexes_; }
	const std::vector<InstanceEntry> &instance_entries() const { return instances_; }
	const std::vector<ArrayEntry> &array_entries() const { return arrays_; }
	std::span<const uint8_t> bytes() const { return bytes_; }

	std::string_view name(uint32_t p_start, uint32_t p_end) const;
	std::string_view complex_name(uint32_t p_complex) const;
	std::string_view field_name(uint32_t p_field) const;
	bool derives_from(uint32_t p_complex, uint32_t p_hash, std::string_view p_type_name) const;
	// nullopt = offset outside the table; inner nullopt = the null string. O(log n) in the table's
	// string count, whatever the string's length (the parser indexes the NULs).
	std::optional<std::optional<std::string_view>> string(uint32_t p_offset) const;
	std::optional<EbxRef> reference(uint32_t p_raw) const;
	std::optional<std::string_view> enumerator(uint32_t p_complex, int32_t p_value) const;
	uint32_t stride(uint32_t p_field) const;
	std::optional<EbxValue> decode(uint32_t p_field, size_t p_at) const;

	static constexpr size_t kBaseDepth = 32;

private:
	EbxPartition() = default;
	friend class EbxValidator;

	Status link_complexes();
	Result<std::optional<uint32_t>> check_members(uint32_t p_complex) const;
	Status check_element(uint32_t p_complex, uint32_t p_member);
	Result<uint32_t> array_member(uint32_t p_complex) const;
	Result<const Complex *> expect_kind(uint32_t p_complex, uint8_t p_code) const;
	Error unsupported(uint32_t p_complex, uint32_t p_field) const;
	Error out_of_bounds(uint32_t p_complex, uint32_t p_field) const;
	Status validate() const;

	std::vector<uint8_t> bytes_;
	Guid guid_;
	uint32_t primary_ = 0;
	std::vector<EbxImport> imports_;
	size_t names_at_ = 0; // name table: bytes_[names_at_, names_at_ + names_len_)
	size_t names_len_ = 0;
	size_t strings_at_ = 0; // string table
	size_t strings_len_ = 0;
	std::vector<uint32_t> string_ends_; // offsets of the string table's NULs, ascending
	std::vector<Field> fields_;
	std::vector<Complex> complexes_;
	std::vector<InstanceEntry> instances_;
	std::vector<ArrayEntry> arrays_;
	std::vector<uint32_t> by_guid_; // instance indices sorted by GUID
};

} // namespace bf3::core

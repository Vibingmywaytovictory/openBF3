#pragma once
// Typed field reads for the gameplay table readers. A Fields object reads values out of validated
// EBX structs and records the first problem (missing field, wrong type, dangling or mistyped
// reference); reads after a failure return defaults, so a reader can run to the end and report one
// error. Partitions are validated in full when parsed (ebx.h), so only the shape of the data, not
// its bytes, can be wrong here.

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <optional>
#include <string>
#include <string_view>

namespace bf3::core::gameplay::detail {

class Fields {
public:
	explicit Fields(const EbxResolver &p_db) :
			db_(p_db) {}

	const EbxResolver &db() const { return db_; }
	bool ok() const { return error_.empty(); }
	Error error() const { return make_error(error_); }
	void fail(std::string p_message) {
		if (error_.empty()) {
			error_ = std::move(p_message);
		}
	}

	// The lowercase partition name of an instance, or "?<partition guid>" when the resolver does not
	// know it.
	std::string partition_name(const EbxPartition &p_partition) const;
	// "<partition> <Type>.<field>" for messages.
	std::string where(const EbxStruct &p_s, std::string_view p_field) const;

	std::optional<EbxValue> value(const EbxStruct &p_s, std::string_view p_field);
	float f32(const EbxStruct &p_s, std::string_view p_field);
	float f32_or(const EbxStruct &p_s, std::string_view p_field, float p_default);
	int32_t i32(const EbxStruct &p_s, std::string_view p_field);
	int32_t i32_or(const EbxStruct &p_s, std::string_view p_field, int32_t p_default);
	uint32_t u32(const EbxStruct &p_s, std::string_view p_field);
	bool flag(const EbxStruct &p_s, std::string_view p_field);
	bool flag_or(const EbxStruct &p_s, std::string_view p_field, bool p_default);
	std::string text(const EbxStruct &p_s, std::string_view p_field);
	std::string text_or(const EbxStruct &p_s, std::string_view p_field);
	// An enum field as its enumerator name (the number as text when the name is absent).
	std::string enum_text(const EbxStruct &p_s, std::string_view p_field);
	Vec3 vec3(const EbxStruct &p_s, std::string_view p_field);
	// A ValueType (inline struct) field.
	std::optional<EbxStruct> sub(const EbxStruct &p_s, std::string_view p_field);
	OptionalEbxArray array(const EbxStruct &p_s, std::string_view p_field);

	// The reference held by a field; nullopt (and a failure) when the field is missing or not a
	// reference.
	std::optional<EbxRef> ref(const EbxStruct &p_s, std::string_view p_field);
	// Follows a reference field. A null reference gives nullopt and, when p_required, a failure; a
	// reference that does not resolve or whose target is not a p_type is always a failure.
	std::optional<EbxInstance> follow(const EbxStruct &p_s, std::string_view p_field, std::string_view p_type, bool p_required);
	// Follows a reference value (an array element) read from p_from.
	std::optional<EbxInstance> follow_ref(const EbxPartition &p_from, const EbxRef &p_ref, std::string_view p_type, std::string_view p_what);
	// The instance GUID a reference names (for an internal reference, the target's GUID).
	std::optional<Guid> target_guid(const EbxPartition &p_from, const EbxRef &p_ref) const;
	// The partition name a reference points into ("" for null).
	std::string target_partition(const EbxPartition &p_from, const EbxRef &p_ref) const;

	MaterialPairRef material_pair(const EbxStruct &p_s, std::string_view p_field);

private:
	const EbxResolver &db_;
	std::string error_;
};

} // namespace bf3::core::gameplay::detail

#pragma once

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

	std::string partition_name(const EbxPartition &p_partition) const;

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

	std::string enum_text(const EbxStruct &p_s, std::string_view p_field);
	Vec3 vec3(const EbxStruct &p_s, std::string_view p_field);

	std::optional<EbxStruct> sub(const EbxStruct &p_s, std::string_view p_field);
	OptionalEbxArray array(const EbxStruct &p_s, std::string_view p_field);

	std::optional<EbxRef> ref(const EbxStruct &p_s, std::string_view p_field);

	std::optional<EbxInstance> follow(const EbxStruct &p_s, std::string_view p_field, std::string_view p_type, bool p_required);

	std::optional<EbxInstance> follow_ref(const EbxPartition &p_from, const EbxRef &p_ref, std::string_view p_type, std::string_view p_what);

	std::optional<Guid> target_guid(const EbxPartition &p_from, const EbxRef &p_ref) const;

	std::string target_partition(const EbxPartition &p_from, const EbxRef &p_ref) const;

	MaterialPairRef material_pair(const EbxStruct &p_s, std::string_view p_field);

private:
	const EbxResolver &db_;
	std::string error_;
};

}

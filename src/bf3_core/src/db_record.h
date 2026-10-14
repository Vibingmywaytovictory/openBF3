#pragma once
// Typed field access on a DbObject object. Ported from IW4L superbundle_bf3 (record.rs).

#include "bf3/core/db_object.h"
#include "format_errors.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bf3::core::detail {

class Record {
public:
	static Result<Record> make(const char *p_what, DbRef p_value) {
		if (!p_value.is_object()) {
			return layout_error(p_what, "is not a DbObject object");
		}
		return Record(p_what, p_value);
	}

	DbRef field(const char *p_field) const { return value_.get(p_field); }

	Result<std::string_view> str(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return missing_field(what_, p_field);
		}
		const std::optional<std::string_view> s = f.as_str();
		if (!s) {
			return field_type(what_, p_field);
		}
		return *s;
	}

	Result<std::optional<std::string_view>> opt_str(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return std::optional<std::string_view>();
		}
		const std::optional<std::string_view> s = f.as_str();
		if (!s) {
			return field_type(what_, p_field);
		}
		return s;
	}

	Result<std::optional<int64_t>> opt_i64(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return std::optional<int64_t>();
		}
		const std::optional<int64_t> v = f.as_i64();
		if (!v) {
			return field_type(what_, p_field);
		}
		return v;
	}

	Result<uint64_t> u64(const char *p_field) const {
		Result<std::optional<int64_t>> v = opt_i64(p_field);
		if (!v) {
			return v.error();
		}
		if (!v->has_value()) {
			return missing_field(what_, p_field);
		}
		return non_negative(p_field, **v);
	}

	Result<std::optional<uint64_t>> opt_u64(const char *p_field) const {
		Result<std::optional<int64_t>> v = opt_i64(p_field);
		if (!v) {
			return v.error();
		}
		if (!v->has_value()) {
			return std::optional<uint64_t>();
		}
		Result<uint64_t> u = non_negative(p_field, **v);
		if (!u) {
			return u.error();
		}
		return std::optional<uint64_t>(*u);
	}

	Result<std::optional<uint32_t>> opt_u32(const char *p_field) const {
		Result<std::optional<int64_t>> v = opt_i64(p_field);
		if (!v) {
			return v.error();
		}
		if (!v->has_value()) {
			return std::optional<uint32_t>();
		}
		const int64_t x = **v;
		if (x < 0 || x > 0xFFFFFFFFll) {
			return field_range(what_, p_field, x);
		}
		return std::optional<uint32_t>(static_cast<uint32_t>(x));
	}

	// A u32 hash stored either as i32 bits or as a non-negative value up to u32::MAX.
	Result<std::optional<uint32_t>> hash32(const char *p_field) const {
		Result<std::optional<int64_t>> v = opt_i64(p_field);
		if (!v) {
			return v.error();
		}
		if (!v->has_value()) {
			return std::optional<uint32_t>();
		}
		const int64_t x = **v;
		if (x >= INT32_MIN && x <= INT32_MAX) {
			return std::optional<uint32_t>(static_cast<uint32_t>(static_cast<int32_t>(x)));
		}
		if (x >= 0 && x <= 0xFFFFFFFFll) {
			return std::optional<uint32_t>(static_cast<uint32_t>(x));
		}
		return field_range(what_, p_field, x);
	}

	Result<bool> flag(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return false;
		}
		const std::optional<bool> b = f.as_bool();
		if (!b) {
			return field_type(what_, p_field);
		}
		return *b;
	}

	Result<Guid> guid(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return missing_field(what_, p_field);
		}
		const std::optional<Guid> g = f.as_guid();
		if (!g) {
			return field_type(what_, p_field);
		}
		return *g;
	}

	Result<Sha1> sha1(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return missing_field(what_, p_field);
		}
		const std::optional<Sha1> s = f.as_sha1();
		if (!s) {
			return field_type(what_, p_field);
		}
		return *s;
	}

	Result<std::optional<Sha1>> opt_sha1(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return std::optional<Sha1>();
		}
		const std::optional<Sha1> s = f.as_sha1();
		if (!s) {
			return field_type(what_, p_field);
		}
		return s;
	}

	Result<std::optional<std::span<const uint8_t>>> opt_blob(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return std::optional<std::span<const uint8_t>>();
		}
		const auto b = f.as_blob();
		if (!b) {
			return field_type(what_, p_field);
		}
		return b;
	}

	// The list node (iterate with first_child/next), or an empty ref when the field is absent.
	Result<DbRef> list(const char *p_field) const {
		const DbRef f = field(p_field);
		if (!f.valid()) {
			return DbRef();
		}
		if (!f.is_list()) {
			return field_type(what_, p_field);
		}
		return f;
	}

private:
	Record(const char *p_what, DbRef p_value) :
			what_(p_what), value_(p_value) {}

	Result<uint64_t> non_negative(const char *p_field, int64_t p_value) const {
		if (p_value < 0) {
			return field_range(what_, p_field, p_value);
		}
		return static_cast<uint64_t>(p_value);
	}

	const char *what_;
	DbRef value_;
};

} // namespace bf3::core::detail

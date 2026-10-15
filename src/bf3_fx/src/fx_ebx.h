#pragma once
// bf3_fx internal: bounded EBX field reads for the effect compiler, the atlas and the impact table
// (docs/design/m2a-vfx1.md §6.2, §10). Every instance examined, array element scanned and reference followed is
// charged to the item's WorkBudget before the work; every float is checked finite and within kMaxAbsParameter; a
// missing field or a value of the wrong kind fails the item. Errors are sticky: the first one is kept and every later
// read returns a zero value, so a reader can read a whole record and test ok() once.
//
// RecordingResolver wraps a resolver and records every partition looked up, found or absent, lowercase (the cache's
// dependency list, effect_cache.h; the weapon_blob.cpp RecordingResolver pattern).

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"
#include "bf3/fx/fx_types.h"

#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace bf3::fx::detail {

// Lowercase, '\' as '/': how names are compared and stored (partition names, texture names).
inline std::string canonical_name(std::string_view p_name) {
	std::string out = core::to_lower_ascii(p_name);
	for (char &c : out) {
		if (c == '\\') {
			c = '/';
		}
	}
	return out;
}

class RecordingResolver final : public core::EbxResolver {
public:
	RecordingResolver(const core::EbxResolver &p_inner, std::set<std::string> &r_names) :
			inner_(p_inner), names_(r_names) {}

	const core::EbxPartition *partition(const core::Guid &p_guid) const override {
		const core::EbxPartition *p = inner_.partition(p_guid);
		if (const std::optional<std::string_view> name = inner_.name_of(p_guid)) {
			names_.insert(canonical_name(*name));
		}
		return p;
	}
	const core::EbxPartition *partition_by_name(std::string_view p_name) const override {
		names_.insert(canonical_name(p_name));
		return inner_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const core::Guid &p_guid) const override {
		const std::optional<std::string_view> name = inner_.name_of(p_guid);
		if (name) {
			names_.insert(canonical_name(*name));
		}
		return name;
	}

private:
	const core::EbxResolver &inner_;
	std::set<std::string> &names_;
};

class Reader {
public:
	Reader(const core::EbxResolver &p_db, core::gameplay::WorkBudget &r_budget) :
			db_(p_db), budget_(r_budget) {}

	const core::EbxResolver &db() const { return db_; }
	bool ok() const { return error_.empty(); }
	const std::string &message() const { return error_; }
	core::Error error() const { return core::make_error(error_); }
	// The partition (or other context) error messages start with.
	void set_where(std::string p_where) { where_ = std::move(p_where); }
	const std::string &where() const { return where_; }

	void fail(const std::string &p_message) {
		if (error_.empty()) {
			error_ = where_.empty() ? p_message : where_ + ": " + p_message;
		}
	}
	bool charge(size_t p_units = 1) {
		if (!ok()) {
			return false;
		}
		const core::Status s = budget_.charge(p_units);
		if (!s.ok()) {
			fail(s.error().message);
			return false;
		}
		return true;
	}

	std::optional<core::EbxValue> field(const core::EbxStruct &p_s, std::string_view p_name) {
		if (!ok()) {
			return std::nullopt;
		}
		std::optional<core::EbxValue> v = p_s.get(p_name);
		if (!v) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": field missing");
		}
		return v;
	}
	// Optional fields (absent in some layouts): nullopt without an error.
	std::optional<core::EbxValue> maybe(const core::EbxStruct &p_s, std::string_view p_name) {
		if (!ok()) {
			return std::nullopt;
		}
		return p_s.get(p_name);
	}

	float number(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		return v ? checked_float(*v, p_s, p_name) : 0.0f;
	}
	float number_or(const core::EbxStruct &p_s, std::string_view p_name, float p_default) {
		const std::optional<core::EbxValue> v = maybe(p_s, p_name);
		return v ? checked_float(*v, p_s, p_name) : p_default;
	}
	bool flag(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return false;
		}
		const std::optional<bool> b = v->as_bool();
		if (!b) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a boolean");
			return false;
		}
		return *b;
	}
	bool flag_or(const core::EbxStruct &p_s, std::string_view p_name, bool p_default) {
		const std::optional<core::EbxValue> v = maybe(p_s, p_name);
		if (!v) {
			return p_default;
		}
		const std::optional<bool> b = v->as_bool();
		if (!b) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a boolean");
			return false;
		}
		return *b;
	}
	int64_t integer(const core::EbxStruct &p_s, std::string_view p_name, int64_t p_min, int64_t p_max) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return 0;
		}
		const std::optional<int64_t> i = v->as_i64();
		if (!i || *i < p_min || *i > p_max) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": integer out of range [" + std::to_string(p_min) + ", " +
					std::to_string(p_max) + "]");
			return 0;
		}
		return *i;
	}
	int64_t integer_or(const core::EbxStruct &p_s, std::string_view p_name, int64_t p_min, int64_t p_max, int64_t p_default) {
		if (!maybe(p_s, p_name)) {
			return p_default;
		}
		return integer(p_s, p_name, p_min, p_max);
	}
	// The enumerator's name ("" after a failure).
	std::string enum_name(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return {};
		}
		const std::optional<std::string_view> n = v->enum_name();
		if (!n) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a named enumerator");
			return {};
		}
		return std::string(*n);
	}
	std::string text(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return {};
		}
		const std::optional<std::string_view> t = v->as_str();
		if (!t) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a string");
			return {};
		}
		if (t->size() > kMaxNameLength) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": string of " + std::to_string(t->size()) + " bytes");
			return {};
		}
		if (!charge(1 + t->size() / core::gameplay::WorkBudget::kBytesPerUnit)) {
			return {};
		}
		return std::string(*t);
	}

	std::optional<core::EbxStruct> inline_struct(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return std::nullopt;
		}
		std::optional<core::EbxStruct> s = v->as_struct();
		if (!s) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a struct");
		}
		return s;
	}
	Vec2 vec2(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxStruct> s = inline_struct(p_s, p_name);
		return s ? Vec2{ number(*s, "x"), number(*s, "y") } : Vec2{};
	}
	Vec3 vec3(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxStruct> s = inline_struct(p_s, p_name);
		return s ? Vec3{ number(*s, "x"), number(*s, "y"), number(*s, "z") } : Vec3{};
	}
	Vec4 vec4(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxStruct> s = inline_struct(p_s, p_name);
		return s ? Vec4{ number(*s, "x"), number(*s, "y"), number(*s, "z"), number(*s, "w") } : Vec4{};
	}
	Transform transform(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxStruct> s = inline_struct(p_s, p_name);
		if (!s) {
			return {};
		}
		Transform t;
		t.right = vec3(*s, "right");
		t.up = vec3(*s, "up");
		t.forward = vec3(*s, "forward");
		t.trans = vec3(*s, "trans");
		return t;
	}

	std::optional<core::EbxRef> ref(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return std::nullopt;
		}
		std::optional<core::EbxRef> r = v->as_ref();
		if (!r) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a reference");
		}
		return r;
	}
	// A reference that must stay inside the partition (null allowed: nullopt, no error).
	std::optional<core::EbxInstance> follow_internal(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxRef> r = ref(p_s, p_name);
		if (!r || r->is_null()) {
			return std::nullopt;
		}
		if (r->kind != core::EbxRef::Kind::Internal) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": external reference where an internal one is required");
			return std::nullopt;
		}
		if (!charge()) {
			return std::nullopt;
		}
		std::optional<core::EbxInstance> i = p_s.partition().resolve(*r);
		if (!i) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": unresolved internal reference");
		}
		return i;
	}
	// The partition name an external reference points into (lowercase), "" for null; fails on an internal one.
	std::string external_name(const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<core::EbxRef> r = ref(p_s, p_name);
		if (!r || r->is_null()) {
			return {};
		}
		return external_name_of(p_s, *r, p_name);
	}
	std::string external_name_of(const core::EbxStruct &p_s, const core::EbxRef &p_ref, std::string_view p_what) {
		if (p_ref.kind != core::EbxRef::Kind::External) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_what) + ": internal reference where an asset is expected");
			return {};
		}
		if (!charge()) {
			return {};
		}
		const std::optional<std::string_view> name = db_.name_of(p_ref.partition);
		if (!name) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_what) + ": referenced partition " + p_ref.partition.to_string() + " not found");
			return {};
		}
		return canonical_name(*name);
	}
	core::OptionalEbxArray array(const core::EbxStruct &p_s, std::string_view p_name, size_t p_max) {
		const std::optional<core::EbxValue> v = field(p_s, p_name);
		if (!v) {
			return std::nullopt;
		}
		core::OptionalEbxArray a = v->as_array();
		if (!a) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not an array");
			return std::nullopt;
		}
		if (a->size() > p_max) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": " + std::to_string(a->size()) + " elements (limit " + std::to_string(p_max) +
					")");
			return std::nullopt;
		}
		if (!charge(1 + a->size())) {
			return std::nullopt;
		}
		return a;
	}

	float checked(float p_value, std::string_view p_what) {
		if (!std::isfinite(p_value) || std::fabs(p_value) > kMaxAbsParameter) {
			fail(std::string(p_what) + ": value " + std::to_string(p_value) + " not finite or above " + std::to_string(kMaxAbsParameter));
			return 0.0f;
		}
		return p_value;
	}

private:
	float checked_float(const core::EbxValue &p_v, const core::EbxStruct &p_s, std::string_view p_name) {
		const std::optional<float> f = p_v.as_f32();
		if (!f) {
			fail(std::string(p_s.type_name()) + "." + std::string(p_name) + ": not a float");
			return 0.0f;
		}
		return checked(*f, std::string(p_s.type_name()) + "." + std::string(p_name));
	}

	const core::EbxResolver &db_;
	core::gameplay::WorkBudget &budget_;
	std::string where_;
	std::string error_;
};

} // namespace bf3::fx::detail

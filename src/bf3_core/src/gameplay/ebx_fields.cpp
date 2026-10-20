#include "ebx_fields.h"

namespace bf3::core::gameplay::detail {

std::string Fields::partition_name(const EbxPartition &p_partition) const {
	const std::optional<std::string_view> name = db_.name_of(p_partition.guid());
	return name ? std::string(*name) : "?" + p_partition.guid().to_string();
}

std::string Fields::where(const EbxStruct &p_s, std::string_view p_field) const {
	std::string out = partition_name(p_s.partition());
	out += ' ';
	out += p_s.type_name();
	out += '.';
	out += p_field;
	return out;
}

std::optional<EbxValue> Fields::value(const EbxStruct &p_s, std::string_view p_field) {
	std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		fail(where(p_s, p_field) + ": missing");
	}
	return v;
}

float Fields::f32(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return 0;
	}
	const std::optional<float> f = v->as_f32();
	if (!f) {
		fail(where(p_s, p_field) + ": not a float");
		return 0;
	}
	return *f;
}

float Fields::f32_or(const EbxStruct &p_s, std::string_view p_field, float p_default) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_f32().value_or(p_default) : p_default;
}

int32_t Fields::i32(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return 0;
	}
	const std::optional<int32_t> i = v->as_i32();
	if (!i) {
		fail(where(p_s, p_field) + ": not a 32-bit integer");
		return 0;
	}
	return *i;
}

int32_t Fields::i32_or(const EbxStruct &p_s, std::string_view p_field, int32_t p_default) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_i32().value_or(p_default) : p_default;
}

uint32_t Fields::u32(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return 0;
	}
	const std::optional<uint32_t> u = v->as_u32();
	if (!u) {
		fail(where(p_s, p_field) + ": not an unsigned 32-bit integer");
		return 0;
	}
	return *u;
}

bool Fields::flag(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return false;
	}
	const std::optional<bool> b = v->as_bool();
	if (!b) {
		fail(where(p_s, p_field) + ": not a boolean");
		return false;
	}
	return *b;
}

bool Fields::flag_or(const EbxStruct &p_s, std::string_view p_field, bool p_default) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_bool().value_or(p_default) : p_default;
}

std::string Fields::text(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return {};
	}
	const std::optional<std::string_view> s = v->as_str();
	if (!s) {
		fail(where(p_s, p_field) + ": not a string");
		return {};
	}
	return std::string(*s);
}

std::string Fields::text_or(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	const std::optional<std::string_view> s = v ? v->as_str() : std::nullopt;
	return s ? std::string(*s) : std::string();
}

std::string Fields::enum_text(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return {};
	}
	if (const std::optional<std::string_view> name = v->enum_name()) {
		return std::string(*name);
	}
	if (const std::optional<int64_t> n = v->as_i64()) {
		return std::to_string(*n);
	}
	fail(where(p_s, p_field) + ": not an enum");
	return {};
}

Vec3 Fields::vec3(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return {};
	}
	const std::optional<std::array<float, 3>> xyz = v->as_vec3();
	if (!xyz) {
		fail(where(p_s, p_field) + ": not a Vec3");
		return {};
	}
	return *xyz;
}

std::optional<EbxStruct> Fields::sub(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return std::nullopt;
	}
	std::optional<EbxStruct> s = v->as_struct();
	if (!s) {
		fail(where(p_s, p_field) + ": not a struct");
	}
	return s;
}

OptionalEbxArray Fields::array(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return {};
	}
	OptionalEbxArray a = v->as_array();
	if (!a) {
		fail(where(p_s, p_field) + ": not an array");
	}
	return a;
}

std::optional<EbxRef> Fields::ref(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = value(p_s, p_field);
	if (!v) {
		return std::nullopt;
	}
	std::optional<EbxRef> r = v->as_ref();
	if (!r) {
		fail(where(p_s, p_field) + ": not a reference");
	}
	return r;
}

std::optional<EbxInstance> Fields::follow(const EbxStruct &p_s, std::string_view p_field, std::string_view p_type, bool p_required) {
	const std::optional<EbxRef> r = ref(p_s, p_field);
	if (!r) {
		return std::nullopt;
	}
	if (r->is_null()) {
		if (p_required) {
			fail(where(p_s, p_field) + ": null reference");
		}
		return std::nullopt;
	}
	return follow_ref(p_s.partition(), *r, p_type, where(p_s, p_field));
}

std::optional<EbxInstance> Fields::follow_ref(const EbxPartition &p_from, const EbxRef &p_ref, std::string_view p_type, std::string_view p_what) {
	if (p_ref.is_null()) {
		return std::nullopt;
	}
	std::optional<EbxInstance> target = db_.resolve(p_from, p_ref);
	if (!target) {
		std::string m(p_what);
		m += ": reference does not resolve";
		if (p_ref.kind == EbxRef::Kind::External) {
			m += " (" + p_ref.partition.to_string() + "/" + p_ref.instance.to_string() + ")";
		}
		fail(std::move(m));
		return std::nullopt;
	}
	if (!p_type.empty() && !target->is_a(p_type)) {
		fail(std::string(p_what) + ": points at a " + std::string(target->type_name()) + ", not a " + std::string(p_type));
		return std::nullopt;
	}
	return target;
}

std::optional<Guid> Fields::target_guid(const EbxPartition &p_from, const EbxRef &p_ref) const {
	switch (p_ref.kind) {
		case EbxRef::Kind::External:
			return p_ref.instance;
		case EbxRef::Kind::Internal:
			if (const std::optional<EbxInstance> i = p_from.instance(p_ref.index)) {
				return i->guid();
			}
			return std::nullopt;
		default:
			return std::nullopt;
	}
}

std::string Fields::target_partition(const EbxPartition &p_from, const EbxRef &p_ref) const {
	switch (p_ref.kind) {
		case EbxRef::Kind::External: {
			const std::optional<std::string_view> name = db_.name_of(p_ref.partition);
			return name ? std::string(*name) : "?" + p_ref.partition.to_string();
		}
		case EbxRef::Kind::Internal:
			return partition_name(p_from);
		default:
			return {};
	}
}

MaterialPairRef Fields::material_pair(const EbxStruct &p_s, std::string_view p_field) {
	MaterialPairRef out;
	const std::optional<EbxRef> r = ref(p_s, p_field);
	if (!r || r->is_null()) {
		return out;
	}
	out.present = true;
	if (r->kind == EbxRef::Kind::External) {
		out.partition = r->partition;
		out.instance = r->instance;
	} else {
		out.partition = p_s.partition().guid();
		out.instance = target_guid(p_s.partition(), *r).value_or(Guid{});
	}

	const std::optional<EbxInstance> pair = db_.resolve(p_s.partition(), *r);
	if (!pair) {
		return out;
	}
	if (!pair->is_a("MaterialContainerPair")) {
		fail(where(p_s, p_field) + ": points at a " + std::string(pair->type_name()) + ", not a MaterialContainerPair");
		return out;
	}
	const EbxStruct d = pair->data();
	out.flags_and_index = u32(d, "FlagsAndIndex");
	out.physics_material_index = i32(d, "PhysicsMaterialIndex");
	out.physics_property_index = i32(d, "PhysicsPropertyIndex");
	out.resolved = ok();
	return out;
}

}

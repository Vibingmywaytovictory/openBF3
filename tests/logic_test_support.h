#pragma once

#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace logic_test {

using namespace bf3::core;

inline void put32(std::vector<uint8_t> &r_out, size_t p_at, uint32_t p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 4);
}

inline uint32_t fbits(float p_value) {
	uint32_t b = 0;
	std::memcpy(&b, &p_value, 4);
	return b;
}

struct SynField {
	std::string name;
	char kind = 'i';
	std::string elem;
};

struct SynType {
	std::string name;
	std::vector<SynField> fields;
	bool value_type = false;
	std::string base;
	bool overlap = false;
	bool enum_type = false;
};

using SynValues = std::vector<std::pair<std::string, uint32_t>>;

struct SynArray {
	std::string elem;
	std::vector<uint32_t> refs;
	std::vector<SynValues> structs;

	static constexpr const char *kStrings = "*CString";
};

struct SynInstance {
	std::string type;
	SynValues values;
};

struct SynPartition {
	std::vector<SynType> types;
	std::vector<SynInstance> instances;
	std::vector<SynArray> arrays;
	std::vector<std::string> strings;
	uint32_t imports = 0;
	uint32_t seed = 1;

	static uint32_t ref(uint32_t p_instance) { return p_instance + 1; }
	static uint32_t import_ref(uint32_t p_import) { return 0x80000000u | p_import; }
	uint32_t add(SynInstance p_instance) {
		instances.push_back(std::move(p_instance));
		return static_cast<uint32_t>(instances.size() - 1);
	}
	uint32_t add_array(SynArray p_array) {
		arrays.push_back(std::move(p_array));
		return static_cast<uint32_t>(arrays.size() - 1);
	}
	uint32_t add_string(std::string p_text) {
		strings.push_back(std::move(p_text));
		return static_cast<uint32_t>(strings.size() - 1);
	}

	uint32_t add_refs(size_t p_count, uint32_t p_word) { return add_array(SynArray{ {}, std::vector<uint32_t>(p_count, p_word), {} }); }

	Guid guid(uint32_t p_index) const {
		Guid g{};
		std::memcpy(g.bytes.data(), &p_index, 4);
		std::memcpy(g.bytes.data() + 4, &seed, 4);
		g.bytes[15] = 0x5b;
		return g;
	}

	std::vector<uint8_t> build() const;
};

inline std::vector<uint8_t> SynPartition::build() const {
	std::vector<std::string> names{ "DataContainer", "$", "array", "member" };
	const auto add_name = [&](const std::string &p_name) {
		if (std::find(names.begin(), names.end(), p_name) == names.end()) {
			names.push_back(p_name);
		}
	};

	std::vector<std::string> elems{ "" };
	for (const SynType &t : types) {
		add_name(t.name);
		for (const SynField &f : t.fields) {
			add_name(f.name);
			const std::string elem = f.kind == 'w' ? std::string(SynArray::kStrings) : f.elem;
			if ((f.kind == 'v' || f.kind == 'w') && std::find(elems.begin(), elems.end(), elem) == elems.end()) {
				elems.push_back(elem);
			}
		}
	}
	std::string name_table;
	for (const std::string &n : names) {
		name_table += n;
		name_table += '\0';
	}
	const auto type_at = [&](const std::string &p_name) -> uint32_t {
		for (uint32_t i = 0; i < types.size(); ++i) {
			if (types[i].name == p_name) {
				return i;
			}
		}
		return 0;
	};
	const auto array_complex = [&](const std::string &p_elem) {
		return static_cast<uint16_t>(1 + types.size() + (std::find(elems.begin(), elems.end(), p_elem) - elems.begin()));
	};
	struct F {
		std::string name;
		uint8_t code;
		uint16_t ref;
		uint32_t offset;
	};
	struct C {
		std::string name;
		uint32_t field_start;
		uint8_t count;
		uint8_t code;
		uint16_t size;
	};
	std::vector<F> fields;
	std::vector<C> complexes{ { "DataContainer", 0, 0, ebx_code::kClass, 8 } };

	std::vector<uint32_t> sizes;
	for (const SynType &t : types) {
		const uint32_t base = t.value_type ? 0 : (t.base.empty() ? 8 : sizes[type_at(t.base)]);
		sizes.push_back((t.overlap || t.enum_type) ? 4u : base + static_cast<uint32_t>(4 * t.fields.size()));
	}
	const auto base_of = [&](const SynType &p_type) -> uint32_t {
		return (p_type.value_type || p_type.overlap) ? 0 : (p_type.base.empty() ? 8 : sizes[type_at(p_type.base)]);
	};
	for (const SynType &t : types) {
		const uint32_t start = static_cast<uint32_t>(fields.size());
		if (t.enum_type) {
			for (size_t k = 0; k < t.fields.size(); ++k) {
				fields.push_back({ t.fields[k].name, ebx_code::kVoid, 0, static_cast<uint32_t>(k + 1) });
			}
			complexes.push_back({ t.name, start, static_cast<uint8_t>(fields.size() - start), ebx_code::kEnum, 4 });
			continue;
		}
		if (!t.value_type || !t.base.empty()) {
			fields.push_back({ "$", ebx_code::kVoid, static_cast<uint16_t>(t.base.empty() ? 0 : 1 + type_at(t.base)), 0 });
		}
		for (size_t k = 0; k < t.fields.size(); ++k) {
			const SynField &f = t.fields[k];
			uint8_t code = ebx_code::kInt32;
			uint16_t ref = 0;
			switch (f.kind) {
				case 'b':
					code = ebx_code::kBoolean;
					break;
				case 'f':
					code = ebx_code::kFloat32;
					break;
				case 'r':
					code = ebx_code::kClass;
					break;
				case 's':
					code = ebx_code::kCString;
					break;
				case 'g':
					code = ebx_code::kGuid;
					break;
				case 'S':
					code = ebx_code::kValueType;
					ref = static_cast<uint16_t>(1 + type_at(f.elem));
					break;
				case 'e':
					code = ebx_code::kEnum;
					ref = static_cast<uint16_t>(1 + type_at(f.elem));
					break;
				case 'a':
				case 'v':
				case 'w':
					code = ebx_code::kArray;
					ref = array_complex(f.kind == 'a' ? std::string() : f.kind == 'w' ? std::string(SynArray::kStrings) : f.elem);
					break;
				default:
					break;
			}
			fields.push_back({ f.name, code, ref, t.overlap ? 0u : base_of(t) + static_cast<uint32_t>(4 * k) });
		}
		complexes.push_back({ t.name, start, static_cast<uint8_t>(fields.size() - start), t.value_type ? ebx_code::kValueType : ebx_code::kClass,
				static_cast<uint16_t>(t.overlap ? 4 : base_of(t) + 4 * t.fields.size()) });
	}
	for (const std::string &e : elems) {
		const uint32_t start = static_cast<uint32_t>(fields.size());
		const uint8_t code = e.empty() ? ebx_code::kClass : e == SynArray::kStrings ? ebx_code::kCString : ebx_code::kValueType;
		fields.push_back({ "member", code, static_cast<uint16_t>(code == ebx_code::kValueType ? 1 + type_at(e) : 0), 0 });
		complexes.push_back({ "array", start, 1, ebx_code::kArray, 4 });
	}

	std::vector<SynArray> all = arrays;
	const size_t empty_at = all.size();
	for (const std::string &e : elems) {
		all.push_back(SynArray{ e, {}, {} });
	}
	std::string string_table;
	std::vector<uint32_t> string_at;
	for (const std::string &s : strings) {
		string_at.push_back(static_cast<uint32_t>(string_table.size()));
		string_table += s;
		string_table += '\0';
	}

	const auto align16 = [](size_t p_at) { return (p_at + 15) & ~size_t(15); };
	const size_t names_at = 80 + size_t(imports) * 32;
	const size_t fields_at = names_at + name_table.size();
	const size_t complexes_at = align16(fields_at + fields.size() * 16);
	const size_t instance_repeaters_at = complexes_at + complexes.size() * 16;
	const size_t array_repeaters_at = align16(instance_repeaters_at + instances.size() * 12);
	const size_t meta_size = align16(array_repeaters_at + all.size() * 12);
	const size_t strings_at = meta_size;
	const size_t instances_at = strings_at + string_table.size();
	size_t instance_payload = 0;
	for (const SynInstance &i : instances) {
		instance_payload += 16 + complexes[1 + type_at(i.type)].size;
	}
	const size_t arrays_at = instances_at + instance_payload;
	const auto by_refs = [](const SynArray &p_array) { return p_array.elem.empty() || p_array.elem == SynArray::kStrings; };
	const auto stride = [&](const SynArray &p_array) -> size_t { return by_refs(p_array) ? 4 : complexes[1 + type_at(p_array.elem)].size; };
	size_t array_bytes = 0;
	for (const SynArray &a : all) {
		array_bytes += 4 + (by_refs(a) ? a.refs.size() : a.structs.size()) * stride(a);
	}
	const size_t total = arrays_at + array_bytes;

	std::vector<uint8_t> b(total, 0);
	put32(b, 0x00, 0x0FB2D1CE);
	put32(b, 0x04, static_cast<uint32_t>(meta_size));
	put32(b, 0x08, static_cast<uint32_t>(total - meta_size));
	put32(b, 0x0C, imports);
	put32(b, 0x14, static_cast<uint32_t>(instances.size()));
	put32(b, 0x18, static_cast<uint32_t>(complexes.size()));
	put32(b, 0x1C, static_cast<uint32_t>(fields.size()));
	put32(b, 0x20, static_cast<uint32_t>(name_table.size()));
	put32(b, 0x24, static_cast<uint32_t>(string_table.size()));
	put32(b, 0x28, static_cast<uint32_t>(all.size()));
	put32(b, 0x2C, static_cast<uint32_t>(instance_payload));
	const Guid partition = guid(0xFFFFFFFFu);
	const Guid primary = guid(0);
	std::memcpy(b.data() + 0x30, partition.bytes.data(), 16);
	std::memcpy(b.data() + 0x40, primary.bytes.data(), 16);
	for (uint32_t i = 0; i < imports; ++i) {
		Guid missing = guid(i);
		missing.bytes[14] = 0x77;
		std::memcpy(b.data() + 80 + i * 32, missing.bytes.data(), 16);
		std::memcpy(b.data() + 80 + i * 32 + 16, missing.bytes.data(), 16);
	}
	std::memcpy(b.data() + names_at, name_table.data(), name_table.size());
	for (size_t i = 0; i < fields.size(); ++i) {
		const size_t at = fields_at + i * 16;
		put32(b, at, ebx_name_hash(fields[i].name));
		const uint16_t flags = static_cast<uint16_t>(fields[i].code << 4);
		std::memcpy(b.data() + at + 4, &flags, 2);
		std::memcpy(b.data() + at + 6, &fields[i].ref, 2);
		put32(b, at + 8, fields[i].offset);
	}
	for (size_t i = 0; i < complexes.size(); ++i) {
		const size_t at = complexes_at + i * 16;
		put32(b, at, ebx_name_hash(complexes[i].name));
		put32(b, at + 4, complexes[i].field_start);
		b[at + 8] = complexes[i].count;
		b[at + 9] = 4;
		const uint16_t flags = static_cast<uint16_t>(complexes[i].code << 4);
		std::memcpy(b.data() + at + 10, &flags, 2);
		std::memcpy(b.data() + at + 12, &complexes[i].size, 2);
	}
	std::memcpy(b.data() + strings_at, string_table.data(), string_table.size());

	std::function<void(const SynType &, const SynValues &, size_t)> write = [&](const SynType &p_type, const SynValues &p_values, size_t p_at) {
		if (p_type.overlap) {
			return;
		}
		if (!p_type.value_type && !p_type.base.empty()) {
			write(types[type_at(p_type.base)], p_values, p_at);
		}
		for (size_t k = 0; k < p_type.fields.size(); ++k) {
			const SynField &f = p_type.fields[k];
			const size_t at = p_at + base_of(p_type) + 4 * k;
			std::optional<uint32_t> word;
			for (const auto &[name, value] : p_values) {
				if (name == f.name) {
					word = value;
				}
			}
			if (f.kind == 's') {
				put32(b, at, word ? string_at[*word] : 0xFFFFFFFFu);
			} else if (f.kind == 'a' || f.kind == 'v' || f.kind == 'w') {
				const std::string elem = f.kind == 'a' ? std::string() : f.kind == 'w' ? std::string(SynArray::kStrings) : f.elem;
				put32(b, at, word ? *word : static_cast<uint32_t>(empty_at + (std::find(elems.begin(), elems.end(), elem) - elems.begin())));
			} else if (f.kind == 'b') {
				b[at] = static_cast<uint8_t>(word.value_or(0) != 0);
			} else {
				put32(b, at, word.value_or(0));
			}
		}
	};
	size_t at = instances_at;
	for (size_t i = 0; i < instances.size(); ++i) {
		const uint32_t type = type_at(instances[i].type);
		put32(b, instance_repeaters_at + i * 12 + 4, 1);
		put32(b, instance_repeaters_at + i * 12 + 8, 1 + type);
		const Guid g = guid(static_cast<uint32_t>(i));
		std::memcpy(b.data() + at, g.bytes.data(), 16);
		write(types[type], instances[i].values, at + 16);
		at += 16 + complexes[1 + type].size;
	}
	size_t array_at = 0;
	for (size_t i = 0; i < all.size(); ++i) {
		const SynArray &a = all[i];
		const size_t count = by_refs(a) ? a.refs.size() : a.structs.size();
		const size_t rep = array_repeaters_at + i * 12;
		put32(b, rep, static_cast<uint32_t>(array_at + 4));
		put32(b, rep + 4, static_cast<uint32_t>(count));
		put32(b, rep + 8, array_complex(a.elem));
		put32(b, arrays_at + array_at, static_cast<uint32_t>(count));
		for (size_t k = 0; k < count; ++k) {
			const size_t element_at = arrays_at + array_at + 4 + k * stride(a);
			if (a.elem.empty()) {
				put32(b, element_at, a.refs[k]);
			} else if (a.elem == SynArray::kStrings) {
				put32(b, element_at, string_at[a.refs[k]]);
			} else {
				write(types[type_at(a.elem)], a.structs[k], element_at);
			}
		}
		array_at += 4 + count * stride(a);
	}
	return b;
}

inline const EbxPartition *insert_syn(EbxDb &r_db, const char *p_name, const SynPartition &p_syn) {
	Result<bool> inserted = r_db.parse_insert(p_name, p_syn.build());
	CHECK_MESSAGE(inserted.ok(), (inserted.ok() ? std::string() : inserted.error().message));
	const EbxPartition *p = r_db.partition_by_name(p_name);
	CHECK(p != nullptr);
	return p;
}

}

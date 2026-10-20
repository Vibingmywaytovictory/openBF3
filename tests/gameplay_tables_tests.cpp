#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;
using doctest::Approx;

namespace {

struct Mounted {
	InstallReader reader;
	std::shared_ptr<Install> install;
};

bool open_or_skip(Mounted &r_m) {
	if (!r_m.reader.locate({})) {
		bf3::test::skip_install_case(r_m.reader.describe().c_str());
		return false;
	}
	Result<std::shared_ptr<Install>> install = Install::open(r_m.reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	r_m.install = *install;
	return true;
}

std::unique_ptr<EbxLibrary> open_level(const Mounted &p_m, const char *p_level_id) {
	const std::optional<Level> level = p_m.install->level(p_level_id);
	REQUIRE_MESSAGE(level.has_value(), p_level_id);
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_m.install, { level->superbundle });
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	return std::move(*lib);
}

ModeDef mode(const EbxResolver &p_db, const char *p_level, const char *p_mode) {
	Result<ModeDef> m = read_mode_def(p_db, p_level, p_mode);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	return std::move(*m);
}

VehicleDef vehicle(const EbxResolver &p_db, const char *p_blueprint) {
	Result<VehicleDef> v = read_vehicle_def(p_db, p_blueprint);
	REQUIRE_MESSAGE(v.ok(), (v.ok() ? std::string() : v.error().message));
	return std::move(*v);
}

size_t hq_count(const ModeDef &p_mode) {
	size_t n = 0;
	for (const CapturePointDef &c : p_mode.capture_points) {
		n += c.is_hq() ? 1 : 0;
	}
	return n;
}

const VehicleTurretDef *turret(const VehicleDef &p_v, int p_seat, int p_axis, bool p_root) {
	for (const VehicleTurretDef &t : p_v.turrets) {
		if (t.seat == p_seat && t.rotation_axis == p_axis && (t.parent < 0) == p_root) {
			return &t;
		}
	}
	return nullptr;
}

void check_mbt_health(const VehicleDef &p_v) {
	CHECK(p_v.body_mass == Approx(5000));
	CHECK(p_v.gravity_modifier == Approx(1.6));
	CHECK(p_v.max_zone_health() == Approx(1000));
	CHECK(p_v.front.min_damage_angle == Approx(81));
	CHECK(p_v.front.damage_angle_multiplier == Approx(1));
	CHECK(p_v.rear.min_damage_angle == Approx(36));
	CHECK(p_v.rear.damage_angle_multiplier == Approx(2.75));
	CHECK(p_v.left.min_damage_angle == Approx(45));
	CHECK(p_v.left.damage_angle_multiplier == Approx(1.8));
	CHECK(p_v.right.min_damage_angle == Approx(45));
	CHECK(p_v.right.damage_angle_multiplier == Approx(1.8));
	CHECK(p_v.rear.max_shield_health == Approx(175));
	CHECK(p_v.left.max_shield_health == Approx(175));
	CHECK(p_v.disabled_damage_threshold == Approx(500));
	CHECK(p_v.emergency_repair_health == Approx(510));
	CHECK(p_v.regeneration_rate == Approx(30));
	CHECK(p_v.regeneration_delay == Approx(12));
	REQUIRE(p_v.seats.size() == 2);
	CHECK(p_v.seats[0].seat_type == "EST_Driver");
	CHECK(p_v.seats[1].seat_type == "EST_Gunner");
}

struct SynthNode {
	bool part = true;
	std::vector<uint32_t> objects;
	uint32_t target = 0;
};

void put32(std::vector<uint8_t> &r_out, size_t p_at, uint32_t p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 4);
}

Guid synth_guid(uint32_t p_n) {
	Guid g{};
	std::memcpy(g.bytes.data(), &p_n, 4);
	g.bytes[15] = 0x5a;
	return g;
}

std::vector<uint8_t> build_world_graph(const std::vector<SynthNode> &p_nodes) {
	const char *names[] = { "DataContainer", "$", "WorldPartData", "Objects", "WorldPartReferenceObjectData", "Blueprint", "array", "member" };
	std::string name_table;
	for (const char *n : names) {
		name_table += n;
		name_table += '\0';
	}
	struct F {
		const char *name;
		uint8_t code;
		uint16_t ref;
		uint32_t offset;
	};
	const F fields[] = {
		{ "$", ebx_code::kVoid, 0, 0 },
		{ "Objects", ebx_code::kArray, 3, 8 },
		{ "$", ebx_code::kVoid, 0, 0 },
		{ "Blueprint", ebx_code::kClass, 0, 8 },
		{ "member", ebx_code::kClass, 0, 0 },
	};
	struct C {
		const char *name;
		uint32_t field_start;
		uint8_t count;
		uint8_t code;
		uint16_t size;
	};
	const C complexes[] = {
		{ "DataContainer", 0, 0, ebx_code::kClass, 8 },
		{ "WorldPartData", 0, 2, ebx_code::kClass, 16 },
		{ "WorldPartReferenceObjectData", 2, 2, ebx_code::kClass, 16 },
		{ "array", 4, 1, ebx_code::kArray, 4 },
	};
	const auto align16 = [](size_t p_at) { return (p_at + 15) & ~size_t(15); };
	size_t part_count = 0, array_bytes = 0;
	for (const SynthNode &n : p_nodes) {
		if (n.part) {
			++part_count;
			array_bytes += 4 + n.objects.size() * 4;
		}
	}
	const size_t names_at = 80;
	const size_t fields_at = names_at + name_table.size();
	const size_t complexes_at = align16(fields_at + std::size(fields) * 16);
	const size_t instance_repeaters_at = complexes_at + std::size(complexes) * 16;
	const size_t array_repeaters_at = align16(instance_repeaters_at + p_nodes.size() * 12);
	const size_t meta_size = align16(array_repeaters_at + part_count * 12);
	const size_t instances_at = meta_size;
	const size_t instance_payload = p_nodes.size() * (16 + 16);
	const size_t arrays_at = instances_at + instance_payload;
	const size_t total = arrays_at + array_bytes;

	std::vector<uint8_t> b(total, 0);
	put32(b, 0x00, 0x0FB2D1CE);
	put32(b, 0x04, static_cast<uint32_t>(meta_size));
	put32(b, 0x08, static_cast<uint32_t>(total - meta_size));
	put32(b, 0x14, static_cast<uint32_t>(p_nodes.size()));
	put32(b, 0x18, static_cast<uint32_t>(std::size(complexes)));
	put32(b, 0x1C, static_cast<uint32_t>(std::size(fields)));
	put32(b, 0x20, static_cast<uint32_t>(name_table.size()));
	put32(b, 0x28, static_cast<uint32_t>(part_count));
	put32(b, 0x2C, static_cast<uint32_t>(instance_payload));
	const Guid partition = synth_guid(0xFFFFFFFFu);
	const Guid primary = synth_guid(0);
	std::memcpy(b.data() + 0x30, partition.bytes.data(), 16);
	std::memcpy(b.data() + 0x40, primary.bytes.data(), 16);
	std::memcpy(b.data() + names_at, name_table.data(), name_table.size());
	for (size_t i = 0; i < std::size(fields); ++i) {
		const size_t at = fields_at + i * 16;
		put32(b, at, ebx_name_hash(fields[i].name));
		const uint16_t flags = static_cast<uint16_t>(fields[i].code << 4);
		std::memcpy(b.data() + at + 4, &flags, 2);
		std::memcpy(b.data() + at + 6, &fields[i].ref, 2);
		put32(b, at + 8, fields[i].offset);
	}
	for (size_t i = 0; i < std::size(complexes); ++i) {
		const size_t at = complexes_at + i * 16;
		put32(b, at, ebx_name_hash(complexes[i].name));
		put32(b, at + 4, complexes[i].field_start);
		b[at + 8] = complexes[i].count;
		b[at + 9] = 4;
		const uint16_t flags = static_cast<uint16_t>(complexes[i].code << 4);
		std::memcpy(b.data() + at + 10, &flags, 2);
		std::memcpy(b.data() + at + 12, &complexes[i].size, 2);
	}
	size_t array_index = 0, array_at = 0;
	for (size_t i = 0; i < p_nodes.size(); ++i) {
		const SynthNode &n = p_nodes[i];

		put32(b, instance_repeaters_at + i * 12 + 4, 1);
		put32(b, instance_repeaters_at + i * 12 + 8, n.part ? 1 : 2);
		const size_t at = instances_at + i * 32;
		const Guid g = synth_guid(static_cast<uint32_t>(i));
		std::memcpy(b.data() + at, g.bytes.data(), 16);
		const size_t data = at + 16;
		if (!n.part) {
			put32(b, data + 8, n.target + 1);
			continue;
		}
		put32(b, data + 8, static_cast<uint32_t>(array_index));
		const size_t rep = array_repeaters_at + array_index * 12;
		put32(b, rep, static_cast<uint32_t>(array_at + 4));
		put32(b, rep + 4, static_cast<uint32_t>(n.objects.size()));
		put32(b, rep + 8, 3);
		put32(b, arrays_at + array_at, static_cast<uint32_t>(n.objects.size()));
		for (size_t k = 0; k < n.objects.size(); ++k) {
			put32(b, arrays_at + array_at + 4 + k * 4, n.objects[k] + 1);
		}
		array_at += 4 + n.objects.size() * 4;
		++array_index;
	}
	return b;
}

std::vector<SynthNode> fan_chain(uint32_t p_parts, uint32_t p_fan) {
	std::vector<SynthNode> nodes(p_parts);
	for (uint32_t i = 0; i + 1 < p_parts; ++i) {
		for (uint32_t k = 0; k < p_fan; ++k) {
			SynthNode r;
			r.part = false;
			r.target = i + 1;
			nodes[i].objects.push_back(static_cast<uint32_t>(nodes.size()));
			nodes.push_back(r);
		}
	}
	return nodes;
}

const EbxPartition &insert_graph(EbxDb &r_db, const std::vector<SynthNode> &p_nodes) {
	Result<bool> inserted = r_db.parse_insert("levels/synthetic/graph", build_world_graph(p_nodes));
	REQUIRE_MESSAGE(inserted.ok(), (inserted.ok() ? std::string() : inserted.error().message));
	const EbxPartition *p = r_db.partition_by_name("levels/synthetic/graph");
	REQUIRE(p != nullptr);
	return *p;
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

std::vector<uint8_t> SynPartition::build() const {
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

const EbxPartition *insert_syn(EbxDb &r_db, const char *p_name, const SynPartition &p_syn) {
	Result<bool> inserted = r_db.parse_insert(p_name, p_syn.build());
	CHECK_MESSAGE(inserted.ok(), (inserted.ok() ? std::string() : inserted.error().message));
	const EbxPartition *p = r_db.partition_by_name(p_name);
	CHECK(p != nullptr);
	return p;
}

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

constexpr double kFastSeconds = 20.0;

bool budget_error(const Error &p_error) {
	return p_error.message.find("work budget of") != std::string::npos;
}

std::vector<SynType> world_types() {
	return {
		{ "WorldPartData", { { "Objects", 'a' } } },
		{ "WorldPartReferenceObjectData", { { "Blueprint", 'r' }, { "Excluded", 'b' } } },
		{ "SubWorldReferenceObjectData", { { "AutoLoad", 'b' }, { "InclusionSettings", 'r' }, { "BundleName", 's' }, { "Excluded", 'b' } } },
		{ "SubWorldInclusionSettings", { { "Settings", 'a' } } },
		{ "ReferenceObjectData", { { "Blueprint", 'r' }, { "Excluded", 'b' } } },
		{ "PropertyConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceFieldId", 'i' }, { "TargetFieldId", 'i' } }, true },
		{ "PrefabBlueprint", { { "Objects", 'a' }, { "PropertyConnections", 'v', "PropertyConnection" }, { "Descriptor", 'r' } } },
		{ "LevelData", { { "Objects", 'a' } } },
		{ "SubWorldData", { { "Objects", 'a' } } },
		{ "TeamEntityData", { { "Team", 'r' } } },
		{ "TeamData", { { "SoldierCustomization", 'a' }, { "VehicleCustomization", 'a' } } },
	};
}

std::string add_wide_value_type(SynPartition &r_syn, int p_levels) {
	std::string base;
	for (int level = 0; level < p_levels; ++level) {
		SynType t{ "W" + std::to_string(level), {}, true, base, true };
		for (int k = 0; k < (base.empty() ? 255 : 254); ++k) {
			t.fields.push_back({ "Pad" + std::to_string(level) + "_" + std::to_string(k), 'i', {} });
		}
		r_syn.types.push_back(t);
		base = t.name;
	}
	return base;
}

SynPartition repeated_part(size_t p_k, size_t p_n, bool p_unresolvable) {
	SynPartition s;
	s.types = world_types();
	s.imports = p_unresolvable ? 1 : 0;
	const uint32_t root = s.add({ "WorldPartData", {} });
	const uint32_t reference = s.add({ "WorldPartReferenceObjectData", {} });
	const uint32_t part = s.add({ "WorldPartData", { { "Objects", s.add_refs(p_n, p_unresolvable ? SynPartition::import_ref(0) : 0) } } });
	s.instances[root].values = { { "Objects", s.add_refs(p_k, SynPartition::ref(reference)) } };
	s.instances[reference].values = { { "Blueprint", SynPartition::ref(part) } };
	return s;
}

}

TEST_SUITE("tables-unit") {
	TEST_CASE("gameplay: LogicWorld skips world parts that contain themselves") {

		std::vector<SynthNode> nodes(3);
		for (const uint32_t target : { 0u, 0u, 2u }) {
			SynthNode r;
			r.part = false;
			r.target = target;
			nodes[target == 2 ? 1 : 0].objects.push_back(static_cast<uint32_t>(nodes.size()));
			nodes.push_back(r);
		}
		{
			SynthNode back;
			back.part = false;
			back.target = 1;
			nodes[2].objects.push_back(static_cast<uint32_t>(nodes.size()));
			nodes.push_back(back);
			SynthNode into;
			into.part = false;
			into.target = 1;
			nodes[0].objects.push_back(static_cast<uint32_t>(nodes.size()));
			nodes.push_back(into);
		}
		EbxDb db;
		const EbxPartition &p = insert_graph(db, nodes);
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), {});
		REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
		CHECK(world->stats().cycles == 3);
		CHECK(world->stats().depth_limited == 0);
		CHECK(world->stats().walk_steps < 100);
	}

	TEST_CASE("gameplay: LogicWorld walk budget stops a nested world-part fan-out") {

		EbxDb db;
		const EbxPartition &p = insert_graph(db, fan_chain(40, 2));
		LogicWorldOptions options;
		options.max_walk_steps = 100000;
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(world.error().message.find("walk exceeds 100000 steps") != std::string::npos);

		Result<LogicWorld> by_default = LogicWorld::build(db, p.primary(), {});
		CHECK_FALSE(by_default.ok());

		EbxDb small_db;
		const EbxPartition &small = insert_graph(small_db, fan_chain(6, 2));
		Result<LogicWorld> fine = LogicWorld::build(small_db, small.primary(), {});
		REQUIRE(fine.ok());
		CHECK(fine->stats().cycles == 0);
	}

	TEST_CASE("gameplay: LogicWorld walk budget counts null Objects entries") {

		std::vector<SynthNode> nodes(1);
		for (uint32_t k = 0; k < 120000; ++k) {
			nodes[0].objects.push_back(0xFFFFFFFFu);
		}
		EbxDb db;
		const EbxPartition &p = insert_graph(db, nodes);
		Result<LogicWorld> whole = LogicWorld::build(db, p.primary(), {});
		REQUIRE(whole.ok());
		CHECK(whole->stats().walk_steps == 1 + 120000);
		LogicWorldOptions options;
		options.max_walk_steps = 100000;
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(world.error().message.find("walk exceeds 100000 steps") != std::string::npos);

		std::vector<SynthNode> fan = fan_chain(22, 2);
		for (uint32_t k = 0; k < 100000; ++k) {
			fan[21].objects.push_back(0xFFFFFFFFu);
		}
		EbxDb fan_db;
		const EbxPartition &f = insert_graph(fan_db, fan);
		Result<LogicWorld> fanned = LogicWorld::build(fan_db, f.primary(), {});
		REQUIRE_FALSE(fanned.ok());
		CHECK(fanned.error().message.find("work budget of 1000000 units exhausted") != std::string::npos);
	}

	TEST_CASE("gameplay: LogicWorld walk fails when the query budget runs out") {

		EbxDb db;
		const EbxPartition &p = insert_graph(db, fan_chain(4, 2));
		LogicWorldOptions options;
		options.max_query_steps = 1;
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(world.error().message.find("query budget") != std::string::npos);
	}

	TEST_CASE("gameplay: LogicWorld depth limit on a long world-part chain") {
		EbxDb db;
		const EbxPartition &p = insert_graph(db, fan_chain(80, 1));
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), {});
		REQUIRE(world.ok());
		CHECK(world->stats().depth_limited == 1);
	}

	TEST_CASE("gameplay: WorkBudget is sticky once exhausted") {
		WorkBudget budget(10);
		CHECK(budget.charge(4).ok());
		CHECK(budget.charge(6).ok());
		CHECK(budget.used() == 10);
		const Status over = budget.charge(1);
		REQUIRE_FALSE(over.ok());
		CHECK(budget_error(over.error()));

		CHECK_FALSE(budget.charge(0).ok());
		CHECK_FALSE(budget.take(0));
		CHECK_FALSE(budget.status().ok());
		CHECK(budget.used() == 10);
		CHECK(WorkBudget(5).charge_bytes(4 * WorkBudget::kBytesPerUnit).ok());
		CHECK_FALSE(WorkBudget(4).charge_bytes(4 * WorkBudget::kBytesPerUnit).ok());
	}

	TEST_CASE("gameplay: round 1 repro - world-part fan-out ends on the total budget") {

		EbxDb db;
		const EbxPartition &p = insert_graph(db, fan_chain(40, 2));
		LogicWorldOptions options;
		options.max_walk_steps = SIZE_MAX;
		options.max_query_steps = SIZE_MAX;
		options.max_work = 200000;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(world.error().message.find("200000") != std::string::npos);
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: round 2 repro - null Objects entries under a fan-out") {

		std::vector<SynthNode> fan = fan_chain(22, 2);
		for (uint32_t k = 0; k < 100000; ++k) {
			fan[21].objects.push_back(0xFFFFFFFFu);
		}
		EbxDb db;
		const EbxPartition &f = insert_graph(db, fan);
		LogicWorldOptions options;
		options.max_walk_steps = SIZE_MAX;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, f.primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: round 3 repro - K references to one sub-world with N null inclusion settings") {

		constexpr size_t kRefs = 100000, kNulls = 100000;
		SynPartition s;
		s.types = world_types();
		const uint32_t root = s.add({ "WorldPartData", {} });
		const uint32_t sub = s.add({ "SubWorldReferenceObjectData", {} });
		const uint32_t settings = s.add({ "SubWorldInclusionSettings", { { "Settings", s.add_refs(kNulls, 0) } } });
		s.instances[root].values = { { "Objects", s.add_refs(kRefs, SynPartition::ref(sub)) } };
		s.instances[sub].values = { { "AutoLoad", 1 }, { "InclusionSettings", SynPartition::ref(settings) } };
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/subworlds", s);
		if (p == nullptr) {
			return;
		}
		for (const bool walk_cap : { true, false }) {
			CAPTURE(walk_cap);
			LogicWorldOptions options;
			options.mode = "ConquestLarge0";
			if (!walk_cap) {
				options.max_walk_steps = SIZE_MAX;
			}
			const auto start = std::chrono::steady_clock::now();
			Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
			REQUIRE_FALSE(world.ok());
			if (!walk_cap) {
				CHECK(budget_error(world.error()));
			}
			CHECK(seconds_since(start) < kFastSeconds);
		}
	}

	TEST_CASE("gameplay: K references to one world part whose Objects never resolve") {

		for (const bool unresolvable : { false, true }) {
			CAPTURE(unresolvable);
			EbxDb db;
			const EbxPartition *p = insert_syn(db, "levels/synthetic/parts", repeated_part(50000, 100000, unresolvable));
			if (p == nullptr) {
				return;
			}
			LogicWorldOptions options;
			options.max_walk_steps = SIZE_MAX;
			const auto start = std::chrono::steady_clock::now();
			Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
			REQUIRE_FALSE(world.ok());
			CHECK(budget_error(world.error()));
			CHECK(seconds_since(start) < kFastSeconds);
		}

		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/parts", repeated_part(3, 5, true));
		if (p == nullptr) {
			return;
		}
		Result<LogicWorld> world = LogicWorld::build(db, p->primary(), {});
		REQUIRE(world.ok());
		CHECK(world->stats().unresolved_objects == 15);
		CHECK(world->budget().used() >= world->stats().walk_steps);
	}

	TEST_CASE("gameplay: K prefabs sharing one connection array of N entries") {

		constexpr size_t kPrefabs = 2000, kConnections = 100000;
		SynPartition s;
		s.types = world_types();
		const uint32_t root = s.add({ "PrefabBlueprint", {} });
		const uint32_t shared = s.add_array(SynArray{ "PropertyConnection", {}, std::vector<SynValues>(kConnections) });
		std::vector<uint32_t> references;
		for (size_t k = 0; k < kPrefabs; ++k) {
			const uint32_t prefab = s.add({ "PrefabBlueprint", { { "PropertyConnections", shared } } });
			references.push_back(SynPartition::ref(s.add({ "ReferenceObjectData", { { "Blueprint", SynPartition::ref(prefab) } } })));
		}
		s.instances[root].values = { { "Objects", s.add_array(SynArray{ {}, references, {} }) } };
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/prefabs", s);
		if (p == nullptr) {
			return;
		}
		LogicWorldOptions options;
		options.expand_root_objects = true;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: self-referencing connection loops end on the total budget") {

		constexpr uint32_t kRefs = 40;
		SynPartition s;
		s.types = world_types();
		const uint32_t root = s.add({ "PrefabBlueprint", {} });
		const uint32_t part = s.add({ "WorldPartData", {} });
		std::vector<uint32_t> references;
		const uint32_t excluded = static_cast<uint32_t>(field_id("Excluded"));
		const uint32_t source = SynPartition::ref(part);
		std::vector<SynValues> loops(2, SynValues{ { "Source", source }, { "Target", source }, { "SourceFieldId", excluded }, { "TargetFieldId", excluded } });
		for (uint32_t k = 0; k < kRefs; ++k) {
			const uint32_t r = SynPartition::ref(s.add({ "WorldPartReferenceObjectData", { { "Blueprint", SynPartition::ref(part) } } }));
			references.push_back(r);
			loops.push_back({ { "Source", source }, { "Target", r }, { "SourceFieldId", excluded }, { "TargetFieldId", excluded } });
		}
		s.instances[root].values = { { "Objects", s.add_array(SynArray{ {}, references, {} }) },
			{ "PropertyConnections", s.add_array(SynArray{ "PropertyConnection", {}, loops }) } };
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/loops", s);
		if (p == nullptr) {
			return;
		}
		LogicWorldOptions options;
		options.max_walk_steps = SIZE_MAX;
		options.max_query_steps = SIZE_MAX;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);

		Result<LogicWorld> by_default = LogicWorld::build(db, p->primary(), {});
		CHECK_FALSE(by_default.ok());

		SynPartition q;
		q.types = world_types();
		const uint32_t q_root = q.add({ "PrefabBlueprint", {} });
		const uint32_t target = SynPartition::ref(q.add({ "WorldPartData", {} }));
		const uint32_t field = static_cast<uint32_t>(field_id("Loop"));
		std::vector<SynValues> q_loops(2, SynValues{ { "Source", target }, { "Target", target }, { "SourceFieldId", field }, { "TargetFieldId", field } });
		q.instances[q_root].values = { { "PropertyConnections", q.add_array(SynArray{ "PropertyConnection", {}, q_loops }) } };
		q.seed = 2;
		const EbxPartition *qp = insert_syn(db, "levels/synthetic/queries", q);
		if (qp == nullptr) {
			return;
		}
		LogicWorldOptions query_options;
		query_options.max_query_steps = SIZE_MAX;
		query_options.max_work = 1000000;
		Result<LogicWorld> built = LogicWorld::build(db, qp->primary(), query_options);
		REQUIRE(built.ok());
		const std::optional<EbxInstance> looped = qp->instance(1);
		REQUIRE(looped.has_value());
		const auto query_start = std::chrono::steady_clock::now();
		for (int i = 0; i < 1000; ++i) {
			CHECK_FALSE(built->property(0, *looped, static_cast<int32_t>(field)).has_value());
		}
		CHECK(built->budget().exhausted());
		CHECK(built->query_budget_spent());
		CHECK(built->budget().used() <= 1000000);
		CHECK(seconds_since(query_start) < kFastSeconds);
	}

	TEST_CASE("gameplay: K team entities naming one TeamData with N null kits (read_mode_def)") {

		constexpr size_t kTeams = 10000, kKits = 100000;
		for (const bool unresolvable : { false, true }) {
			CAPTURE(unresolvable);
			SynPartition level;
			level.types = world_types();
			const uint32_t level_root = level.add({ "LevelData", {} });
			const uint32_t reference = level.add({ "SubWorldReferenceObjectData", { { "AutoLoad", 1 }, { "BundleName", level.add_string("Levels/Syn/Sub") } } });
			level.instances[level_root].values = { { "Objects", level.add_refs(1, SynPartition::ref(reference)) } };
			SynPartition sub;
			sub.seed = 2;
			sub.types = world_types();
			sub.imports = unresolvable ? 1 : 0;
			const uint32_t sub_root = sub.add({ "SubWorldData", {} });
			const uint32_t team_data = sub.add({ "TeamData", { { "SoldierCustomization", sub.add_refs(kKits, unresolvable ? SynPartition::import_ref(0) : 0) } } });
			const uint32_t team = sub.add({ "TeamEntityData", { { "Team", SynPartition::ref(team_data) } } });
			sub.instances[sub_root].values = { { "Objects", sub.add_refs(kTeams, SynPartition::ref(team)) } };
			EbxDb db;
			if (insert_syn(db, "levels/syn/syn", level) == nullptr || insert_syn(db, "levels/syn/sub", sub) == nullptr) {
				return;
			}
			const auto start = std::chrono::steady_clock::now();
			Result<ModeDef> mode = read_mode_def(db, "Syn", "CQL");
			REQUIRE_FALSE(mode.ok());
			CHECK(budget_error(mode.error()));
			CHECK(seconds_since(start) < kFastSeconds);
		}
	}

	TEST_CASE("gameplay: K components naming one component with N null children (soldier, vehicle)") {

		constexpr size_t kListed = 8192, kNulls = 200000;
		for (const bool vehicle : { false, true }) {
			CAPTURE(vehicle);
			SynPartition s;
			s.types = {
				{ vehicle ? "VehicleBlueprint" : "SoldierBlueprint", { { "Object", 'r' } } },
				{ vehicle ? "VehicleEntityData" : "SoldierEntityData", { { "Components", 'a' } } },
				{ "ComponentData", { { "Components", 'a' }, { "Excluded", 'b' } } },
			};
			const uint32_t blueprint = s.add({ s.types[0].name, {} });
			const uint32_t entity = s.add({ s.types[1].name, {} });
			const uint32_t component = s.add({ "ComponentData", { { "Components", s.add_refs(kNulls, 0) } } });
			s.instances[blueprint].values = { { "Object", SynPartition::ref(entity) } };
			s.instances[entity].values = { { "Components", s.add_refs(kListed, SynPartition::ref(component)) } };
			EbxDb db;
			const char *name = vehicle ? "vehicles/syn/syn" : "characters/soldiers/syn";
			if (insert_syn(db, name, s) == nullptr) {
				return;
			}
			const auto start = std::chrono::steady_clock::now();
			if (vehicle) {
				Result<VehicleDef> v = read_vehicle_def(db, name);
				REQUIRE_FALSE(v.ok());
				CHECK(budget_error(v.error()));
			} else {
				Result<SoldierDef> soldier = read_soldier_def(db, name);
				REQUIRE_FALSE(soldier.ok());
				CHECK(budget_error(soldier.error()));
			}
			CHECK(seconds_since(start) < kFastSeconds);
		}
	}

	TEST_CASE("gameplay: K unlock parts naming one part with N null unlocks (read_kit_def)") {
		constexpr size_t kParts = 100000, kNulls = 100000;
		SynPartition s;
		s.types = {
			{ "VeniceSoldierCustomizationAsset", { { "WeaponTable", 'r' } } },
			{ "CustomizationTable", { { "UnlockParts", 'a' } } },
			{ "CustomizationUnlockParts", { { "SelectableUnlocks", 'a' } } },
		};
		const uint32_t kit = s.add({ "VeniceSoldierCustomizationAsset", {} });
		const uint32_t table = s.add({ "CustomizationTable", {} });
		const uint32_t part = s.add({ "CustomizationUnlockParts", { { "SelectableUnlocks", s.add_refs(kNulls, 0) } } });
		s.instances[kit].values = { { "WeaponTable", SynPartition::ref(table) } };
		s.instances[table].values = { { "UnlockParts", s.add_refs(kParts, SynPartition::ref(part)) } };
		EbxDb db;
		if (insert_syn(db, "gameplay/kits/syn", s) == nullptr) {
			return;
		}
		const UnlockProgression none;
		const auto start = std::chrono::steady_clock::now();
		Result<KitDef> read = read_kit_def(db, "gameplay/kits/syn", &none);
		REQUIRE_FALSE(read.ok());
		CHECK(budget_error(read.error()));
		CHECK(seconds_since(start) < kFastSeconds);

		WorkBudget shared(1000);
		CHECK_FALSE(read_kit_def(db, "gameplay/kits/syn", &none, &shared).ok());
		CHECK(shared.exhausted());
		CHECK_FALSE(read_kit_def(db, "gameplay/kits/syn", &none, &shared).ok());
		CHECK(shared.used() <= 1000);
	}

	TEST_CASE("gameplay: wide type chains - rejected past the parser's cap, O(log n) lookups at it") {

		const auto build = [](int p_levels, size_t p_references) {
			SynPartition s;
			s.types = world_types();
			std::string base;
			for (int level = 0; level < p_levels; ++level) {
				SynType wide{ "Wide" + std::to_string(level), {}, false, base };
				for (int k = 0; k < 254; ++k) {
					wide.fields.push_back({ "Pad" + std::to_string(k), 'i', {} });
				}
				s.types.push_back(wide);
				base = wide.name;
			}

			s.types.push_back({ "WorldPartReferenceObjectData", { { "Blueprint", 'r' } }, false, base });
			s.types.erase(s.types.begin() + 1);
			const uint32_t root = s.add({ "WorldPartData", {} });
			const uint32_t part = s.add({ "WorldPartData", {} });
			const uint32_t reference = s.add({ "WorldPartReferenceObjectData", { { "Blueprint", SynPartition::ref(part) } } });
			s.instances[root].values = { { "Objects", s.add_refs(p_references, SynPartition::ref(reference)) } };
			return s;
		};
		{

			EbxDb db;
			const Result<bool> inserted = db.parse_insert("levels/synthetic/wide", build(30, 10).build());
			REQUIRE_FALSE(inserted.ok());
			CHECK(inserted.error().message.find("field descriptors along its base chain") != std::string::npos);
		}

		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/wide", build(4, 10));
		if (p == nullptr) {
			return;
		}
		const std::optional<EbxInstance> wide = p->instance(2);
		REQUIRE(wide.has_value());
		CHECK(WorkBudget::chain_descriptors(wide->data()) == 4 * 255 + 2);
		CHECK_FALSE(wide->get("Excluded").has_value());
		CHECK(wide->get("Pad253").has_value());
		Result<LogicWorld> small = LogicWorld::build(db, p->primary(), {});
		REQUIRE(small.ok());
		CHECK(small->budget().used() < 200);

		EbxDb many_db;
		const EbxPartition *many = insert_syn(many_db, "levels/synthetic/wide", build(4, 400000));
		if (many == nullptr) {
			return;
		}
		LogicWorldOptions options;
		options.max_walk_steps = SIZE_MAX;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(many_db, many->primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: inline value types and enum fields read by lookup, nested cycles included") {

		SynPartition s;
		s.types = {
			{ "Choice", { { "One", 'i' }, { "Two", 'i' }, { "Three", 'i' } }, false, {}, false, true },
			{ "V1", { { "x", 'i' }, { "next", 'S', "V2" } }, true, {}, true },
			{ "V2", { { "y", 'i' }, { "next", 'S', "V1" } }, true, {}, true },
			{ "V3", { { "z", 'i' }, { "w", 'i' } }, true },
			{ "Holder", { { "Inline", 'S', "V3" }, { "Pick", 'e', "Choice" } } },
		};
		const uint32_t holder = s.add({ "Holder", {} });
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/widths", s);
		if (p == nullptr) {
			return;
		}
		CHECK(WorkBudget::chain_descriptors(EbxStruct(p, 2, 0)) == 2);
		const std::optional<EbxInstance> h = p->instance(holder);
		REQUIRE(h.has_value());
		CHECK(WorkBudget::chain_descriptors(h->data()) == 3);
		const std::optional<EbxValue> inline_value = h->get("Inline");
		REQUIRE(inline_value.has_value());
		REQUIRE(inline_value->as_struct().has_value());

		const std::optional<EbxValue> pick = h->get("Pick");
		REQUIRE(pick.has_value());
		CHECK_FALSE(pick->enum_name().has_value());
		CHECK(h->get_path("Inline.w").has_value());
		CHECK_FALSE(h->get_path("Inline.missing").has_value());

		WorkBudget budget(2);
		CHECK(budget.charge_type(h->data()).ok());
		CHECK(budget.charge_fields(h->data()).ok());
		CHECK_FALSE(budget.charge_type(h->data()).ok());
	}

	TEST_CASE("gameplay: round 5 repro - wide inline structs in a narrow pose (read_soldier_def)") {

		const auto build = [](size_t p_poses) {
			SynPartition s;
			const std::string wide = add_wide_value_type(s, 4);
			s.types.push_back({ "SoldierBlueprint", { { "Object", 'r' } } });
			s.types.push_back({ "SoldierEntityData", { { "CharacterPhysics", 'r' } } });
			s.types.push_back({ "CharacterPhysicsData", { { "Poses", 'a' } } });
			s.types.push_back({ "CharacterPoseData", { { "EyePosition", 'S', wide }, { "CollisionBoxMinExpand", 'S', wide },
															 { "CollisionBoxMaxExpand", 'S', wide }, { "LookConstraints", 'S', wide }, { "Height", 'i' } } });
			const uint32_t blueprint = s.add({ "SoldierBlueprint", {} });
			const uint32_t entity = s.add({ "SoldierEntityData", {} });
			const uint32_t physics = s.add({ "CharacterPhysicsData", {} });
			const uint32_t pose = s.add({ "CharacterPoseData", {} });
			s.instances[blueprint].values = { { "Object", SynPartition::ref(entity) } };
			s.instances[entity].values = { { "CharacterPhysics", SynPartition::ref(physics) } };
			s.instances[physics].values = { { "Poses", s.add_refs(p_poses, SynPartition::ref(pose)) } };
			return s;
		};
		const char *name = "characters/soldiers/syn";
		{

			SynPartition too_wide;
			add_wide_value_type(too_wide, 5);
			too_wide.types.push_back({ "Holder", { { "Pad", 'i' } } });
			too_wide.add({ "Holder", {} });
			EbxDb db;
			const Result<bool> inserted = db.parse_insert(name, too_wide.build());
			REQUIRE_FALSE(inserted.ok());
			CHECK(inserted.error().message.find("along its base chain") != std::string::npos);
		}
		{
			constexpr size_t kPoses = 250000;
			EbxDb db;
			const EbxPartition *p = insert_syn(db, name, build(kPoses));
			if (p == nullptr) {
				return;
			}
			const std::optional<EbxInstance> pose = p->instance(3);
			REQUIRE(pose.has_value());
			const std::optional<EbxValue> eye = pose->get("EyePosition");
			REQUIRE((eye && eye->as_struct()));
			CHECK(WorkBudget::chain_descriptors(*eye->as_struct()) == 4 * 255);
			WorkBudget budget;
			const auto start = std::chrono::steady_clock::now();
			Result<SoldierDef> soldier = read_soldier_def(db, name, &budget);
			REQUIRE_MESSAGE(soldier.ok(), (soldier.ok() ? std::string() : soldier.error().message));
			CHECK(soldier->poses.size() == kPoses);
			CHECK(budget.used() >= 3 * kPoses);
			CHECK(seconds_since(start) < kFastSeconds);
		}
		EbxDb db;
		if (insert_syn(db, name, build(600000)) == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Result<SoldierDef> soldier = read_soldier_def(db, name);
		REQUIRE_FALSE(soldier.ok());
		CHECK(budget_error(soldier.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: round 5 repro - K prefabs sharing one EventConnections array with wide inline events") {

		const auto run = [](EbxDb &r_db, size_t p_prefabs, size_t p_connections, WorkBudget &r_budget, bool p_wide) -> Result<LogicWorld> {
			SynPartition root;
			root.seed = 1;
			root.types = {
				{ "LevelData", { { "Objects", 'a' } } },
				{ "SubWorldReferenceObjectData", { { "AutoLoad", 'b' }, { "BundleName", 's' } } },
			};
			const uint32_t level = root.add({ "LevelData", {} });
			const uint32_t sub_ref = root.add({ "SubWorldReferenceObjectData", { { "AutoLoad", 1 }, { "BundleName", root.add_string("levels/synthetic/events_sub") } } });
			root.instances[level].values = { { "Objects", root.add_refs(1, SynPartition::ref(sub_ref)) } };
			SynPartition sub;
			sub.seed = 2;
			std::string wide = "EventSpec";
			if (p_wide) {
				wide = add_wide_value_type(sub, 4);
			} else {
				sub.types.push_back({ "EventSpec", { { "Id", 'i' } }, true });
			}
			sub.types.push_back({ "EventConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceEvent", 'S', wide }, { "TargetEvent", 'S', wide } }, true });
			sub.types.push_back({ "SubWorldData", { { "Objects", 'a' } } });
			sub.types.push_back({ "ReferenceObjectData", { { "Blueprint", 'r' }, { "Excluded", 'b' } } });
			sub.types.push_back({ "PrefabBlueprint", { { "Objects", 'a' }, { "EventConnections", 'v', "EventConnection" } } });
			const uint32_t world = sub.add({ "SubWorldData", {} });
			SynArray events{ "EventConnection", {}, {} };
			SynArray objects;
			std::vector<uint32_t> prefabs;
			for (size_t i = 0; i < p_prefabs; ++i) {
				prefabs.push_back(sub.add({ "PrefabBlueprint", {} }));
			}
			for (size_t i = 0; i < p_prefabs; ++i) {
				objects.refs.push_back(SynPartition::ref(sub.add({ "ReferenceObjectData", { { "Blueprint", SynPartition::ref(prefabs[i]) } } })));
			}
			for (size_t i = 0; i < p_connections; ++i) {
				events.structs.push_back({ { "Source", SynPartition::ref(prefabs[0]) }, { "Target", SynPartition::ref(prefabs[0]) } });
			}
			const uint32_t shared = sub.add_array(events);
			for (const uint32_t prefab : prefabs) {
				sub.instances[prefab].values = { { "EventConnections", shared } };
			}
			sub.instances[world].values = { { "Objects", sub.add_array(objects) } };
			const EbxPartition *p = insert_syn(r_db, "levels/synthetic/events", root);
			if (p == nullptr) {
				return make_error("synthetic partition did not parse");
			}
			if (Result<bool> inserted = r_db.parse_insert("levels/synthetic/events_sub", sub.build()); !inserted) {
				return inserted.error();
			}
			LogicWorldOptions options;
			options.budget = &r_budget;
			return LogicWorld::build(r_db, p->primary(), options);
		};
		{
			EbxDb db;
			WorkBudget budget;
			const auto start = std::chrono::steady_clock::now();
			Result<LogicWorld> world = run(db, 100, 9000, budget, true);
			REQUIRE_FALSE(world.ok());
			CHECK(world.error().message.find("cannot be checked") != std::string::npos);
			CHECK(seconds_since(start) < kFastSeconds);
		}
		{
			EbxDb db;
			WorkBudget budget;
			const auto start = std::chrono::steady_clock::now();
			Result<LogicWorld> world = run(db, 100, 9000, budget, false);
			REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
			CHECK(world->scopes().size() == 102);
			CHECK(world->connections(2).event.size() == 9000);
			CHECK(budget.used() >= 100 * 9000);
			CHECK(seconds_since(start) < kFastSeconds);
		}
		EbxDb db;
		WorkBudget budget;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = run(db, 200, 9000, budget, false);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: enum fields decode by binary search (soldier states)") {

		const auto build = [](size_t p_states) {
			SynPartition s;
			SynType choice{ "WideEnum", {}, false, {}, false, true };
			for (int k = 0; k < 255; ++k) {
				choice.fields.push_back({ "Value" + std::to_string(k), 'i', {} });
			}
			s.types.push_back(choice);
			std::string base;
			for (int level = 0; level < 4; ++level) {
				SynType t{ "EnumState" + std::to_string(level), {}, false, base };
				for (int k = 0; k < 254; ++k) {
					t.fields.push_back({ "E" + std::to_string(k), 'e', "WideEnum" });
				}
				s.types.push_back(t);
				base = t.name;
			}
			s.types.push_back({ "SoldierBlueprint", { { "Object", 'r' } } });
			s.types.push_back({ "SoldierEntityData", { { "CharacterPhysics", 'r' } } });
			s.types.push_back({ "CharacterPhysicsData", { { "States", 'a' } } });
			const uint32_t blueprint = s.add({ "SoldierBlueprint", {} });
			const uint32_t entity = s.add({ "SoldierEntityData", {} });
			const uint32_t physics = s.add({ "CharacterPhysicsData", {} });
			const uint32_t state = s.add({ base, {} });
			s.instances[blueprint].values = { { "Object", SynPartition::ref(entity) } };
			s.instances[entity].values = { { "CharacterPhysics", SynPartition::ref(physics) } };
			s.instances[physics].values = { { "States", s.add_refs(p_states, SynPartition::ref(state)) } };
			return s;
		};
		const char *name = "characters/soldiers/syn";
		{
			constexpr size_t kStates = 3000;
			EbxDb db;
			const EbxPartition *p = insert_syn(db, name, build(kStates));
			if (p == nullptr) {
				return;
			}
			const std::optional<EbxInstance> wide = p->instance(3);
			REQUIRE(wide.has_value());
			CHECK(WorkBudget::chain_descriptors(wide->data()) == 4 * 255);
			const std::optional<EbxValue> e = wide->get("E253");
			REQUIRE(e.has_value());
			CHECK_FALSE(e->enum_name().has_value());
			WorkBudget budget;
			const auto start = std::chrono::steady_clock::now();
			Result<SoldierDef> soldier = read_soldier_def(db, name, &budget);
			REQUIRE_MESSAGE(soldier.ok(), (soldier.ok() ? std::string() : soldier.error().message));
			CHECK(soldier->states.size() == kStates);
			CHECK(budget.used() >= kStates * (1 + 4 * 255 / WorkBudget::kFieldsPerUnit));
			CHECK(seconds_since(start) < kFastSeconds);
		}
		EbxDb db;
		if (insert_syn(db, name, build(60000)) == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Result<SoldierDef> soldier = read_soldier_def(db, name);
		REQUIRE_FALSE(soldier.ok());
		CHECK(budget_error(soldier.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: one long string named by many fields decodes without a rescan") {

		constexpr size_t kOptions = 200000;
		constexpr size_t kLength = size_t(2) << 20;
		SynPartition s;
		s.types = world_types();
		s.types.push_back({ "SubWorldInclusionSetting", { { "Criterion", 'r' }, { "EnabledOptions", 'w' } } });
		const uint32_t root = s.add({ "LevelData", {} });
		const uint32_t reference = s.add({ "SubWorldReferenceObjectData", {} });
		const uint32_t settings = s.add({ "SubWorldInclusionSettings", {} });
		const uint32_t setting = s.add({ "SubWorldInclusionSetting", {} });
		const uint32_t text = s.add_string(std::string(kLength, 'a'));
		const uint32_t mode = s.add_string("ConquestLarge0");
		std::vector<uint32_t> options(kOptions, text);
		options.push_back(mode);
		s.instances[setting].values = { { "EnabledOptions", s.add_array(SynArray{ SynArray::kStrings, options, {} }) } };
		s.instances[settings].values = { { "Settings", s.add_refs(1, SynPartition::ref(setting)) } };
		s.instances[reference].values = { { "AutoLoad", 1 }, { "InclusionSettings", SynPartition::ref(settings) } };
		s.instances[root].values = { { "Objects", s.add_refs(1, SynPartition::ref(reference)) } };
		const auto start = std::chrono::steady_clock::now();
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/strings", s);
		if (p == nullptr) {
			return;
		}
		const std::optional<EbxInstance> placed = p->instance(reference);
		REQUIRE(placed.has_value());
		WorkBudget budget;
		const Result<bool> included = subworld_included(db, *placed, "ConquestLarge0", budget);
		REQUIRE(included.ok());
		CHECK(*included);
		const Result<bool> other = subworld_included(db, *placed, "RushLarge0", budget);
		REQUIRE(other.ok());
		CHECK_FALSE(*other);
		CHECK(seconds_since(start) < kFastSeconds);

		const std::optional<EbxValue> list = p->instance(setting)->get("EnabledOptions");
		REQUIRE(list.has_value());
		const OptionalEbxArray array = list->as_array();
		REQUIRE(array.has_value());
		REQUIRE(array->size() == kOptions + 1);
		CHECK(array->get(0)->as_str()->size() == kLength);
		CHECK(*array->get(kOptions)->as_str() == "ConquestLarge0");
	}

	TEST_CASE("gameplay: K references to one wide blueprint pay for its lookups (place_reference)") {

		constexpr size_t kReferences = 400000;
		SynPartition s;
		s.types = world_types();
		std::string base;
		for (int level = 0; level < 3; ++level) {
			SynType t{ "WideBlueprint" + std::to_string(level), {}, false, base };
			if (level == 0) {
				t.fields.push_back({ "Object", 'r', {} });
			}
			while (t.fields.size() < 254) {
				t.fields.push_back({ "Pad" + std::to_string(t.fields.size()), 'i', {} });
			}
			s.types.push_back(t);
			base = t.name;
		}
		SynType blueprint_type{ "ObjectBlueprint", {}, false, base };
		for (int k = 0; k < 254; ++k) {
			blueprint_type.fields.push_back({ "Top" + std::to_string(k), 'i', {} });
		}
		s.types.push_back(blueprint_type);
		const uint32_t root = s.add({ "LevelData", {} });
		const uint32_t reference = s.add({ "ReferenceObjectData", {} });
		const uint32_t blueprint = s.add({ "ObjectBlueprint", {} });
		const uint32_t object = s.add({ "TeamData", {} });
		s.instances[root].values = { { "Objects", s.add_refs(kReferences, SynPartition::ref(reference)) } };
		s.instances[reference].values = { { "Blueprint", SynPartition::ref(blueprint) } };
		s.instances[blueprint].values = { { "Object", SynPartition::ref(object) } };
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/blueprints", s);
		if (p == nullptr) {
			return;
		}
		const std::optional<EbxInstance> bp = p->instance(blueprint);
		REQUIRE(bp.has_value());
		CHECK(WorkBudget::chain_descriptors(bp->data()) == 4 * 255);
		const std::optional<EbxValue> named = bp->get("Object");
		REQUIRE(named.has_value());
		CHECK(named->as_ref() == EbxRef::internal(object));
		LogicWorldOptions options;
		options.expand_root_objects = true;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: round 7 repro - feeds of a depth-1 leaf are charged (property_in, source_value)") {

		constexpr size_t kLevels = 16, kFeeds = 100000;
		const auto build = [&](bool p_walk, size_t p_heads) {
			SynPartition s;
			s.seed = 7;
			s.types = world_types();
			for (SynType &t : s.types) {
				if (t.name == "ReferenceObjectData") {
					t.fields = { { "Blueprint", 'r' } };
				}
			}
			s.types.push_back({ "Node", { { "Pad", 'i' } } });
			s.types.push_back({ "Desc", { { "Pad", 'i' } } });
			const uint32_t root = s.add({ "PrefabBlueprint", {} });
			const uint32_t field = p_walk ? static_cast<uint32_t>(field_id("Excluded")) : 0x13572468u;

			std::vector<uint32_t> node;
			node.push_back(s.add({ p_walk ? "WorldPartReferenceObjectData" : "Node", {} }));
			for (size_t i = 1; i < kLevels; ++i) {
				node.push_back(s.add({ "Node", {} }));
			}
			const uint32_t prefab = s.add({ "PrefabBlueprint", {} });
			const uint32_t desc = s.add({ "Desc", {} });
			const uint32_t source = s.add({ "Node", {} });
			const uint32_t leaf = s.add({ "ReferenceObjectData", { { "Blueprint", SynPartition::ref(prefab) } } });
			SynArray chain{ "PropertyConnection", {}, {} };
			for (size_t i = 0; i < kLevels; ++i) {
				const uint32_t from = i + 1 < kLevels ? node[i + 1] : leaf;
				for (int c = 0; c < 2; ++c) {
					chain.structs.push_back({ { "Source", SynPartition::ref(from) }, { "Target", SynPartition::ref(node[i]) }, { "SourceFieldId", field },
							{ "TargetFieldId", field } });
				}
			}
			SynArray inner{ "PropertyConnection", {}, {} };
			for (size_t i = 0; i < kFeeds; ++i) {
				inner.structs.push_back({ { "Source", SynPartition::ref(source) }, { "Target", SynPartition::ref(desc) }, { "SourceFieldId", field },
						{ "TargetFieldId", field } });
			}
			SynArray objects{ {}, std::vector<uint32_t>(p_heads, SynPartition::ref(node[0])), {} };
			s.instances[root].values = { { "PropertyConnections", s.add_array(chain) }, { "Objects", s.add_array(objects) } };
			s.instances[prefab].values = { { "PropertyConnections", s.add_array(inner) }, { "Descriptor", SynPartition::ref(desc) } };
			return std::make_pair(s, field);
		};
		{

			auto [s, field] = build(false, 0);
			EbxDb db;
			const EbxPartition *p = insert_syn(db, "levels/synthetic/depth", s);
			if (p == nullptr) {
				return;
			}
			LogicWorldOptions options;
			options.max_query_steps = SIZE_MAX;
			Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
			REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
			const std::optional<EbxInstance> head = p->instance(1);
			REQUIRE(head.has_value());
			const auto start = std::chrono::steady_clock::now();
			const size_t before = world->budget().used();
			CHECK_FALSE(world->property(0, *head, static_cast<int32_t>(field)).has_value());
			const size_t one = world->budget().used() - before;
			CHECK(one > 65536);
			CHECK(one < 600000);
			for (int i = 0; i < 6; ++i) {
				CHECK_FALSE(world->property(0, *head, static_cast<int32_t>(field)).has_value());
			}
			CHECK(world->budget().exhausted());
			CHECK(seconds_since(start) < kFastSeconds);
		}

		auto [s, field] = build(true, 10);
		(void)field;
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/depth", s);
		if (p == nullptr) {
			return;
		}
		LogicWorldOptions options;
		options.max_walk_steps = SIZE_MAX;
		options.max_query_steps = SIZE_MAX;
		const auto start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, p->primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(budget_error(world.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: K specialization pairs copying one long target name (read_soldier_def)") {

		const auto build = [](size_t p_pairs, size_t p_name_length) {
			SynPartition s;
			s.types = world_types();
			const std::string long_type = "Target" + std::string(p_name_length, 'x');
			s.types.push_back({ "SoldierBlueprint", { { "Object", 'r' }, { "PropertyConnections", 'v', "PropertyConnection" } } });
			s.types.push_back({ "SoldierEntityData", { { "Components", 'a' } } });
			s.types.push_back({ "UnlockComponentData", { { "UnlockAsset", 'r' } } });
			s.types.push_back({ "ValueUnlockAsset", { { "UnlockValues", 'a' } } });
			s.types.push_back({ "UnlockValuePair", { { "Id", 's' }, { "UnlockedValue", 'i' }, { "DefaultValue", 'i' } } });
			s.types.push_back({ long_type, { { "Boost", 'i' } } });
			const uint32_t blueprint = s.add({ "SoldierBlueprint", {} });
			const uint32_t entity = s.add({ "SoldierEntityData", {} });
			const uint32_t unlock = s.add({ "UnlockComponentData", {} });
			const uint32_t asset = s.add({ "ValueUnlockAsset", {} });
			const uint32_t pair = s.add({ "UnlockValuePair", { { "Id", s.add_string("BoostL1") }, { "UnlockedValue", 2 } } });
			const uint32_t target = s.add({ long_type, {} });
			const uint32_t boost = static_cast<uint32_t>(field_id("Boost"));
			s.instances[blueprint].values = { { "Object", SynPartition::ref(entity) },
				{ "PropertyConnections", s.add_array(SynArray{ "PropertyConnection", {},
												 { { { "Source", SynPartition::ref(unlock) }, { "Target", SynPartition::ref(target) }, { "SourceFieldId", 1 },
														 { "TargetFieldId", boost } } } }) } };
			s.instances[entity].values = { { "Components", s.add_refs(1, SynPartition::ref(unlock)) } };
			s.instances[unlock].values = { { "UnlockAsset", SynPartition::ref(asset) } };
			s.instances[asset].values = { { "UnlockValues", s.add_refs(p_pairs, SynPartition::ref(pair)) } };
			return s;
		};
		const char *name = "characters/soldiers/syn";
		{
			EbxDb db;
			if (insert_syn(db, name, build(3, 10)) == nullptr) {
				return;
			}
			Result<SoldierDef> soldier = read_soldier_def(db, name);
			REQUIRE_MESSAGE(soldier.ok(), (soldier.ok() ? std::string() : soldier.error().message));
			REQUIRE(soldier->specializations.size() == 3);
			CHECK(soldier->specializations[0].id == "BoostL1");
			CHECK(soldier->specializations[0].target_type == "Target" + std::string(10, 'x'));
			CHECK(soldier->specializations[0].target_field == "Boost");
			CHECK(soldier->specializations[2].value == Approx(2));
		}
		EbxDb db;

		if (insert_syn(db, name, build(100000, (size_t(1) << 20) + 7)) == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Result<SoldierDef> soldier = read_soldier_def(db, name);
		REQUIRE_FALSE(soldier.ok());
		CHECK(budget_error(soldier.error()));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("gameplay: round 4 repro - crafted target field ids do not flood the connection index") {

		constexpr size_t kConnections = 100000;
		constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
		SynPartition s;
		s.types = world_types();
		const uint32_t root = s.add({ "PrefabBlueprint", {} });
		std::vector<uint32_t> targets;
		std::vector<int32_t> fields;
		std::vector<SynValues> connections;
		for (size_t i = 0; i < kConnections; ++i) {
			const uint32_t t = s.add({ "WorldPartData", {} });
			targets.push_back(t);
			fields.push_back(static_cast<int32_t>(static_cast<uint32_t>((uint64_t(t) * kGolden) >> 1)));
			connections.push_back({ { "Source", SynPartition::ref(t) }, { "Target", SynPartition::ref(t) }, { "SourceFieldId", 1 },
					{ "TargetFieldId", static_cast<uint32_t>(fields.back()) } });
		}

		for (uint32_t k = 0; k < 2; ++k) {
			connections.push_back({ { "Source", SynPartition::ref(targets[1 + k]) }, { "Target", SynPartition::ref(targets[0]) }, { "SourceFieldId", 2 + k },
					{ "TargetFieldId", static_cast<uint32_t>(fields[0]) } });
		}
		s.instances[root].values = { { "PropertyConnections", s.add_array(SynArray{ "PropertyConnection", {}, connections }) } };
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/collide", s);
		if (p == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		WorkBudget budget;
		Result<LogicConnections> set = LogicConnections::of(db, p->primary(), budget);
		REQUIRE(set.ok());
		CHECK(seconds_since(start) < kFastSeconds);
		REQUIRE(set->property.size() == kConnections + 2);
		const auto into = [&](uint32_t p_target, int32_t p_field) {
			const std::optional<EbxInstance> target = p->instance(p_target);
			REQUIRE(target.has_value());
			const std::span<const uint32_t> at = set->property_into(*target, p_field);
			return std::vector<uint32_t>(at.begin(), at.end());
		};
		CHECK(into(targets[0], fields[0]) == std::vector<uint32_t>{ 0, kConnections, kConnections + 1 });
		for (size_t i = 1; i < kConnections; i += 997) {
			CHECK(into(targets[i], fields[i]) == std::vector<uint32_t>{ static_cast<uint32_t>(i) });
		}
		CHECK(into(targets[5], fields[6]).empty());
		const auto world_start = std::chrono::steady_clock::now();
		Result<LogicWorld> world = LogicWorld::build(db, p->primary(), {});
		REQUIRE(world.ok());
		CHECK(seconds_since(world_start) < kFastSeconds);
	}

	TEST_CASE("gameplay: round 4 repro - crafted unlock GUIDs do not flood the progression gates") {

		constexpr size_t kUnlocks = 250000;
		constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
		SynPartition s;
		s.types = {
			{ "BasicUnlockInfo", { { "UnlockGuid", 'g' }, { "W1", 'i' }, { "W2", 'i' }, { "W3", 'i' } }, true },
			{ "RankLevelData", { { "UnlockInfos", 'v', "BasicUnlockInfo" } } },
		};
		const uint32_t root = s.add({ "RankLevelData", {} });
		std::vector<Guid> guids;
		std::vector<SynValues> infos;
		for (size_t i = 0; i < kUnlocks; ++i) {
			const uint64_t b = i + 1, a = b * kGolden;
			Guid g;
			std::memcpy(g.bytes.data(), &a, 8);
			std::memcpy(g.bytes.data() + 8, &b, 8);
			guids.push_back(g);
			infos.push_back({ { "UnlockGuid", uint32_t(a) }, { "W1", uint32_t(a >> 32) }, { "W2", uint32_t(b) }, { "W3", uint32_t(b >> 32) } });
		}
		s.instances[root].values = { { "UnlockInfos", s.add_array(SynArray{ "BasicUnlockInfo", {}, infos }) } };
		EbxDb db;
		if (insert_syn(db, "persistence/ranks/rankparams", s) == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Result<UnlockProgression> progression = read_unlock_progression(db);
		REQUIRE(progression.ok());
		CHECK(seconds_since(start) < kFastSeconds);
		CHECK(progression->gates.size() == kUnlocks);
		for (size_t i = 0; i < kUnlocks; i += 997) {
			CHECK(progression->gate(guids[i]) == UnlockGate::Rank);
		}
		CHECK(progression->gate(Guid{}) == UnlockGate::Free);
	}

	TEST_CASE("gameplay: keyed hashes spread keys crafted against a fixed mixer") {

		constexpr size_t kKeys = 20000;
		constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
		const auto max_bucket = [](const auto &p_map) {
			size_t most = 0;
			for (size_t i = 0; i < p_map.bucket_count(); ++i) {
				most = std::max(most, p_map.bucket_size(i));
			}
			return most;
		};
		std::unordered_map<Guid, uint32_t, GuidHash> guids;
		std::unordered_map<Sha1, uint32_t, Sha1Hash> sha1s;
		std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> instances;
		for (uint32_t i = 0; i < kKeys; ++i) {
			const uint64_t b = uint64_t(i) + 1, a = b * kGolden;
			Guid g;
			std::memcpy(g.bytes.data(), &a, 8);
			std::memcpy(g.bytes.data() + 8, &b, 8);
			guids.emplace(g, i);
			Sha1 h;
			std::memcpy(h.bytes.data() + 16, &i, 4);
			sha1s.emplace(h, i);
			instances.emplace(InstanceKey{ nullptr, i }, i);
		}
		REQUIRE(guids.size() == kKeys);
		REQUIRE(sha1s.size() == kKeys);
		CHECK(max_bucket(guids) < 32);
		CHECK(max_bucket(sha1s) < 32);
		CHECK(max_bucket(instances) < 32);
		CHECK(GuidHash()(guids.begin()->first) == GuidHash()(guids.begin()->first));
	}

	TEST_CASE("gameplay: kit default skips gated and variant unlocks") {
		KitDef kit;
		kit.weapon_slots.resize(kWeaponSlotCount);
		KitSlot &knife = kit.weapon_slots[static_cast<size_t>(WeaponSlot::Knife)];
		KitUnlock razor{ "weapons/xp2_knife_razorblade/u_knife_razor", "SoldierWeaponUnlockAsset", "weapons/xp2_knife_razorblade/knife_razorblade",
			UnlockGate::Licence, false };
		KitUnlock stock{ "weapons/knife/u_knife", "SoldierWeaponUnlockAsset", "weapons/knife/knife", UnlockGate::Free, false };
		knife.unlocks = { razor, stock };
		KitSlot &launcher = kit.weapon_slots[static_cast<size_t>(WeaponSlot::Gadget1)];
		KitUnlock he{ "weapons/gadgets/m320/u_m320_he", "SoldierWeaponUnlockAsset", "weapons/gadgets/m320/m320", UnlockGate::ScoringBucket, false };
		KitUnlock shg{ "weapons/gadgets/m320/u_m320_shg", "SoldierWeaponUnlockAsset", "weapons/gadgets/m320/m320", UnlockGate::Free, true };
		launcher.unlocks = { he, shg };

		CHECK(kit.default_unlock(WeaponSlot::Knife) == nullptr);
		kit.progression_known = true;
		REQUIRE(kit.default_unlock(WeaponSlot::Knife) != nullptr);
		CHECK(kit.default_unlock(WeaponSlot::Knife)->unlock == "weapons/knife/u_knife");
		CHECK(kit.default_unlock(WeaponSlot::Gadget1) == nullptr);
		CHECK(kit.default_unlock(WeaponSlot::Primary) == nullptr);
		CHECK(std::string(unlock_gate_name(UnlockGate::Licence)) == "licence");
	}

	TEST_CASE("gameplay: interface constants parse by their type word") {
		std::optional<PropertyValue> v = PropertyValue::parse("Int32 250");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Int);
		CHECK(v->as_i64() == 250);
		v = PropertyValue::parse("  Float32 1.5 ");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Float);
		CHECK(*v->as_f64() == Approx(1.5));
		v = PropertyValue::parse("Boolean True");
		REQUIRE(v.has_value());
		CHECK(v->as_bool() == true);
		v = PropertyValue::parse("TeamId Team2");
		REQUIRE(v.has_value());
		CHECK(v->as_team() == 2);
		v = PropertyValue::parse("CString \"ID_LOC_A\"");
		REQUIRE(v.has_value());
		CHECK(v->as_str() == std::optional<std::string_view>("ID_LOC_A"));
		v = PropertyValue::parse("Vec3 1 2 3");
		REQUIRE(v.has_value());
		REQUIRE(v->as_vec3().has_value());
		CHECK((*v->as_vec3())[2] == Approx(3));

		v = PropertyValue::parse("Int32 12abc");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Constant);
		CHECK(!v->as_i64().has_value());
		v = PropertyValue::parse("Vec3 1 2");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Constant);
		CHECK(!PropertyValue::parse("   ").has_value());
	}

	TEST_CASE("gameplay: piecewise-linear curves clamp outside their range") {
		const std::vector<CurvePoint> fall = { { 0, 0 }, { 13.9f, 0 }, { 14, 20 }, { 22, 40 }, { 30, 100 } };
		CHECK(eval_curve({}, 5) == 0);
		CHECK(eval_curve(fall, -3) == Approx(0));
		CHECK(eval_curve(fall, 10) == Approx(0));
		CHECK(eval_curve(fall, 18) == Approx(30));
		CHECK(eval_curve(fall, 26) == Approx(70));
		CHECK(eval_curve(fall, 99) == Approx(100));
	}

	TEST_CASE("gameplay: mode aliases") {
		CHECK(canonical_mode_id("CQS") == "ConquestSmall0");
		CHECK(canonical_mode_id("cql") == "ConquestLarge0");
		CHECK(canonical_mode_id("RL") == "RushLarge0");
		CHECK(canonical_mode_id("SQDM") == "SquadDeathMatch0");
		CHECK(canonical_mode_id("TDM") == "TeamDeathMatch0");
		CHECK(canonical_mode_id("Domination0") == "Domination0");
		CHECK(soldier_pose_from_name("CharacterPoseType_Prone") == SoldierPose::Prone);
		CHECK(!soldier_pose_from_name("CharacterPoseType_Lie").has_value());
	}
}

TEST_SUITE("tables") {
	TEST_CASE("gameplay: SoldierDef (plan §7.2)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_001");
		Result<SoldierDef> read = read_soldier_def(*lib);
		REQUIRE_MESSAGE(read.ok(), (read.ok() ? std::string() : read.error().message));
		const SoldierDef &s = *read;
		CHECK(s.max_health == Approx(100));
		CHECK(s.low_health_threshold == Approx(30));
		CHECK(s.regeneration_delay == Approx(10));
		CHECK(s.regeneration_rate == Approx(5));
		CHECK(s.immortal_time_after_spawn == Approx(2));
		CHECK(s.man_down_time == Approx(13));
		CHECK(s.post_revive_health == Approx(20));
		CHECK(s.time_for_corpse == Approx(15));
		CHECK(s.mass == Approx(100));
		CHECK(s.radius == Approx(0.35));
		CHECK(s.max_ascend_angle == Approx(50));
		CHECK(s.water_depth_limit == Approx(1.4));

		const PoseDef *stand = s.pose(SoldierPose::Stand);
		const PoseDef *crouch = s.pose(SoldierPose::Crouch);
		const PoseDef *prone = s.pose(SoldierPose::Prone);
		REQUIRE(stand != nullptr);
		REQUIRE(crouch != nullptr);
		REQUIRE(prone != nullptr);
		CHECK(stand->height == Approx(1.8));
		CHECK(crouch->height == Approx(1.15));
		CHECK(prone->height == Approx(0.3));
		CHECK(stand->eye_height == Approx(1.6));
		CHECK(crouch->eye_height == Approx(1.0));
		CHECK(prone->eye_height == Approx(0.3));
		CHECK(stand->step_height == Approx(0.3));
		CHECK(stand->transition_to[1].value_or(-1) == Approx(0.3));
		CHECK(stand->transition_to[2].value_or(-1) == Approx(0.7));
		CHECK(crouch->transition_to[0].value_or(-1) == Approx(0.3));
		CHECK(crouch->transition_to[2].value_or(-1) == Approx(0.5));
		CHECK(prone->transition_to[0].value_or(-1) == Approx(0.7));
		CHECK(prone->transition_to[1].value_or(-1) == Approx(1.05));

		CHECK(s.ground_speed(SoldierPose::Stand).value_or(-1) == Approx(4));
		CHECK(s.ground_speed(SoldierPose::Crouch).value_or(-1) == Approx(2));
		CHECK(s.ground_speed(SoldierPose::Prone).value_or(-1) == Approx(0.9));
		CHECK(s.sprint_speed().value_or(-1) == Approx(6.5));
		CHECK(s.jump_height().value_or(-1) == Approx(0.6));
		const MovementStateDef *ground = s.state("OnGroundStateData");
		REQUIRE(ground != nullptr);
		REQUIRE(ground->pose(SoldierPose::Stand) != nullptr);
		CHECK(ground->pose(SoldierPose::Stand)->backward == Approx(0.75));
		CHECK(ground->pose(SoldierPose::Stand)->left == Approx(0.75));
		const MovementStateDef *swim = s.state("SwimmingStateData");
		const MovementStateDef *climb = s.state("ClimbingStateData");
		const MovementStateDef *chute = s.state("ParachuteStateData");
		REQUIRE(swim != nullptr);
		REQUIRE(climb != nullptr);
		REQUIRE(chute != nullptr);
		REQUIRE(swim->pose(SoldierPose::Stand) != nullptr);
		REQUIRE(climb->pose(SoldierPose::Stand) != nullptr);
		REQUIRE(chute->pose(SoldierPose::Stand) != nullptr);
		CHECK(swim->pose(SoldierPose::Stand)->velocity == Approx(3));
		CHECK(climb->pose(SoldierPose::Stand)->velocity == Approx(1.32));
		CHECK(chute->pose(SoldierPose::Stand)->velocity == Approx(9));
		CHECK(chute->scalar("TerminalVelocity").value_or(-1) == Approx(5));

		CHECK(s.fall_damage(13.9f) == Approx(0));
		CHECK(s.fall_damage(14) == Approx(20));
		CHECK(s.fall_damage(22) == Approx(40));
		CHECK(s.fall_damage(30) == Approx(100));
		CHECK(s.fall_damage(40) == Approx(100));

		struct Spec {
			const char *id;
			double value;
			const char *field;
		};
		const Spec specs[] = {
			{ "SprintBoostL1", 1.1, "SprintMultiplier" },
			{ "AmmoBoostL1", 1.5, "AmmoClipIncreaseMultiplier" },
			{ "ExplosiveBoostL1", 2.0, "ExplosiveIncreaseMultiplier" },
			{ "ExplosiveResistL1", 0.85, "ExplosionDamageModifier" },
			{ "SuppressionResistL1", 0.5, "SuppressionResistModifier" },
			{ "SuppressionBoostL1", 1.5, "SuppressionBoostModifier" },
			{ "GrenadeBoostL1", 1.0, "GrenadeIncrease" },
			{ "UnderslungBoostL1", 3.0, "UnderslungGrenadeIncrease" },
			{ "HealSpeedBoostL1", 0.8, "RegenerationDelayModifier" },
		};
		for (const Spec &spec : specs) {
			CAPTURE(spec.id);
			const SpecializationDef *d = s.specialization(spec.id);
			REQUIRE(d != nullptr);
			CHECK(d->value == Approx(spec.value));
			CHECK(d->target_field == spec.field);
			CHECK(!d->squad);
		}
		const SpecializationDef *squad = s.specialization("SprintBoostL2");
		REQUIRE(squad != nullptr);
		CHECK(squad->squad);
		CHECK(squad->value == Approx(1.1));
		CHECK(s.specializations.size() == 18);
	}

	TEST_CASE("gameplay: ModeDef MP_001 and scoring (plan §7.3)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_001");
		const Result<std::vector<std::string>> modes = level_modes(*lib, "MP_001");
		REQUIRE(modes.ok());
		CHECK(modes->size() == 7);

		const ModeDef cqs = mode(*lib, "MP_001", "CQS");
		CHECK(cqs.mode_id == "ConquestSmall0");
		for (const int team : { 1, 2 }) {
			CAPTURE(team);
			REQUIRE(cqs.tickets(team) != nullptr);
			CHECK(cqs.tickets(team)->initial_tickets == 250);
		}
		CHECK(cqs.flag_area_value().value_or(-1) == Approx(51));
		CHECK(cqs.flag_count() == 3);
		CHECK(hq_count(cqs) == 2);
		REQUIRE(cqs.size.has_value());
		CHECK(cqs.size->player_count == 32);
		CHECK(cqs.size->squad_size() == 4);
		REQUIRE(cqs.teams.size() >= 2);
		CHECK(cqs.teams.front().team <= cqs.teams.back().team);
		REQUIRE(cqs.team(1) != nullptr);
		CHECK(cqs.team(1)->faction == "FactionUS");
		CHECK(cqs.team(1)->kits.size() == 4);

		const ModeDef cql = mode(*lib, "MP_001", "CQL");
		REQUIRE(cql.tickets(1) != nullptr);
		CHECK(cql.tickets(1)->initial_tickets == 400);
		CHECK(cql.flag_area_value().value_or(-1) == Approx(34));
		CHECK(cql.flag_count() == 5);
		REQUIRE(cql.size.has_value());
		CHECK(cql.size->player_count == 64);

		const ModeDef rush = mode(*lib, "MP_001", "RL");
		const LifeCounterDef *attackers = nullptr;
		const LifeCounterDef *defenders = nullptr;
		for (const LifeCounterDef &l : rush.life_counters) {
			(l.attacker ? attackers : defenders) = &l;
		}
		REQUIRE(attackers != nullptr);
		REQUIRE(defenders != nullptr);
		CHECK(attackers->starting_lives == 75);
		CHECK(defenders->base_count == 4);

		REQUIRE(rush.mcoms.size() >= 8);
		for (const McomDef &c : rush.mcoms) {
			CHECK(c.arm_time.value_or(-1) == Approx(2.5));
			CHECK(c.disarm_time.value_or(-1) == Approx(5));
			CHECK(c.radius.value_or(-1) == Approx(1.5));
			CHECK(c.fuse.value_or(-1) == Approx(25));
		}
		REQUIRE(rush.size.has_value());
		CHECK(rush.size->player_count == 32);
		CHECK(rush.size->rounds_per_map == 2);

		const ModeDef squad_rush = mode(*lib, "MP_001", "SR");
		bool sr_attackers = false;
		for (const LifeCounterDef &l : squad_rush.life_counters) {
			if (l.attacker) {
				sr_attackers = true;
				CHECK(l.starting_lives == 20);
			}
		}
		CHECK(sr_attackers);
		REQUIRE(!squad_rush.mcoms.empty());
		CHECK(squad_rush.mcoms.front().arm_time.value_or(-1) == Approx(5));
		REQUIRE(squad_rush.size.has_value());
		CHECK(squad_rush.size->player_count == 8);

		const ModeDef tdm = mode(*lib, "MP_001", "TDM");
		for (const int team : { 1, 2 }) {
			CAPTURE(team);
			REQUIRE(tdm.kills(team) != nullptr);
			CHECK(tdm.kills(team)->max_kills == 100);
		}

		const ModeDef sqdm = mode(*lib, "MP_001", "SQDM");
		for (const int team : { 1, 2, 3, 4 }) {
			CAPTURE(team);
			REQUIRE(sqdm.kills(team) != nullptr);
			CHECK(sqdm.kills(team)->max_kills == 50);
		}
		REQUIRE(sqdm.size.has_value());
		CHECK(sqdm.size->player_count == 16);

		Result<ScoringTable> scoring = read_scoring(*lib);
		REQUIRE_MESSAGE(scoring.ok(), (scoring.ok() ? std::string() : scoring.error().message));
		CHECK(scoring->score("ID_SCORE_KILL").value_or(-1) == Approx(100));
		CHECK(scoring->score("ID_SCORE_CAPTURE_FLAG").value_or(-1) == Approx(250));
		CHECK(scoring->score("ID_SCORE_NEUTRALIZE_FLAG").value_or(-1) == Approx(200));
		CHECK(scoring->score("ID_SCORE_REVIVE").value_or(-1) == Approx(100));
		CHECK(scoring->score("ID_SCORE_HEADSHOT").value_or(-1) == Approx(10));
		CHECK(!scoring->score("ID_SCORE_NO_SUCH").has_value());
	}

	TEST_CASE("gameplay: ModeDef Metro, Caspian Border and Palace Domination (plan §7.3)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		{
			const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_Subway");
			const ModeDef cqs = mode(*lib, "MP_Subway", "CQS");
			REQUIRE(cqs.tickets(1) != nullptr);
			CHECK(cqs.tickets(1)->initial_tickets == 350);
			CHECK(cqs.flag_area_value().value_or(-1) == Approx(55));
			CHECK(cqs.flag_count() == 3);
			const ModeDef rush = mode(*lib, "MP_Subway", "RL");
			bool attackers = false;
			for (const LifeCounterDef &l : rush.life_counters) {
				if (l.attacker) {
					attackers = true;
					CHECK(l.starting_lives == 100);
				}
			}
			CHECK(attackers);
			REQUIRE(!rush.mcoms.empty());
			CHECK(rush.mcoms.front().fuse.value_or(-1) == Approx(25));
			const ModeDef tdm = mode(*lib, "MP_Subway", "TDM");
			REQUIRE(tdm.kills(1) != nullptr);
			CHECK(tdm.kills(1)->max_kills == 100);
		}
		{
			const std::unique_ptr<EbxLibrary> lib = open_level(m, "XP2_Palace");
			const ModeDef dom = mode(*lib, "XP2_Palace", "Domination0");
			REQUIRE(dom.tickets(1) != nullptr);
			CHECK(dom.tickets(1)->initial_tickets == 150);
			CHECK(dom.flag_count() == 3);
			REQUIRE(dom.size.has_value());
			CHECK(dom.size->player_count == 16);
		}
		{
			const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_007");
			const ModeDef cql = mode(*lib, "MP_007", "CQL");
			REQUIRE(cql.tickets(1) != nullptr);
			CHECK(cql.tickets(1)->initial_tickets == 300);
			CHECK(cql.flag_area_value().value_or(-1) == Approx(34));
		}
	}

	TEST_CASE("gameplay: KitDef per team (plan §3.4)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_001");
		const ModeDef cqs = mode(*lib, "MP_001", "CQS");
		const Result<UnlockProgression> read_progression = read_unlock_progression(*lib);
		REQUIRE(read_progression.ok());
		const UnlockProgression &progression = *read_progression;
		CHECK(progression.tables == 4);
		CHECK(progression.gates.size() > 500);

		struct Expect {
			int team;
			const char *kit;
			const char *primary;
			const char *sidearm;
			const char *gadget1;
			const char *kit_item;
			const char *gadget2;
		};
		const Expect expected[] = {
			{ 1, "gameplay/kits/usassault", "weapons/m16a4/m16a4", "weapons/m9/m9", nullptr, "weapons/gadgets/medicbag/medicbag", "" },
			{ 1, "gameplay/kits/usengineer", "weapons/m4a1/m4a1", "weapons/m9/m9", "weapons/smaw/smaw", nullptr,
				"weapons/gadgets/repairtool/repairtool" },
			{ 1, "gameplay/kits/ussupport", "weapons/m27iar/m27iar", "weapons/m9/m9", nullptr, "weapons/gadgets/ammobag/ammobag", "" },
			{ 1, "gameplay/kits/usrecon", "weapons/mk11/mk11", "weapons/m9/m9", "", nullptr, "weapons/gadgets/radiobeacon/radio_beacon" },
			{ 2, "gameplay/kits/ruassault", "weapons/ak74m/ak74", "weapons/mp443/mp443", nullptr, "weapons/gadgets/medicbag/medicbag", "" },
			{ 2, "gameplay/kits/ruengineer", "weapons/aks74u/aks74u", "weapons/mp443/mp443", "weapons/rpg7/rpg7", nullptr,
				"weapons/gadgets/repairtool/repairtool" },
			{ 2, "gameplay/kits/rusupport", "weapons/rpk/rpk", "weapons/mp443/mp443", nullptr, "weapons/gadgets/ammobag/ammobag", "" },
			{ 2, "gameplay/kits/rurecon", "weapons/svd/svd", "weapons/mp443/mp443", "", nullptr, "weapons/gadgets/radiobeacon/radio_beacon" },
		};
		const auto check_slot = [](const KitDef &p_kit, WeaponSlot p_slot, const char *p_weapon) {
			CAPTURE(weapon_slot_name(p_slot));
			const KitUnlock *u = p_kit.default_unlock(p_slot);
			if (p_weapon == nullptr) {
				CHECK(u == nullptr);
				return;
			}
			REQUIRE(u != nullptr);
			CHECK(u->weapon == p_weapon);
			CHECK(u->gate == UnlockGate::Free);
			CHECK_FALSE(u->variant);
		};
		for (const Expect &e : expected) {
			CAPTURE(e.kit);
			const TeamDef *team = cqs.team(e.team);
			REQUIRE(team != nullptr);
			bool listed = false;
			for (const std::string &k : team->kits) {
				listed = listed || k == e.kit;
			}
			CHECK(listed);
			Result<KitDef> kit = read_kit_def(*lib, e.kit, &progression);
			REQUIRE_MESSAGE(kit.ok(), (kit.ok() ? std::string() : kit.error().message));
			CHECK(kit->progression_known);
			check_slot(*kit, WeaponSlot::Primary, e.primary);
			check_slot(*kit, WeaponSlot::Sidearm, e.sidearm);
			check_slot(*kit, WeaponSlot::Gadget1, e.gadget1);
			check_slot(*kit, WeaponSlot::KitItem, e.kit_item);
			check_slot(*kit, WeaponSlot::Gadget2, e.gadget2);
			check_slot(*kit, WeaponSlot::Grenade, "weapons/m67/m67");

			check_slot(*kit, WeaponSlot::Knife, "weapons/knife/knife");
			const KitSlot *knives = kit->slot(WeaponSlot::Knife);
			REQUIRE(knives != nullptr);
			REQUIRE(knives->unlocks.size() == 2);
			CHECK(knives->unlocks[0].unlock == "weapons/xp2_knife_razorblade/u_knife_razor");
			CHECK(knives->unlocks[0].gate == UnlockGate::Licence);
		}

		Result<KitDef> own = read_kit_def(*lib, "gameplay/kits/usassault");
		REQUIRE(own.ok());
		REQUIRE(own->default_unlock(WeaponSlot::Knife) != nullptr);
		CHECK(own->default_unlock(WeaponSlot::Knife)->weapon == "weapons/knife/knife");

		const KitSlot *launchers = own->slot(WeaponSlot::Gadget1);
		REQUIRE(launchers != nullptr);
		size_t variants = 0, free_variants = 0;
		for (const KitUnlock &u : launchers->unlocks) {
			variants += u.variant ? 1 : 0;
			free_variants += (u.variant && u.gate == UnlockGate::Free) ? 1 : 0;
		}
		CHECK(variants == 8);
		CHECK(free_variants == 6);
	}

	TEST_CASE("gameplay: VehicleDef (plan §7.4)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_007");

		const VehicleDef m1 = vehicle(*lib, "vehicles/m1a2/m1abrams");
		check_mbt_health(m1);
		const VehicleTurretDef *ring = turret(m1, 0, 1, true);
		const VehicleTurretDef *gun = turret(m1, 0, 0, false);
		const VehicleTurretDef *hmg = turret(m1, 1, 1, false);
		REQUIRE(ring != nullptr);
		REQUIRE(gun != nullptr);
		REQUIRE(hmg != nullptr);
		CHECK(ring->world_space_lock_efficiency == Approx(0.0001));
		CHECK(gun->world_space_lock_efficiency == Approx(0.85));
		CHECK(hmg->world_space_lock_efficiency == Approx(1.0));
		CHECK(gun->constrained);
		CHECK(gun->min_angle == Approx(-20));
		CHECK(gun->max_angle == Approx(0));
		REQUIRE(m1.weapons.size() >= 2);
		CHECK(m1.weapons[0].seat == 0);
		CHECK(m1.weapons[0].classification == "WCPrimary");

		const VehicleDef t90 = vehicle(*lib, "vehicles/t90/t90");
		check_mbt_health(t90);
		const VehicleTurretDef *t90_ring = turret(t90, 0, 1, true);
		const VehicleTurretDef *t90_gun = turret(t90, 0, 0, false);
		REQUIRE(t90_ring != nullptr);
		REQUIRE(t90_gun != nullptr);
		CHECK(t90_ring->world_space_lock_efficiency == Approx(0.0));
		CHECK(t90_gun->world_space_lock_efficiency == Approx(1.0));
		CHECK(t90_gun->min_angle == Approx(-20));
		CHECK(t90_gun->max_angle == Approx(4));

		for (const char *heli : { "vehicles/ah1z/ah1z", "vehicles/mi28/mi28" }) {
			CAPTURE(heli);
			const VehicleDef v = vehicle(*lib, heli);
			CHECK(v.body_mass == Approx(5000));
			CHECK(v.gravity_modifier == Approx(1.0));
			CHECK(v.max_zone_health() == Approx(500));
			CHECK(v.disabled_damage_threshold == Approx(170));
			CHECK(v.emergency_repair_health == Approx(185));
		}
		for (const char *jet : { "vehicles/f18-f/f18", "vehicles/su-35bm-e/su-35bm-e" }) {
			CAPTURE(jet);
			const VehicleDef v = vehicle(*lib, jet);
			CHECK(v.body_mass == Approx(10000));
			CHECK(v.gravity_modifier == Approx(1.0));
			CHECK(v.max_zone_health() == Approx(500));
			CHECK(v.regeneration_rate == Approx(18));
			CHECK(v.regeneration_delay == Approx(10));
			REQUIRE(v.driver() != nullptr);
		}
		const VehicleDef humvee = vehicle(*lib, "vehicles/humveearmored/humveearmored");
		CHECK(humvee.body_mass == Approx(1500));
		CHECK(humvee.gravity_modifier == Approx(1.85));
		CHECK(humvee.seats.size() == 4);
		for (const char *buggy : { "vehicles/vdv_buggy/vdv_buggy", "vehicles/growleritv/growleritv" }) {
			CAPTURE(buggy);
			const VehicleDef v = vehicle(*lib, buggy);
			CHECK(v.body_mass == Approx(800));
			CHECK(v.gravity_modifier == Approx(1.65));
		}
		for (const char *aa : { "vehicles/centurion_c-ram/centurion_c-ram", "vehicles/pantsir/pantsir-s1" }) {
			CAPTURE(aa);
			const VehicleDef v = vehicle(*lib, aa);
			CHECK(v.armor_multiplier == Approx(99999));
		}
		CHECK(!read_vehicle_def(*lib, "vehicles/no/such_vehicle").ok());

		const std::unique_ptr<EbxLibrary> kharg = open_level(m, "MP_018");
		const VehicleDef rhib = vehicle(*kharg, "vehicles/rhib/rhib");
		CHECK(rhib.body_mass == Approx(1000));
		CHECK(rhib.gravity_modifier == Approx(1.25));
	}
}

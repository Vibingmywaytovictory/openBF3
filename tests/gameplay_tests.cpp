// Gameplay tables (bf3_core/gameplay): SoldierDef, KitDef, ModeDef, VehicleDef against the retail
// install (plan §7.2-§7.4, research/topics/02-soldier-kits-modes.md), plus install-free unit cases.
// The install is read through InstallReader / Install / EbxLibrary only (read-only); without one the
// "gameplay" suite skips (exit 77).
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

// The level's own superbundle (self-contained: gameplay/*, characters/*, settings, scoring).
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

// The first turret of a seat that turns about an axis (1 yaw, 0 pitch) at the given nesting.
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

// A synthetic world-part graph as an EBX partition (no install needed), for hostile LogicWorld
// inputs. Node i is instance i: a WorldPartData whose Objects are the listed instances, or a
// WorldPartReferenceObjectData whose Blueprint is instance `target`. Instance 0 is the primary.
//   DataContainer (class, 8); WorldPartData : DataContainer (16): Objects array@8;
//   WorldPartReferenceObjectData : DataContainer (16): Blueprint Class@8; array (member Class)
struct SynthNode {
	bool part = true;
	std::vector<uint32_t> objects; // part: instance indices
	uint32_t target = 0; // reference: instance index
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
		{ "$", ebx_code::kVoid, 0, 0 }, // WorldPartData (0, 1)
		{ "Objects", ebx_code::kArray, 3, 8 },
		{ "$", ebx_code::kVoid, 0, 0 }, // WorldPartReferenceObjectData (2, 3)
		{ "Blueprint", ebx_code::kClass, 0, 8 },
		{ "member", ebx_code::kClass, 0, 0 }, // array (4)
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
	const size_t instances_at = meta_size; // no strings
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
		// One repeater per instance: {internal 0, count 1, complex}.
		put32(b, instance_repeaters_at + i * 12 + 4, 1);
		put32(b, instance_repeaters_at + i * 12 + 8, n.part ? 1 : 2);
		const size_t at = instances_at + i * 32;
		const Guid g = synth_guid(static_cast<uint32_t>(i));
		std::memcpy(b.data() + at, g.bytes.data(), 16);
		const size_t data = at + 16;
		if (!n.part) {
			put32(b, data + 8, n.target + 1); // internal reference
			continue;
		}
		put32(b, data + 8, static_cast<uint32_t>(array_index));
		const size_t rep = array_repeaters_at + array_index * 12;
		put32(b, rep, static_cast<uint32_t>(array_at + 4)); // elements after the stored count
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

// A chain of p_parts world parts, each holding p_fan references to the next one.
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

// A general synthetic EBX partition for the hostile-input cases of every gameplay reader. Each class
// derives from DataContainer (size 8) or from a class listed before it (`base`) and has one 4-byte
// slot per field after its base; a value type has its slots from offset 0. Field kinds: 'b' Boolean, 'i' Int32, 'r' class reference, 's'
// CString, 'a' array of references, 'v' array of the value type `elem`. A field value is the raw
// word, except 's' (an index into `strings`) and 'a' / 'v' (an index into `arrays`; arrays may be
// shared by any number of fields, as EBX allows). Unset strings are null, unset arrays empty.
struct SynField {
	std::string name;
	char kind = 'i';
	std::string elem;
};

struct SynType {
	std::string name;
	std::vector<SynField> fields;
	bool value_type = false;
	std::string base; // a class listed earlier; empty: DataContainer
};

using SynValues = std::vector<std::pair<std::string, uint32_t>>;

struct SynArray {
	std::string elem; // the value type of a struct array, empty for references
	std::vector<uint32_t> refs;
	std::vector<SynValues> structs;
};

struct SynInstance {
	std::string type;
	SynValues values;
};

struct SynPartition {
	std::vector<SynType> types;
	std::vector<SynInstance> instances; // instance 0 is the primary
	std::vector<SynArray> arrays;
	std::vector<std::string> strings;
	uint32_t imports = 0; // imports of a partition no db holds: references to them never resolve
	uint32_t seed = 1; // partitions of one db need distinct seeds (GUIDs)

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
	// A reference array of p_count copies of p_word (null refs when 0).
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
	// Element kinds of the array complexes: "" (references), then each value type used.
	std::vector<std::string> elems{ "" };
	for (const SynType &t : types) {
		add_name(t.name);
		for (const SynField &f : t.fields) {
			add_name(f.name);
			if (f.kind == 'v' && std::find(elems.begin(), elems.end(), f.elem) == elems.end()) {
				elems.push_back(f.elem);
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
	// Byte size of each type (bases come first in `types`), and where its own fields start.
	std::vector<uint32_t> sizes;
	for (const SynType &t : types) {
		const uint32_t base = t.value_type ? 0 : (t.base.empty() ? 8 : sizes[type_at(t.base)]);
		sizes.push_back(base + static_cast<uint32_t>(4 * t.fields.size()));
	}
	const auto base_of = [&](const SynType &p_type) -> uint32_t {
		return p_type.value_type ? 0 : (p_type.base.empty() ? 8 : sizes[type_at(p_type.base)]);
	};
	for (const SynType &t : types) {
		const uint32_t start = static_cast<uint32_t>(fields.size());
		if (!t.value_type) {
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
				case 'a':
				case 'v':
					code = ebx_code::kArray;
					ref = array_complex(f.kind == 'a' ? std::string() : f.elem);
					break;
				default:
					break;
			}
			fields.push_back({ f.name, code, ref, base_of(t) + static_cast<uint32_t>(4 * k) });
		}
		complexes.push_back({ t.name, start, static_cast<uint8_t>(fields.size() - start), t.value_type ? ebx_code::kValueType : ebx_code::kClass,
				static_cast<uint16_t>(base_of(t) + 4 * t.fields.size()) });
	}
	for (const std::string &e : elems) {
		const uint32_t start = static_cast<uint32_t>(fields.size());
		fields.push_back({ "member", e.empty() ? ebx_code::kClass : ebx_code::kValueType, static_cast<uint16_t>(e.empty() ? 0 : 1 + type_at(e)), 0 });
		complexes.push_back({ "array", start, 1, ebx_code::kArray, 4 });
	}

	// Arrays: the listed ones, then one empty array per element kind for unset fields.
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
	const auto stride = [&](const SynArray &p_array) -> size_t { return p_array.elem.empty() ? 4 : complexes[1 + type_at(p_array.elem)].size; };
	size_t array_bytes = 0;
	for (const SynArray &a : all) {
		array_bytes += 4 + (a.elem.empty() ? a.refs.size() : a.structs.size()) * stride(a);
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
		missing.bytes[14] = 0x77; // a partition no db holds
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
	// The fields of one object (instance data or array element) at p_at, base types first.
	std::function<void(const SynType &, const SynValues &, size_t)> write = [&](const SynType &p_type, const SynValues &p_values, size_t p_at) {
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
			} else if (f.kind == 'a' || f.kind == 'v') {
				const std::string elem = f.kind == 'a' ? std::string() : f.elem;
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
		const size_t count = a.elem.empty() ? a.refs.size() : a.structs.size();
		const size_t rep = array_repeaters_at + i * 12;
		put32(b, rep, static_cast<uint32_t>(array_at + 4));
		put32(b, rep + 4, static_cast<uint32_t>(count));
		put32(b, rep + 8, array_complex(a.elem));
		put32(b, arrays_at + array_at, static_cast<uint32_t>(count));
		for (size_t k = 0; k < count; ++k) {
			const size_t element_at = arrays_at + array_at + 4 + k * stride(a);
			if (a.elem.empty()) {
				put32(b, element_at, a.refs[k]);
			} else {
				write(types[type_at(a.elem)], a.structs[k], element_at);
			}
		}
		array_at += 4 + count * stride(a);
	}
	return b;
}

// Parses and inserts a synthetic partition; null (and a failed check) when it does not parse.
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

// A hostile case must end with an error well inside this (unbudgeted, each would run for minutes to
// hours); generous for the ASan build on a busy machine.
constexpr double kFastSeconds = 20.0;

bool budget_error(const Error &p_error) {
	return p_error.message.find("work budget of") != std::string::npos;
}

// The world-walk types every LogicWorld case below uses.
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

// One world part whose Objects hold p_k references to one WorldPartReferenceObjectData, whose part
// holds p_n entries that never yield an object (null, or imports no db holds).
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

} // namespace

TEST_SUITE("gameplay_unit") {
	TEST_CASE("gameplay: LogicWorld skips world parts that contain themselves") {
		// Part 0 holds two references back to itself; part 1 <-> part 2 reference each other.
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
		CHECK(world->stats().cycles == 3); // 0 -> 0 twice, 0 -> 1 -> 2 -> 1 once
		CHECK(world->stats().depth_limited == 0);
		CHECK(world->stats().walk_steps < 100);
	}

	TEST_CASE("gameplay: LogicWorld walk budget stops a nested world-part fan-out") {
		// 40 distinct parts, each referencing the next twice: 2^39 visits without a budget.
		EbxDb db;
		const EbxPartition &p = insert_graph(db, fan_chain(40, 2));
		LogicWorldOptions options;
		options.max_walk_steps = 100000;
		Result<LogicWorld> world = LogicWorld::build(db, p.primary(), options);
		REQUIRE_FALSE(world.ok());
		CHECK(world.error().message.find("walk exceeds 100000 steps") != std::string::npos);
		// The default budget also ends it with an error (in about two million steps).
		Result<LogicWorld> by_default = LogicWorld::build(db, p.primary(), {});
		CHECK_FALSE(by_default.ok());
		// A short fan-out stays inside the budget and completes.
		EbxDb small_db;
		const EbxPartition &small = insert_graph(small_db, fan_chain(6, 2));
		Result<LogicWorld> fine = LogicWorld::build(small_db, small.primary(), {});
		REQUIRE(fine.ok());
		CHECK(fine->stats().cycles == 0);
	}

	TEST_CASE("gameplay: LogicWorld walk budget counts null Objects entries") {
		// One part whose Objects hold 120000 null refs: they yield no object but are scanned, so they
		// must be charged to the budget. (The EBX parser already rejects internal refs to no
		// instance, so unresolvable entries only come from imports and take the same path.)
		std::vector<SynthNode> nodes(1);
		for (uint32_t k = 0; k < 120000; ++k) {
			nodes[0].objects.push_back(0xFFFFFFFFu); // written as 0: a null ref
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

		// The same leaf at the end of a fan-out (2^21 leaf visits) ends after a few leaf scans
		// instead of re-scanning the array on every visit. Every walk step is also charged to the
		// total work budget, whose default (1M units) is below the default walk cap (2M steps), so
		// the budget is what ends it.
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
		// Every reference is an Excluded query of at least one resolver step; with a query budget of
		// 1 the walk must fail rather than fall back to the stored Excluded field.
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
		// Every later charge fails, even one of zero units, and nothing more is added.
		CHECK_FALSE(budget.charge(0).ok());
		CHECK_FALSE(budget.take(0));
		CHECK_FALSE(budget.status().ok());
		CHECK(budget.used() == 10);
		CHECK(WorkBudget(5).charge_bytes(4 * WorkBudget::kBytesPerUnit).ok());
		CHECK_FALSE(WorkBudget(4).charge_bytes(4 * WorkBudget::kBytesPerUnit).ok());
	}

	TEST_CASE("gameplay: round 1 repro - world-part fan-out ends on the total budget") {
		// 40 parts referencing the next twice (2^39 visits unbounded), with the walk cap lifted so the
		// total budget alone must end it.
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
		// 22 parts with fan-out 2, 100000 nulls in the leaf: 2^21 leaf scans unbounded (the round 2
		// probe took 95.6 s for 10000 nulls).
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
		// One part whose Objects hold K refs to one SubWorldReferenceObjectData (AutoLoad) whose
		// InclusionSettings.Settings hold N nulls: K * N = 1e10 scans unbounded (the round 3 probe
		// was killed after 120 s at K = N = 100000, about 2 walk steps per reference).
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
		// The generic pattern on the walk: K entries -> one reference -> one part of N entries that
		// yield nothing (null refs, or imports of a partition no db holds).
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
		// A small one completes and reports what it charged.
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
		// Distinct blueprints that share one PropertyConnections array (EBX lets any number of
		// instances name the same array): each connection set costs N, K times.
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
		// Every reference's Excluded is fed from a part's Excluded, which the part feeds into itself
		// twice (and does not have as a field): each Excluded query branches 2^16 deep and stops at
		// kResolveDepth or kQuerySteps; with the per-walk caps lifted, K of them must end on the budget.
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
		// With the default caps the query cap or the budget ends it, also fast.
		Result<LogicWorld> by_default = LogicWorld::build(db, p->primary(), {});
		CHECK_FALSE(by_default.ok());

		// Queries on a world that builds: a loop on an object the walk never asks about. Each query
		// stops at kQuerySteps; the shared budget runs out after a few and every later query returns
		// at once (nullopt), with the budget reported exhausted.
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
		// A level whose one sub-world holds K TeamEntityData of one TeamData whose
		// SoldierCustomization lists N entries that never resolve: K * N = 1e9 unbounded.
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
		// collect_components (4096 components) and the vehicle Layout walk (8192) visit a component
		// once per listing, and each visit scans its Components array.
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
		// One budget shared by several reads ends them all: the next read fails at once.
		WorkBudget shared(1000);
		CHECK_FALSE(read_kit_def(db, "gameplay/kits/syn", &none, &shared).ok());
		CHECK(shared.exhausted());
		CHECK_FALSE(read_kit_def(db, "gameplay/kits/syn", &none, &shared).ok());
		CHECK(shared.used() <= 1000);
	}

	TEST_CASE("gameplay: wide type chains pay for their field lookups") {
		// A lookup of a field a type lacks scans every field of the chain: up to 255 fields x 32
		// base types. References of such a type (Excluded missing, so the walk scans all 8160 field
		// descriptors per object) cost their width in units, so a budget buys far fewer of them.
		SynPartition s;
		s.types = world_types();
		std::string base;
		for (int level = 0; level < 30; ++level) { // 30 bases + itself + DataContainer: the deepest chain the parser accepts
			SynType wide{ "Wide" + std::to_string(level), {}, false, base };
			for (int k = 0; k < 254; ++k) {
				wide.fields.push_back({ "Pad" + std::to_string(k), 'i', {} });
			}
			s.types.push_back(wide);
			base = wide.name;
		}
		// Most derived: the reference type itself, without an Excluded field.
		s.types.push_back({ "WorldPartReferenceObjectData", { { "Blueprint", 'r' } }, false, base });
		s.types.erase(s.types.begin() + 1); // drop the plain WorldPartReferenceObjectData
		const uint32_t root = s.add({ "WorldPartData", {} });
		const uint32_t part = s.add({ "WorldPartData", {} });
		const uint32_t reference = s.add({ "WorldPartReferenceObjectData", { { "Blueprint", SynPartition::ref(part) } } });
		s.instances[root].values = { { "Objects", s.add_refs(10, SynPartition::ref(reference)) } };
		EbxDb db;
		const EbxPartition *p = insert_syn(db, "levels/synthetic/wide", s);
		if (p == nullptr) {
			return;
		}
		const std::optional<EbxInstance> wide = p->instance(reference);
		REQUIRE(wide.has_value());
		CHECK(WorkBudget::type_width(wide->data()) == 30 * 255 + 2);
		Result<LogicWorld> small = LogicWorld::build(db, p->primary(), {});
		REQUIRE(small.ok());
		CHECK(small->budget().used() >= 10 * (WorkBudget::type_width(wide->data()) / WorkBudget::kFieldsPerUnit));

		// 200000 of them: the default budget ends the walk after a few thousand.
		s.arrays[0].refs.assign(200000, SynPartition::ref(reference));
		s.seed = 2;
		EbxDb many_db;
		const EbxPartition *many = insert_syn(many_db, "levels/synthetic/wide", s);
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
		// Without the progression tables nothing is a known default.
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
		// Malformed or unknown: kept as an untyped constant, never a number.
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

TEST_SUITE("gameplay") {
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
		// 10 crates are placed (a 5th base included); which ones run is decided by logic at runtime.
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
		// Starting loadouts (research/topics/02-soldier-kits-modes.md §2.2, the bold column entries):
		// per slot the weapon partition, "" for an unlock without a weapon (nogadget), nullptr for no
		// default (every entry gated, or an empty slot).
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
			// The razor knife is listed first but is a licence unlock (mpstaticunlocks).
			check_slot(*kit, WeaponSlot::Knife, "weapons/knife/knife");
			const KitSlot *knives = kit->slot(WeaponSlot::Knife);
			REQUIRE(knives != nullptr);
			REQUIRE(knives->unlocks.size() == 2);
			CHECK(knives->unlocks[0].unlock == "weapons/xp2_knife_razorblade/u_knife_razor");
			CHECK(knives->unlocks[0].gate == UnlockGate::Licence);
		}
		// Without a progression table read here, read_kit_def reads one itself.
		Result<KitDef> own = read_kit_def(*lib, "gameplay/kits/usassault");
		REQUIRE(own.ok());
		REQUIRE(own->default_unlock(WeaponSlot::Knife) != nullptr);
		CHECK(own->default_unlock(WeaponSlot::Knife)->weapon == "weapons/knife/knife");
		// Underslung ammo variants carry an Extra unlock, are in no progression table and are never a
		// default; the two scoped crossbows are variants too (award-gated).
		const KitSlot *launchers = own->slot(WeaponSlot::Gadget1);
		REQUIRE(launchers != nullptr);
		size_t variants = 0, free_variants = 0;
		for (const KitUnlock &u : launchers->unlocks) {
			variants += u.variant ? 1 : 0;
			free_variants += (u.variant && u.gate == UnlockGate::Free) ? 1 : 0;
		}
		CHECK(variants == 8);
		CHECK(free_variants == 6); // M320 SHG/SMK/LVG, M26 flechette/frag/slug
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

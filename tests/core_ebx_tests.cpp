// EBX reader: a synthetic partition built here (suite "containers", no install needed) and the
// retail install (suite "install", read-only, skipped without an install). The install cases
// assert counts and numbers only; the reference values are the ebx-dump probe's (IW4L Rust
// readers) output on the same install, patch v7 + Xpack1-5.
#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace bf3::core;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes &r_out, size_t p_at, uint32_t p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 4);
}
void put16(Bytes &r_out, size_t p_at, uint16_t p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 2);
}
void putf(Bytes &r_out, size_t p_at, float p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 4);
}
void put_guid(Bytes &r_out, size_t p_at, const Guid &p_guid) {
	std::memcpy(r_out.data() + p_at, p_guid.bytes.data(), 16);
}
size_t align16(size_t p_at) {
	return (p_at + 15) & ~size_t(15);
}

Guid guid_of(const char *p_text) {
	return *Guid::parse(p_text);
}

const Guid kPartition = guid_of("88d61a88-ecc8-11df-b39c-fd673456481d");
const Guid kThing0 = guid_of("11111111-2222-3333-4444-555555555555");
const Guid kThing1 = guid_of("66666666-7777-8888-9999-aaaaaaaaaaaa");
const Guid kOtherPartition = guid_of("0a0b0c0d-0e0f-1011-1213-141516171819");
const Guid kOtherInstance = guid_of("deadbeef-0000-1111-2222-333344445555");

struct SynthLayout {
	size_t fields_at, complexes_at, instances_at, strings_at, arrays_at, array_repeaters_at;
};

// A small partition read through its own descriptors:
//   DataContainer (class, size 8)
//   Thing : DataContainer (class, size 48): Name CString@8, Value Float32@12, Kind Color@16,
//           Items array-of-Float32@20, Other Class@24, Flag Boolean@28, Pos Vec3@32
//   array (member Float32), Color enum { Red = 0, Green = 1 }, Vec3 { x, y, z } (value type, 16)
// Two Thing instances; one import; strings "hello"; array 0 = [1.5, 2.5, -0.0], array 1 = [].
Bytes build_partition(const Guid &p_partition, const Guid &p_i0, const Guid &p_i1, SynthLayout *r_layout = nullptr) {
	const char *names[] = { "DataContainer", "$", "Thing", "Name", "Value", "Kind", "Items", "Other", "Flag", "Pos", "array", "member", "Color", "Red",
		"Green", "Vec3", "x", "y", "z" };
	std::string name_table;
	for (const char *n : names) {
		name_table += n;
		name_table += '\0';
	}
	struct F {
		const char *name;
		uint8_t code;
		uint16_t ref;
		int32_t offset;
	};
	const F fields[] = {
		{ "$", ebx_code::kVoid, 0, 0 }, // Thing (0..8)
		{ "Name", ebx_code::kCString, 0, 8 },
		{ "Value", ebx_code::kFloat32, 0, 12 },
		{ "Kind", ebx_code::kEnum, 3, 16 },
		{ "Items", ebx_code::kArray, 2, 20 },
		{ "Other", ebx_code::kClass, 0, 24 },
		{ "Flag", ebx_code::kBoolean, 0, 28 },
		{ "Pos", ebx_code::kValueType, 4, 32 },
		{ "member", ebx_code::kFloat32, 0, 0 }, // array (8)
		{ "Red", ebx_code::kVoid, 0, 0 }, // Color (9, 10)
		{ "Green", ebx_code::kVoid, 0, 1 },
		{ "x", ebx_code::kFloat32, 0, 0 }, // Vec3 (11..13)
		{ "y", ebx_code::kFloat32, 0, 4 },
		{ "z", ebx_code::kFloat32, 0, 8 },
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
		{ "Thing", 0, 8, ebx_code::kClass, 48 },
		{ "array", 8, 1, ebx_code::kArray, 4 },
		{ "Color", 9, 2, ebx_code::kEnum, 4 },
		{ "Vec3", 11, 3, ebx_code::kValueType, 16 },
	};
	const std::string strings("hello\0", 6);
	const size_t names_at = 80 + 32;
	const size_t fields_at = names_at + name_table.size();
	const size_t complexes_at = align16(fields_at + std::size(fields) * 16);
	const size_t instance_repeaters_at = complexes_at + std::size(complexes) * 16;
	const size_t array_repeaters_at = align16(instance_repeaters_at + 12);
	const size_t meta_size = align16(array_repeaters_at + 2 * 12);
	const size_t strings_at = meta_size;
	const size_t instances_at = strings_at + strings.size();
	const size_t instance_payload = 2 * (16 + 48);
	const size_t arrays_at = instances_at + instance_payload;
	const size_t total = arrays_at + 4 + 12;

	Bytes b(total, 0);
	put32(b, 0x00, 0x0FB2D1CE);
	put32(b, 0x04, static_cast<uint32_t>(meta_size));
	put32(b, 0x08, static_cast<uint32_t>(total - meta_size));
	put32(b, 0x0C, 1);
	put32(b, 0x14, 1);
	put32(b, 0x18, static_cast<uint32_t>(std::size(complexes)));
	put32(b, 0x1C, static_cast<uint32_t>(std::size(fields)));
	put32(b, 0x20, static_cast<uint32_t>(name_table.size()));
	put32(b, 0x24, static_cast<uint32_t>(strings.size()));
	put32(b, 0x28, 2);
	put32(b, 0x2C, static_cast<uint32_t>(instance_payload));
	put_guid(b, 0x30, p_partition);
	put_guid(b, 0x40, p_i0);
	put_guid(b, 80, kOtherPartition);
	put_guid(b, 96, kOtherInstance);
	std::memcpy(b.data() + names_at, name_table.data(), name_table.size());
	for (size_t i = 0; i < std::size(fields); ++i) {
		const size_t at = fields_at + i * 16;
		put32(b, at, ebx_name_hash(fields[i].name));
		put16(b, at + 4, static_cast<uint16_t>(fields[i].code << 4));
		put16(b, at + 6, fields[i].ref);
		put32(b, at + 8, static_cast<uint32_t>(fields[i].offset));
	}
	for (size_t i = 0; i < std::size(complexes); ++i) {
		const size_t at = complexes_at + i * 16;
		put32(b, at, ebx_name_hash(complexes[i].name));
		put32(b, at + 4, complexes[i].field_start);
		b[at + 8] = complexes[i].count;
		b[at + 9] = 4;
		put16(b, at + 10, static_cast<uint16_t>(complexes[i].code << 4));
		put16(b, at + 12, complexes[i].size);
	}
	put32(b, instance_repeaters_at, 0);
	put32(b, instance_repeaters_at + 4, 2);
	put32(b, instance_repeaters_at + 8, 1);
	put32(b, array_repeaters_at, 4); // array 0: count at section+0, elements at section+4
	put32(b, array_repeaters_at + 4, 3);
	put32(b, array_repeaters_at + 8, 2);
	put32(b, array_repeaters_at + 12, 0); // array 1: empty
	put32(b, array_repeaters_at + 16, 0);
	put32(b, array_repeaters_at + 20, 2);
	std::memcpy(b.data() + strings_at, strings.data(), strings.size());
	// Instance 0.
	size_t at = instances_at;
	put_guid(b, at, p_i0);
	size_t d = at + 16;
	put32(b, d + 8, 0); // "hello"
	putf(b, d + 12, 0.1f);
	put32(b, d + 16, 1); // Green
	put32(b, d + 20, 0); // array 0
	put32(b, d + 24, 0x80000000u); // import 0
	b[d + 28] = 1;
	putf(b, d + 32, 1.0f);
	putf(b, d + 36, 2.0f);
	putf(b, d + 40, 3.0f);
	// Instance 1.
	at = d + 48;
	put_guid(b, at, p_i1);
	d = at + 16;
	put32(b, d + 8, 0xFFFFFFFFu); // null string
	putf(b, d + 12, -2.0f);
	put32(b, d + 16, 7); // no enumerator
	put32(b, d + 20, 1); // array 1 (empty)
	put32(b, d + 24, 1); // internal: instance 0
	// Arrays.
	put32(b, arrays_at, 3);
	putf(b, arrays_at + 4, 1.5f);
	putf(b, arrays_at + 8, 2.5f);
	putf(b, arrays_at + 12, -0.0f);
	if (r_layout != nullptr) {
		*r_layout = SynthLayout{ fields_at, complexes_at, instances_at, strings_at, arrays_at, array_repeaters_at };
	}
	return b;
}

std::string parse_error(Bytes p_bytes) {
	Result<EbxPartition> p = EbxPartition::parse(std::move(p_bytes));
	return p.ok() ? std::string("ok") : p.error().message;
}

bool contains(const std::string &p_text, const char *p_part) {
	return p_text.find(p_part) != std::string::npos;
}

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

std::unique_ptr<EbxLibrary> open_library(const Mounted &p_m, const std::string &p_spec) {
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_m.install, expand_superbundles(*p_m.install, p_spec));
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	return std::move(*lib);
}

float f32_at(const EbxInstance &p_inst, const char *p_path) {
	const std::optional<EbxValue> v = p_inst.get_path(p_path);
	REQUIRE_MESSAGE(v.has_value(), p_path);
	REQUIRE(v->as_f32().has_value());
	return *v->as_f32();
}

} // namespace

TEST_SUITE("containers") {
	TEST_CASE("ebx: name hash is case-sensitive djb2-xor") {
		CHECK(ebx_name_hash("") == 5381u);
		CHECK(ebx_name_hash("a") == ((5381u * 33u) ^ 'a'));
		CHECK(ebx_name_hash("a") == asset_name_hash("A"));
		CHECK(ebx_name_hash("A") != asset_name_hash("A"));
	}

	TEST_CASE("ebx: synthetic partition through its own descriptors") {
		Result<EbxPartition> parsed = EbxPartition::parse(build_partition(kPartition, kThing0, kThing1));
		REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
		const EbxPartition &p = *parsed;
		CHECK(p.guid() == kPartition);
		CHECK(p.primary_guid() == kThing0);
		CHECK(p.instance_count() == 2);
		CHECK(p.type_count() == 5);
		CHECK(p.array_count() == 2);
		REQUIRE(p.imports().size() == 1);
		CHECK(p.imports()[0].partition == kOtherPartition);

		const EbxInstance t0 = p.primary();
		CHECK(t0.is_primary());
		CHECK(t0.type_name() == "Thing");
		CHECK(t0.type_chain() == std::vector<std::string_view>{ "Thing", "DataContainer" });
		CHECK(t0.is_a("DataContainer"));
		CHECK_FALSE(t0.is_a("Vec3"));
		CHECK(p.instances_of("DataContainer").size() == 2);
		CHECK(p.instances_of("Thing").size() == 2);
		CHECK(p.instance_by_guid(kThing1)->index() == 1);
		CHECK_FALSE(p.instance_by_guid(kOtherInstance).has_value());

		CHECK(t0.get("Name")->as_str() == std::optional<std::string_view>("hello"));
		CHECK(*t0.get("Value")->as_f32() == 0.1f);
		CHECK(t0.get("Kind")->enum_name() == std::optional<std::string_view>("Green"));
		CHECK(*t0.get("Flag")->as_bool());
		CHECK(t0.get("Pos")->as_vec3() == std::optional<std::array<float, 3>>({ 1.0f, 2.0f, 3.0f }));
		CHECK(f32_at(t0, "Pos.y") == 2.0f);
		const EbxArray items = *t0.get("Items")->as_array();
		REQUIRE(items.size() == 3);
		CHECK(*items.get(0)->as_f32() == 1.5f);
		CHECK(f32_at(t0, "Items[1]") == 2.5f);
		CHECK(std::signbit(*items.get(2)->as_f32()));
		CHECK_FALSE(items.get(3).has_value());
		CHECK_FALSE(t0.get_path("Items[3]").has_value());
		// Range-for over a temporary array view (C++20 ends the optional's life after range-init),
		// and iterators that outlive the EbxArray they came from: both must stay valid.
		std::vector<float> seen;
		for (const EbxValue e : *t0.get("Items")->as_array()) {
			seen.push_back(*e.as_f32());
		}
		REQUIRE(seen.size() == 3);
		CHECK(seen[0] == 1.5f);
		CHECK(seen[1] == 2.5f);
		CHECK(std::signbit(seen[2]));
		EbxArrayIterator it = t0.get("Items")->as_array()->begin();
		const EbxArrayIterator last = t0.get("Items")->as_array()->end();
		size_t walked = 0;
		for (; it != last; ++it) {
			CHECK((*it).as_f32().has_value());
			++walked;
		}
		CHECK(walked == 3);
		CHECK(t0.get("Items")->as_array() != std::nullopt);
		CHECK_FALSE(t0.get("Name")->as_array().has_value());
		CHECK((*t0.get("Name")->as_array()).empty()); // empty dereferences to an empty array
		CHECK_FALSE(t0.get("Missing").has_value());
		CHECK_FALSE(t0.get("name").has_value()); // field names are case-sensitive
		const EbxRef other = *t0.get("Other")->as_ref();
		CHECK(other.kind == EbxRef::Kind::External);
		CHECK(other.partition == kOtherPartition);
		CHECK(other.instance == kOtherInstance);
		CHECK_FALSE(p.resolve(other).has_value());

		const EbxInstance t1 = *p.instance(1);
		CHECK_FALSE(t1.is_primary());
		CHECK(t1.get("Name")->is_null_string());
		CHECK(t1.get("Name")->as_str() == std::optional<std::string_view>(""));
		CHECK_FALSE(t1.get("Kind")->enum_name().has_value());
		CHECK(t1.get("Kind")->as_i64() == std::optional<int64_t>(7));
		CHECK(t1.get("Items")->as_array()->empty());
		const EbxRef internal = *t1.get("Other")->as_ref();
		CHECK(internal == EbxRef::internal(0));
		CHECK(p.resolve(internal) == t0);

		// Every field, base types first, declaration order, '$' skipped.
		std::vector<std::string_view> names;
		for (const EbxField &f : t0.fields()) {
			names.push_back(f.name);
		}
		CHECK(names == std::vector<std::string_view>{ "Name", "Value", "Kind", "Items", "Other", "Flag", "Pos" });
		size_t vec_fields = 0;
		for (const EbxField &f : t0.get("Pos")->as_struct()->fields()) {
			vec_fields += f.value.as_f32().has_value() ? 1 : 0;
		}
		CHECK(vec_fields == 3);
	}

	TEST_CASE("ebx: database lookup, first wins, cross-partition references") {
		EbxDb db;
		CHECK(db.parse_insert("Things/A", build_partition(kPartition, kThing0, kThing1)).value());
		// Same name (any case) or same GUID: dropped.
		CHECK_FALSE(db.parse_insert("things/a", build_partition(kOtherPartition, kOtherInstance, kThing1)).value());
		CHECK_FALSE(db.parse_insert("things/b", build_partition(kPartition, kThing0, kThing1)).value());
		CHECK(db.parse_insert("things/b", build_partition(kOtherPartition, kOtherInstance, kThing1)).value());
		CHECK(db.size() == 2);
		CHECK(db.name_of(kPartition) == std::optional<std::string_view>("things/a"));
		REQUIRE(db.partition_by_name("THINGS/A") != nullptr);
		const EbxPartition &a = *db.partition_by_name("things/a");
		const std::optional<EbxInstance> target = db.follow(a.primary().data(), "Other");
		REQUIRE(target.has_value());
		CHECK(target->guid() == kOtherInstance);
		CHECK(&target->partition() == db.partition(kOtherPartition));
		CHECK(db.follow(a.instance(1)->data(), "Other")->guid() == kThing0);
		CHECK_FALSE(db.parse_insert("bad", Bytes(10, 0)).ok());
	}

	TEST_CASE("ebx: malformed partitions are rejected with the reader's messages") {
		SynthLayout l{};
		const Bytes good = build_partition(kPartition, kThing0, kThing1, &l);
		CHECK(parse_error(good) == "ok");
		Bytes b = good;
		put32(b, 0, 0x12345678);
		CHECK(parse_error(b) == "not an EBX partition (magic 0x12345678)");
		b = good;
		put32(b, 0, 0xCED1B20F);
		CHECK(contains(parse_error(b), "big-endian"));
		b = good;
		b.pop_back();
		CHECK(contains(parse_error(b), "do not add up to the partition length"));
		CHECK(parse_error(Bytes(good.begin(), good.begin() + 40)) == "Header runs past the end of the partition");
		b = good;
		put32(b, l.arrays_at, 4); // stored count differs from the repeater
		CHECK(parse_error(b) == "array 0 has 3 elements but 4 is stored before its data");
		b = good;
		put32(b, l.instances_at + 16 + 24, 3); // internal reference past the 2 instances
		CHECK(parse_error(b) == "reference 0x00000003 names no instance or import");
		b = good;
		put32(b, l.instances_at + 16 + 24, 0x80000001u); // import 1 of 1
		CHECK(parse_error(b) == "reference 0x80000001 names no instance or import");
		b = good;
		put32(b, l.instances_at + 16 + 8, 6); // string offset at the end of the table
		CHECK(parse_error(b) == "string offset 0x6 is outside the string table");
		b = good;
		put32(b, l.instances_at + 16 + 20, 2); // array index out of range
		CHECK(parse_error(b) == "array 2 is out of range");
		b = good;
		put16(b, l.fields_at + 2 * 16 + 4, static_cast<uint16_t>(23 << 4)); // Value: type code 23
		CHECK(parse_error(b) == "field `Thing.Value` has unsupported type code 23");
		b = good;
		put32(b, l.fields_at + 7 * 16 + 8, 40); // Pos at 40 + 16 > 48
		CHECK(parse_error(b) == "field `Thing.Pos` lies outside its type");
		b = good;
		put32(b, l.fields_at + 1 * 16, ebx_name_hash("Nope"));
		CHECK(contains(parse_error(b), "is not in the name table"));
		b = good;
		put32(b, 0x40, 0xFFFFFFFFu); // primary GUID not an instance
		CHECK(contains(parse_error(b), "is not in the partition"));
		b = good;
		put32(b, l.instances_at + 16 + 8, 1); // offset 1 inside "hello" is a valid string
		Result<EbxPartition> p = EbxPartition::parse(b);
		REQUIRE(p.ok());
		CHECK(p->primary().get("Name")->as_str() == std::optional<std::string_view>("ello"));
	}

	// H hygiene: the type tables are interned by the descriptor bytes.
	TEST_CASE("ebx: partitions of one layout share their type tables; the last one frees them") {
		const size_t live_before = ebx_live_type_layouts();
		SynthLayout l{};
		const Bytes good = build_partition(kPartition, kThing0, kThing1, &l);
		Bytes moved = good;
		put32(moved, l.fields_at + 2 * 16 + 8, 44); // Thing.Value at 44 (inside the type, over Pos.z): another layout
		{
			Result<EbxPartition> a = EbxPartition::parse(good);
			Result<EbxPartition> b = EbxPartition::parse(build_partition(kOtherPartition, kOtherInstance, kThing1));
			Result<EbxPartition> c = EbxPartition::parse(moved);
			REQUIRE(a.ok());
			REQUIRE(b.ok());
			REQUIRE(c.ok());
			CHECK(a->memory_use().types_id != nullptr);
			CHECK(a->memory_use().types_id == b->memory_use().types_id);
			CHECK(&a->field_descs() == &b->field_descs());
			CHECK(a->memory_use().types_id != c->memory_use().types_id);
			CHECK(ebx_live_type_layouts() == live_before + 2);
			CHECK(a->primary().get("Value")->as_f32() == b->primary().get("Value")->as_f32());
			CHECK(c->primary().get("Value").has_value()); // read at 44 now
			// A layout error is still found while an equal-looking good layout is alive.
			Bytes bad = good;
			put16(bad, l.fields_at + 2 * 16 + 4, static_cast<uint16_t>(23 << 4));
			CHECK(parse_error(bad) == "field `Thing.Value` has unsupported type code 23");
			CHECK(ebx_live_type_layouts() == live_before + 2);
			// Moving a partition keeps its views of the shared tables.
			EbxPartition kept = std::move(*a);
			CHECK(kept.primary().get("Name")->as_str() == std::optional<std::string_view>("hello"));
			CHECK(kept.imports().size() == 1);
			CHECK(kept.imports()[0].partition == kOtherPartition);
		}
		CHECK(ebx_live_type_layouts() == live_before);
	}

	TEST_CASE("ebx: 8 threads parsing two layouts at once agree on one copy per layout") {
		const size_t live_before = ebx_live_type_layouts();
		SynthLayout l{};
		const Bytes good = build_partition(kPartition, kThing0, kThing1, &l);
		Bytes moved = good;
		put32(moved, l.fields_at + 2 * 16 + 8, 44);
		std::vector<std::vector<EbxPartition>> kept(8);
		std::vector<std::thread> threads;
		std::atomic<int> failures{ 0 };
		for (int t = 0; t < 8; ++t) {
			threads.emplace_back([&, t] {
				for (int i = 0; i < 200; ++i) {
					Result<EbxPartition> p = EbxPartition::parse(((i + t) % 2) ? good : moved);
					if (!p.ok()) {
						failures.fetch_add(1);
						continue;
					}
					if (i % 4 == 0) {
						kept[t].push_back(std::move(*p)); // some live on, the rest go at once
					}
				}
			});
		}
		for (std::thread &t : threads) {
			t.join();
		}
		CHECK(failures.load() == 0);
		std::set<const void *> ids;
		for (const std::vector<EbxPartition> &v : kept) {
			for (const EbxPartition &p : v) {
				ids.insert(p.memory_use().types_id);
			}
		}
		CHECK(ids.size() == 2);
		CHECK(ebx_live_type_layouts() == live_before + 2);
		kept.clear();
		CHECK(ebx_live_type_layouts() == live_before);
	}
}

TEST_SUITE("install") {
	TEST_CASE("ebx: MP_Subway partitions and M16A4 values") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> lib = open_library(m, "Win32/Levels/MP_Subway/MP_Subway");
		CHECK(lib->catalog_size() == 12524);

		// Lazy: parse on first use, GUID lookups through the 80-byte headers.
		const EbxPartition *weapon = lib->partition_by_name("Weapons/M16A4/M16A4");
		REQUIRE(weapon != nullptr);
		CHECK(weapon->guid() == guid_of("ea96401d-f166-11df-a2f6-9139d3229269"));
		CHECK(weapon->primary().type_name() == "SoldierWeaponBlueprint");
		CHECK(weapon->instance_count() == 259);
		CHECK(lib->name_of(weapon->guid()) == std::optional<std::string_view>("weapons/m16a4/m16a4"));
		size_t firing = 0;
		for (size_t i = 0; i < weapon->instance_count(); ++i) {
			const EbxInstance inst = *weapon->instance(i);
			if (inst.type_name() != "FiringFunctionData") {
				continue;
			}
			++firing;
			CHECK(f32_at(inst, "FireLogic.RateOfFire") == 800.0f);
		}
		CHECK(firing == 3);
		const std::optional<EbxInstance> ffd = weapon->instance_by_guid(guid_of("1cae0e91-1ec1-48b9-8ffe-90eda105ffb4"));
		REQUIRE(ffd.has_value());
		CHECK(ffd->get_path("Ammo.MagazineCapacity")->as_i64() == std::optional<int64_t>(31));
		CHECK(f32_at(*ffd, "Shot.InitialSpeed.z") == 650.0f);
		const std::optional<EbxInstance> bullet = lib->follow(*ffd->get_path("Shot")->as_struct(), "ProjectileData");
		REQUIRE(bullet.has_value());
		CHECK(bullet->type_name() == "BulletEntityData");
		CHECK(bullet->guid() == guid_of("6c055c66-9145-4ecb-8c41-bbe1a6c91287"));
		CHECK(lib->name_of(bullet->partition().guid()) == std::optional<std::string_view>("weapons/common/bullets/556x45mmnato"));
		CHECK(f32_at(*bullet, "StartDamage") == 25.0f);
		CHECK(f32_at(*bullet, "EndDamage") == 18.4f);

		// Bulk: every partition parses; the probe loads the same 12,524.
		const EbxLibrary::LoadReport report = lib->load_all();
		CHECK(report.partitions == 12524);
		CHECK(report.failed == 0);
		CHECK(report.duplicate_guids == 0);
		CHECK(lib->partition_by_name("weapons/m16a4/m16a4")->guid() == weapon->guid());
		const std::optional<EbxLibrary::Origin> origin = lib->origin("weapons/m16a4/m16a4");
		REQUIRE(origin.has_value());
		CHECK(origin->superbundle == "Win32/Levels/MP_Subway/MP_Subway");
		CHECK_FALSE(origin->bundle.empty());
	}

	TEST_CASE("ebx: MP_007 soldier health") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> lib = open_library(m, "Win32/Levels/MP_007/MP_007");
		const EbxLibrary::LoadReport report = lib->load_all();
		CHECK(report.partitions == 12328);
		CHECK(report.failed == 0);
		const EbxPartition *soldier = lib->partition_by_name("characters/soldiers/mpsoldier");
		REQUIRE(soldier != nullptr);
		CHECK(soldier->primary().type_name() == "SoldierBlueprint");
		CHECK(soldier->instance_count() == 218);
		const std::vector<EbxInstance> entities = soldier->instances_of("SoldierEntityData");
		REQUIRE(entities.size() == 1);
		const EbxInstance &entity = entities[0];
		CHECK(entity.guid() == guid_of("a9ffe6b4-257f-4fe8-a950-b323b50d2112"));
		CHECK(f32_at(entity, "MaxHealth") == 100.0f);
		CHECK(f32_at(entity, "LowHealthThreshold") == 30.0f);
		CHECK(entity.get("DefaultTeam")->enum_name() == std::optional<std::string_view>("TeamNeutral"));
		const std::optional<EbxInstance> health = lib->follow(entity.data(), "HealthModule");
		REQUIRE(health.has_value());
		CHECK(health->type_name() == "VeniceSoldierHealthModuleData");
		CHECK(f32_at(*health, "RegenerationDelay") == 10.0f);
		CHECK(f32_at(*health, "RegenerationRate") == 5.0f);
		CHECK(f32_at(*health, "PostReviveHealth") == 20.0f);
		CHECK(f32_at(*health, "TimeForCorpse") == 15.0f);
	}

	TEST_CASE("ebx: every partition of @all-mp parses") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::vector<std::string> sbs = expand_superbundles(*m.install, "@all-mp");
		CHECK(sbs.size() == 33);
		std::unique_ptr<EbxLibrary> lib = open_library(m, "@all-mp");
		const EbxLibrary::LoadReport report = lib->load_all();
		CHECK(report.partitions == 50486);
		CHECK(report.failed == 0);
		CHECK(report.duplicate_guids == 0);
		for (const std::string &e : report.errors) {
			MESSAGE(e);
		}
		REQUIRE(report.superbundles.size() == 33);
		CHECK(report.superbundles[0].superbundle == "Win32/default_settings_Win32");
		CHECK(report.superbundles[0].fresh == 720);
		CHECK(report.superbundles[1].fresh == 105); // Win32/Globals
		size_t instances = 0, dup_instances = 0;
		for (const EbxLibrary::Entry &e : lib->entries()) {
			instances += e.partition->instance_count();
			dup_instances += e.partition->duplicate_instance_guids();
		}
		CHECK(instances == 795316);
		CHECK(dup_instances == 0);
	}
}

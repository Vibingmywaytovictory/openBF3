// Level traversal (bf3_core/level). Suite level_unit: transforms, interface constants and the bake
// split on synthetic values. Suite level: the retail install (read-only; exits 77 = skipped without
// one) against handoff §9 and sections/03 counts; nothing from the install is stored.
//
// Handoff §9 definitions (see README "Level traversal"): Metro TDM "10,873 placements -> 842
// meshes (762 model, 80 world), 10,571 instances" and Bazaar CQL "620 meshes, 5,783 instances" are
// the IW4L geometry builder's counts at bab7c1a, which mesh_census reproduces (Bazaar with
// legacy_variations; the current MeshVariationDatabase rule gives 613). Caspian ConquestSmall0
// "20,311 placements on 2,277 meshes, 12,725 placed hidden" is the IW engine's later world report
// ("bf3 static models: ... 12725 placed hidden for destruction"), counted after IW4L's geometry
// builder gave each destruction state's part an instance of its own, hidden until the state shows:
// a renderer count, not a traversal count. The traversal places 6,847 mesh objects there; the
// report's 20,311 - 12,725 = 7,586 shown instances exceed that by 739, which is not attributed here.
// Metro: sections/03's "Metro 702" composite placements hiding parts is Metro ConquestSmall0
// (8-RENDER-DESTRUCTION-PART probe geom_MP_Subway_ConquestSmall0.txt); TDM's is 710
// (3-CONVERTER-PART/geometry.md and geom_MP_Subway_TeamDeathMatch0.txt).
#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/connections.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>

using namespace bf3::core;
using namespace bf3::core::level;

namespace {

uint32_t bits(float p_value) {
	uint32_t b = 0;
	std::memcpy(&b, &p_value, 4);
	return b;
}

std::shared_ptr<Install> open_or_skip() {
	static InstallReader reader;
	static std::shared_ptr<Install> install;
	static bool tried = false;
	if (!tried) {
		tried = true;
		if (reader.locate({})) {
			Result<std::shared_ptr<Install>> opened = Install::open(reader);
			REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
			install = *opened;
		}
	}
	if (!install) {
		bf3::test::skip_install_case(reader.describe().c_str());
	}
	return install;
}

struct Traversed {
	std::unique_ptr<LoadedLevel> level;
	std::unique_ptr<World> world;
};

Traversed traverse(const std::shared_ptr<Install> &p_install, const char *p_level, const char *p_mode) {
	Traversed t;
	Result<std::unique_ptr<LoadedLevel>> loaded = load_level(p_install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	t.level = std::move(*loaded);
	Result<std::unique_ptr<World>> world = World::traverse(*t.level);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	t.world = std::move(*world);
	return t;
}

size_t count_mesh_objects(const World &p_world) {
	size_t n = 0;
	for (const PlacedObject &o : p_world.objects) {
		n += o.kind == ObjectKind::Mesh ? 1 : 0;
	}
	return n;
}

// Composite placements whose first health state hides parts (handoff sections/03 §2.5).
size_t composite_hiding(const LoadedLevel &p_level, const World &p_world) {
	size_t n = 0;
	for (const PlacedObject &o : p_world.objects) {
		if (o.mesh && o.mesh->asset.is_a("CompositeMeshAsset")) {
			const Result<std::optional<std::vector<uint16_t>>> parts = hidden_parts(p_level.db(), o.entity, *p_world.budget);
			REQUIRE(parts.ok());
			n += *parts && !(*parts)->empty() ? 1 : 0;
		}
	}
	return n;
}

size_t count_start_hidden(const World &p_world) {
	size_t n = 0;
	for (const PlacedObject &o : p_world.objects) {
		const Result<bool> hidden = starts_hidden(p_world, o);
		REQUIRE(hidden.ok());
		n += *hidden ? 1 : 0;
	}
	return n;
}

MeshCensus census_of(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world, const CensusOptions &p_options = {}) {
	Result<MeshCensus> census = mesh_census(p_db, p_mount, p_world, p_options);
	REQUIRE_MESSAGE(census.ok(), (census.ok() ? std::string() : census.error().message));
	return std::move(*census);
}

std::vector<CollisionPlacement> collision_of(const LoadedLevel &p_level, const World &p_world) {
	Result<std::vector<CollisionPlacement>> collision = collision_placements(p_level, p_world);
	REQUIRE_MESSAGE(collision.ok(), (collision.ok() ? std::string() : collision.error().message));
	return std::move(*collision);
}

// A partition set with some partitions replaced: lookups try the overlay first, then the base.
class OverlayDb final : public EbxResolver {
public:
	explicit OverlayDb(const EbxResolver &p_base) :
			base_(p_base) {}

	void add(const std::string &p_name, std::unique_ptr<EbxPartition> p_partition) {
		const uint32_t at = uint32_t(partitions_.size());
		by_guid_.emplace(p_partition->guid(), at);
		by_name_.emplace(p_name, at);
		partitions_.emplace_back(p_name, std::move(p_partition));
	}
	const EbxPartition *partition(const Guid &p_guid) const override {
		const auto found = by_guid_.find(p_guid);
		return found != by_guid_.end() ? partitions_[found->second].second.get() : base_.partition(p_guid);
	}
	const EbxPartition *partition_by_name(std::string_view p_name) const override {
		const auto found = by_name_.find(to_lower_ascii(p_name));
		return found != by_name_.end() ? partitions_[found->second].second.get() : base_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const Guid &p_guid) const override {
		const auto found = by_guid_.find(p_guid);
		return found != by_guid_.end() ? std::optional<std::string_view>(partitions_[found->second].first) : base_.name_of(p_guid);
	}

private:
	const EbxResolver &base_;
	std::vector<std::pair<std::string, std::unique_ptr<EbxPartition>>> partitions_;
	std::unordered_map<Guid, uint32_t, GuidHash> by_guid_;
	std::unordered_map<std::string, uint32_t> by_name_;
};

// A hostile partition read through its own descriptors: a WorldPartData (the primary) whose
// Objects list p_objects entries, each an internal reference to instance 1 (a
// WorldPartReferenceObjectData whose Blueprint is the WorldPartData itself) or, for 2, to instance 2
// (a StaticModelGroupEntityData whose MemberDatas holds p_members members that place nothing:
// no InstanceCount, MemberType or MeshEntityType).
std::vector<uint8_t> self_listing_part(const std::vector<uint32_t> &p_objects, uint32_t p_members = 0) {
	using Bytes = std::vector<uint8_t>;
	const auto put32 = [](Bytes &r_out, size_t p_at, uint32_t p_value) { std::memcpy(r_out.data() + p_at, &p_value, 4); };
	const auto put16 = [](Bytes &r_out, size_t p_at, uint16_t p_value) { std::memcpy(r_out.data() + p_at, &p_value, 2); };
	const auto align16 = [](size_t p_at) { return (p_at + 15) & ~size_t(15); };
	const char *names[] = { "DataContainer", "$", "WorldPartData", "Objects", "WorldPartReferenceObjectData", "Blueprint", "array", "member",
		"StaticModelGroupEntityData", "MemberDatas", "StaticModelGroupMemberData", "Unused" };
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
		{ "Objects", ebx_code::kArray, 2, 8 },
		{ "member", ebx_code::kClass, 0, 0 }, // array (2)
		{ "$", ebx_code::kVoid, 0, 0 }, // WorldPartReferenceObjectData (3, 4)
		{ "Blueprint", ebx_code::kClass, 0, 8 },
		{ "$", ebx_code::kVoid, 0, 0 }, // StaticModelGroupEntityData (5, 6)
		{ "MemberDatas", ebx_code::kArray, 5, 8 },
		{ "member", ebx_code::kValueType, 6, 0 }, // array (7)
		{ "Unused", ebx_code::kUInt32, 0, 0 }, // StaticModelGroupMemberData (8)
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
		{ "array", 2, 1, ebx_code::kArray, 4 },
		{ "WorldPartReferenceObjectData", 3, 2, ebx_code::kClass, 16 },
		{ "StaticModelGroupEntityData", 5, 2, ebx_code::kClass, 16 },
		{ "array", 7, 1, ebx_code::kArray, 4 },
		{ "StaticModelGroupMemberData", 8, 1, ebx_code::kValueType, 4 },
	};
	const uint32_t repeaters[3][3] = { { 0, 1, 1 }, { 0, 1, 3 }, { 0, 1, 4 } }; // internal, count, complex
	const size_t names_at = 80;
	const size_t fields_at = names_at + name_table.size();
	const size_t complexes_at = align16(fields_at + std::size(fields) * 16);
	const size_t instance_repeaters_at = complexes_at + std::size(complexes) * 16;
	const size_t array_repeaters_at = align16(instance_repeaters_at + std::size(repeaters) * 12);
	const size_t meta_size = align16(array_repeaters_at + 2 * 12);
	const size_t instances_at = meta_size; // no strings
	const size_t instance_payload = 3 * (16 + 16);
	const size_t arrays_at = instances_at + instance_payload;
	const size_t members_at = align16(4 + 4 * p_objects.size() + 4); // array 1, after its count
	const size_t total = arrays_at + members_at + 4 * size_t(p_members);
	Bytes b(total, 0);
	put32(b, 0x00, 0x0FB2D1CE);
	put32(b, 0x04, uint32_t(meta_size));
	put32(b, 0x08, uint32_t(total - meta_size));
	put32(b, 0x14, uint32_t(std::size(repeaters)));
	put32(b, 0x18, uint32_t(std::size(complexes)));
	put32(b, 0x1C, uint32_t(std::size(fields)));
	put32(b, 0x20, uint32_t(name_table.size()));
	put32(b, 0x28, 2);
	put32(b, 0x2C, uint32_t(instance_payload));
	const Guid partition = *Guid::parse("5e1f0000-0000-4000-8000-000000000001");
	const Guid instance_guids[3] = { *Guid::parse("5e1f0000-0000-4000-8000-000000000010"), *Guid::parse("5e1f0000-0000-4000-8000-000000000011"),
		*Guid::parse("5e1f0000-0000-4000-8000-000000000012") };
	std::memcpy(b.data() + 0x30, partition.bytes.data(), 16);
	std::memcpy(b.data() + 0x40, instance_guids[0].bytes.data(), 16);
	std::memcpy(b.data() + names_at, name_table.data(), name_table.size());
	for (size_t i = 0; i < std::size(fields); ++i) {
		const size_t at = fields_at + i * 16;
		put32(b, at, ebx_name_hash(fields[i].name));
		put16(b, at + 4, uint16_t(fields[i].code << 4));
		put16(b, at + 6, fields[i].ref);
		put32(b, at + 8, fields[i].offset);
	}
	for (size_t i = 0; i < std::size(complexes); ++i) {
		const size_t at = complexes_at + i * 16;
		put32(b, at, ebx_name_hash(complexes[i].name));
		put32(b, at + 4, complexes[i].field_start);
		b[at + 8] = complexes[i].count;
		b[at + 9] = 4;
		put16(b, at + 10, uint16_t(complexes[i].code << 4));
		put16(b, at + 12, complexes[i].size);
	}
	for (size_t i = 0; i < std::size(repeaters); ++i) {
		for (size_t j = 0; j < 3; ++j) {
			put32(b, instance_repeaters_at + i * 12 + j * 4, repeaters[i][j]);
		}
	}
	// Array 0 (Objects): count at section+0, elements at section+4. Array 1 (MemberDatas): count
	// just before members_at, elements at members_at.
	put32(b, array_repeaters_at, 4);
	put32(b, array_repeaters_at + 4, uint32_t(p_objects.size()));
	put32(b, array_repeaters_at + 8, 2);
	put32(b, array_repeaters_at + 12, uint32_t(members_at));
	put32(b, array_repeaters_at + 16, p_members);
	put32(b, array_repeaters_at + 20, 5);
	for (size_t i = 0; i < 3; ++i) {
		std::memcpy(b.data() + instances_at + i * 32, instance_guids[i].bytes.data(), 16);
	}
	put32(b, instances_at + 16 + 8, 0); // WorldPartData.Objects: array 0
	put32(b, instances_at + 32 + 16 + 8, 1); // WorldPartReferenceObjectData.Blueprint: internal, instance 0
	put32(b, instances_at + 64 + 16 + 8, 1); // StaticModelGroupEntityData.MemberDatas: array 1
	put32(b, arrays_at, uint32_t(p_objects.size()));
	for (size_t i = 0; i < p_objects.size(); ++i) {
		put32(b, arrays_at + 4 + 4 * i, p_objects[i] + 1); // internal references
	}
	put32(b, arrays_at + members_at - 4, p_members);
	return b;
}

// A synthetic partition written through its own type descriptors, the layout EbxPartition::parse
// reads: classes derive from DataContainer (8 bytes), every field takes 4 bytes (references, array
// indices, string offsets, Booleans, 32-bit numbers) except value types, one instance repeater per
// instance (instance 0 is the primary). Any number of fields may name one array, which is how a
// small hostile partition makes K lookups of one N-element list.
class SynthPartition {
public:
	struct Field {
		std::string name;
		uint8_t code = ebx_code::kUInt32;
		std::string type; // kValueType: its type; kArray of kValueType: the element type
		uint8_t element = ebx_code::kClass; // kArray: the element code
		bool overlap = false; // shares one slot with the type's other overlapping fields (same size)
	};

	explicit SynthPartition(uint32_t p_seed) :
			seed_(p_seed) {
		declare("DataContainer", ebx_code::kClass, "", {});
	}

	// p_base: "" for a value type or the root class, else a class declared earlier.
	void declare(const std::string &p_name, uint8_t p_code, const std::string &p_base, const std::vector<Field> &p_fields) {
		Type t;
		t.name = p_name;
		t.code = p_code;
		std::vector<Layout> layout;
		for (const Field &f : p_fields) {
			Layout l;
			l.name = f.name;
			l.code = f.code;
			l.overlap = f.overlap;
			if (f.code == ebx_code::kArray) {
				l.stride = f.element == ebx_code::kValueType ? types_[find(f.type)].size : 4;
				l.ref = uint32_t(complexes_.size());
				const uint32_t element_ref = f.element == ebx_code::kValueType ? types_[find(f.type)].complex : 0;
				complexes_.push_back({ "array", uint32_t(descs_.size()), 1, ebx_code::kArray, 4 });
				descs_.push_back({ "member", f.element, element_ref, 0 });
			} else if (f.code == ebx_code::kValueType) {
				l.ref = types_[find(f.type)].complex;
			}
			layout.push_back(l);
		}
		const uint32_t field_start = uint32_t(descs_.size());
		uint32_t offset = 0;
		if (!p_base.empty()) {
			t.base = find(p_base);
			descs_.push_back({ "$", ebx_code::kVoid, types_[*t.base].complex, 0 });
			offset = types_[*t.base].size;
		} else if (p_code == ebx_code::kClass) {
			offset = 8;
		}
		std::optional<uint32_t> shared_at;
		for (Layout &l : layout) {
			const uint32_t size = l.code == ebx_code::kValueType ? types_[find(fields_type(p_fields, l.name))].size : 4;
			if (l.overlap && shared_at) {
				l.offset = *shared_at;
			} else {
				l.offset = offset;
				offset += size;
				shared_at = l.overlap ? std::optional<uint32_t>(l.offset) : shared_at;
			}
			descs_.push_back({ l.name, l.code, l.ref, l.offset });
		}
		t.size = offset;
		t.fields = layout;
		t.complex = uint32_t(complexes_.size());
		complexes_.push_back({ p_name, field_start, uint32_t(descs_.size() - field_start), p_code, offset });
		types_.push_back(t);
	}

	// A new instance, all zero except its arrays, which start as empty ones.
	uint32_t add(const std::string &p_type) {
		const Type &t = types_[find(p_type)];
		instances_.push_back({ t.complex, std::vector<uint8_t>(t.size, 0) });
		instance_types_.push_back(find(p_type));
		const uint32_t at = uint32_t(instances_.size() - 1);
		for (std::optional<size_t> type = find(p_type); type; type = types_[*type].base) {
			for (const Layout &l : types_[*type].fields) {
				if (l.code != ebx_code::kArray) {
					continue;
				}
				auto empty = empty_arrays_.find(l.ref);
				if (empty == empty_arrays_.end()) {
					arrays_.push_back(Array{ l.ref, 0, {} });
					empty = empty_arrays_.emplace(l.ref, uint32_t(arrays_.size() - 1)).first;
				}
				std::memcpy(instances_[at].second.data() + l.offset, &empty->second, 4);
			}
		}
		return at;
	}

	void set(uint32_t p_instance, const std::string &p_field, uint32_t p_raw) {
		const size_t at = offset_of(types_[instance_types_[p_instance]].name, p_field);
		std::memcpy(instances_[p_instance].second.data() + at, &p_raw, 4);
	}
	// Field p_field as base type p_type declares it (a derived type may redeclare the name).
	void set_as(uint32_t p_instance, const std::string &p_type, const std::string &p_field, uint32_t p_raw) {
		const size_t at = offset_of(p_type, p_field);
		std::memcpy(instances_[p_instance].second.data() + at, &p_raw, 4);
	}
	void set_ref(uint32_t p_instance, const std::string &p_field, uint32_t p_target) { set(p_instance, p_field, p_target + 1); }
	void set_string(uint32_t p_instance, const std::string &p_field, const std::string &p_text) { set(p_instance, p_field, string(p_text)); }
	static uint32_t ref(uint32_t p_instance) { return p_instance + 1; }

	uint32_t string(const std::string &p_text) {
		const auto found = strings_at_.find(p_text);
		if (found != strings_at_.end()) {
			return found->second;
		}
		const uint32_t at = uint32_t(strings_.size());
		strings_ += p_text;
		strings_ += '\0';
		strings_at_.emplace(p_text, at);
		return at;
	}

	// A new array for field p_field of p_type (any field naming it may then be set to it): p_count
	// elements of the field's element type, each 32-bit word p_word(element, word index) (zero:
	// null references).
	uint32_t array(const std::string &p_type, const std::string &p_field, uint32_t p_count,
			const std::function<uint32_t(uint32_t, uint32_t)> &p_word = {}) {
		const Layout &l = layout_of(p_type, p_field);
		Array a;
		a.complex = l.ref;
		a.count = p_count;
		a.bytes.assign(size_t(p_count) * l.stride, 0);
		if (p_word) {
			for (uint32_t i = 0; i < p_count; ++i) {
				for (uint32_t w = 0; w < l.stride / 4; ++w) {
					const uint32_t v = p_word(i, w);
					std::memcpy(a.bytes.data() + size_t(i) * l.stride + 4 * w, &v, 4);
				}
			}
		}
		arrays_.push_back(std::move(a));
		return uint32_t(arrays_.size() - 1);
	}

	size_t offset_of(const std::string &p_type, const std::string &p_field) const { return layout_of(p_type, p_field).offset; }

	std::vector<uint8_t> bytes() const {
		const auto align16 = [](size_t p_at) { return (p_at + 15) & ~size_t(15); };
		std::string names;
		std::set<std::string> named;
		const auto name = [&](const std::string &p_name) {
			if (named.insert(p_name).second) {
				names += p_name;
				names += '\0';
			}
		};
		for (const Complex &c : complexes_) {
			name(c.name);
		}
		for (const Desc &d : descs_) {
			name(d.name);
		}
		const size_t names_at = 0x50;
		const size_t fields_at = names_at + names.size();
		const size_t complexes_at = align16(fields_at + descs_.size() * 16);
		const size_t instance_repeaters_at = complexes_at + complexes_.size() * 16;
		const size_t array_repeaters_at = align16(instance_repeaters_at + instances_.size() * 12);
		const size_t meta = align16(array_repeaters_at + arrays_.size() * 12);
		size_t payload = 0;
		for (const auto &[complex, data] : instances_) {
			payload += 16 + data.size();
		}
		const size_t arrays_at = meta + strings_.size() + payload;
		std::vector<size_t> starts;
		size_t cursor = 0;
		for (const Array &a : arrays_) {
			const size_t start = align16(cursor + 4);
			starts.push_back(start);
			cursor = start + a.bytes.size();
		}
		std::vector<uint8_t> b(arrays_at + cursor, 0);
		const auto put32 = [&](size_t p_at, uint32_t p_value) { std::memcpy(b.data() + p_at, &p_value, 4); };
		const auto put16 = [&](size_t p_at, uint16_t p_value) { std::memcpy(b.data() + p_at, &p_value, 2); };
		put32(0x00, 0x0FB2D1CE);
		put32(0x04, uint32_t(meta));
		put32(0x08, uint32_t(b.size() - meta));
		put32(0x14, uint32_t(instances_.size()));
		put32(0x18, uint32_t(complexes_.size()));
		put32(0x1C, uint32_t(descs_.size()));
		put32(0x20, uint32_t(names.size()));
		put32(0x24, uint32_t(strings_.size()));
		put32(0x28, uint32_t(arrays_.size()));
		put32(0x2C, uint32_t(payload));
		const Guid partition = guid(0xFFFFFFFFu);
		const Guid primary = guid(0);
		std::memcpy(b.data() + 0x30, partition.bytes.data(), 16);
		std::memcpy(b.data() + 0x40, primary.bytes.data(), 16);
		std::memcpy(b.data() + names_at, names.data(), names.size());
		for (size_t i = 0; i < descs_.size(); ++i) {
			const size_t at = fields_at + i * 16;
			put32(at, ebx_name_hash(descs_[i].name));
			put16(at + 4, uint16_t(descs_[i].code << 4));
			put16(at + 6, uint16_t(descs_[i].ref));
			put32(at + 8, descs_[i].offset);
		}
		for (size_t i = 0; i < complexes_.size(); ++i) {
			const size_t at = complexes_at + i * 16;
			put32(at, ebx_name_hash(complexes_[i].name));
			put32(at + 4, complexes_[i].field_start);
			b[at + 8] = uint8_t(complexes_[i].count);
			b[at + 9] = 4;
			put16(at + 10, uint16_t(complexes_[i].code << 4));
			put16(at + 12, uint16_t(complexes_[i].size));
		}
		for (size_t i = 0; i < instances_.size(); ++i) {
			put32(instance_repeaters_at + i * 12 + 4, 1);
			put32(instance_repeaters_at + i * 12 + 8, instances_[i].first);
		}
		for (size_t i = 0; i < arrays_.size(); ++i) {
			put32(array_repeaters_at + i * 12, uint32_t(starts[i]));
			put32(array_repeaters_at + i * 12 + 4, arrays_[i].count);
			put32(array_repeaters_at + i * 12 + 8, arrays_[i].complex);
		}
		std::memcpy(b.data() + meta, strings_.data(), strings_.size());
		size_t at = meta + strings_.size();
		for (size_t i = 0; i < instances_.size(); ++i) {
			const Guid g = guid(uint32_t(i));
			std::memcpy(b.data() + at, g.bytes.data(), 16);
			std::memcpy(b.data() + at + 16, instances_[i].second.data(), instances_[i].second.size());
			at += 16 + instances_[i].second.size();
		}
		for (size_t i = 0; i < arrays_.size(); ++i) {
			put32(arrays_at + starts[i] - 4, arrays_[i].count);
			std::memcpy(b.data() + arrays_at + starts[i], arrays_[i].bytes.data(), arrays_[i].bytes.size());
		}
		return b;
	}

private:
	struct Layout {
		std::string name;
		uint8_t code = 0;
		uint32_t ref = 0; // array complex, or the value type's complex
		uint32_t offset = 0;
		uint32_t stride = 4; // arrays
		bool overlap = false;
	};
	struct Type {
		std::string name;
		uint8_t code = 0;
		uint32_t size = 0;
		uint32_t complex = 0;
		std::optional<size_t> base;
		std::vector<Layout> fields;
	};
	struct Complex {
		std::string name;
		uint32_t field_start, count;
		uint8_t code;
		uint32_t size;
	};
	struct Desc {
		std::string name;
		uint8_t code;
		uint32_t ref, offset;
	};
	struct Array {
		uint32_t complex = 0, count = 0;
		std::vector<uint8_t> bytes;
	};

	static std::string fields_type(const std::vector<Field> &p_fields, const std::string &p_name) {
		for (const Field &f : p_fields) {
			if (f.name == p_name) {
				return f.type;
			}
		}
		return {};
	}
	size_t find(const std::string &p_type) const {
		for (size_t i = 0; i < types_.size(); ++i) {
			if (types_[i].name == p_type) {
				return i;
			}
		}
		FAIL("synthetic type " << p_type << " is not declared");
		return 0;
	}
	const Layout &layout_of(const std::string &p_type, const std::string &p_field) const {
		std::optional<size_t> at = find(p_type);
		while (at) {
			for (const Layout &l : types_[*at].fields) {
				if (l.name == p_field) {
					return l;
				}
			}
			at = types_[*at].base;
		}
		FAIL("synthetic field " << p_type << "." << p_field << " is not declared");
		return types_[0].fields.front();
	}
	Guid guid(uint32_t p_index) const {
		Guid g;
		g.bytes[0] = 0x5e;
		std::memcpy(g.bytes.data() + 4, &seed_, 4);
		std::memcpy(g.bytes.data() + 12, &p_index, 4);
		return g;
	}

	uint32_t seed_;
	std::vector<Type> types_;
	std::vector<Complex> complexes_;
	std::vector<Desc> descs_;
	std::vector<std::pair<uint32_t, std::vector<uint8_t>>> instances_;
	std::vector<size_t> instance_types_;
	std::vector<Array> arrays_;
	std::unordered_map<uint32_t, uint32_t> empty_arrays_; // by array complex
	std::string strings_;
	std::unordered_map<std::string, uint32_t> strings_at_;
};

// The types the traversal and its helpers read, with only the fields they read.
void hostile_types(SynthPartition &r_part) {
	using F = SynthPartition::Field;
	using namespace ebx_code;
	r_part.declare("PropertyConnection", kValueType, "", { F{ "Source", kClass }, F{ "Target", kClass }, F{ "SourceFieldId", kInt32 }, F{ "TargetFieldId", kInt32 } });
	r_part.declare("StaticModelGroupMemberData", kValueType, "", { F{ "InstanceCount", kUInt32 }, F{ "MemberType", kClass } });
	r_part.declare("LevelDescriptionInclusionCategory", kValueType, "", { F{ "Category", kCString }, F{ "Mode", kArray, "", kCString } });
	r_part.declare("WorldPartData", kClass, "DataContainer", { F{ "Objects", kArray }, F{ "PropertyConnections", kArray, "PropertyConnection", kValueType } });
	r_part.declare("PrefabBlueprint", kClass, "DataContainer",
			{ F{ "Objects", kArray }, F{ "PropertyConnections", kArray, "PropertyConnection", kValueType }, F{ "Descriptor", kClass } });
	r_part.declare("InterfaceDescriptorData", kClass, "DataContainer", {});
	r_part.declare("ReferenceObjectData", kClass, "DataContainer", { F{ "Blueprint", kClass }, F{ "Excluded", kBoolean } });
	r_part.declare("WorldPartReferenceObjectData", kClass, "ReferenceObjectData", {});
	r_part.declare("SubWorldReferenceObjectData", kClass, "ReferenceObjectData", { F{ "BundleName", kCString }, F{ "AutoLoad", kBoolean }, F{ "InclusionSettings", kClass } });
	r_part.declare("SubWorldInclusionSettings", kClass, "DataContainer", { F{ "Settings", kArray } });
	r_part.declare("SubWorldInclusionSetting", kClass, "DataContainer", { F{ "Criterion", kClass }, F{ "EnabledOptions", kArray, "", kCString } });
	r_part.declare("StaticModelGroupEntityData", kClass, "DataContainer", { F{ "MemberDatas", kArray, "StaticModelGroupMemberData", kValueType } });
	r_part.declare("MeshVariationDatabase", kClass, "DataContainer", { F{ "Entries", kArray }, F{ "RedirectEntries", kArray } });
	r_part.declare("MeshAsset", kClass, "DataContainer", {});
	r_part.declare("CompositeMeshAsset", kClass, "MeshAsset", { F{ "Name", kCString }, F{ "Materials", kArray } });
	r_part.declare("PartComponentData", kClass, "DataContainer", { F{ "Components", kArray }, F{ "HealthStates", kArray } });
	r_part.declare("PhysicsEntityData", kClass, "DataContainer", { F{ "RigidBodies", kArray }, F{ "ScaledAssets", kArray }, F{ "Asset", kClass } });
	r_part.declare("StaticModelEntityData", kClass, "DataContainer", { F{ "Mesh", kClass }, F{ "Components", kArray }, F{ "PhysicsData", kClass } });
	r_part.declare("BoolEntityData", kClass, "DataContainer", { F{ "DefaultValue", kBoolean } });
	r_part.declare("LevelDescriptionAsset", kClass, "DataContainer", { F{ "Categories", kArray, "LevelDescriptionInclusionCategory", kValueType } });
}

const EbxPartition &insert(EbxDb &r_db, const std::string &p_name, const SynthPartition &p_part) {
	Result<bool> inserted = r_db.parse_insert(p_name, p_part.bytes());
	REQUIRE_MESSAGE(inserted.ok(), (inserted.ok() ? std::string() : inserted.error().message));
	const EbxPartition *partition = r_db.partition_by_name(p_name);
	REQUIRE(partition != nullptr);
	return *partition;
}

// A connection list entry: Source, Target, SourceFieldId, TargetFieldId.
std::function<uint32_t(uint32_t, uint32_t)> connections(const std::vector<std::array<uint32_t, 4>> &p_rows) {
	return [p_rows](uint32_t p_element, uint32_t p_word) { return p_rows[p_element % p_rows.size()][p_word]; };
}

template <class F>
double timed(F &&p_run) {
	const auto start = std::chrono::steady_clock::now();
	p_run();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool budget_error(const Error &p_error) {
	return p_error.message.find("work steps") != std::string::npos;
}

// The string offset EBX uses for a null string.
constexpr uint32_t kNullText = 0xFFFFFFFFu;

// A level partition holding only a LevelData root, for read_level_info.
SynthPartition level_root(uint32_t p_seed) {
	SynthPartition part(p_seed);
	hostile_types(part);
	part.declare("LevelData", ebx_code::kClass, "DataContainer", {});
	part.add("LevelData");
	return part;
}

// p_depth types, each deriving from the previous (the first from p_base) and adding p_pads 4-byte
// fields that overlap in one slot, named Pad0..Pad3 (so the lookup tables stay small while each
// type's descriptors do not). Returns the last type's name (p_base when p_depth is 0).
std::string padded_chain(SynthPartition &r_part, const std::string &p_prefix, const std::string &p_base, uint8_t p_code, int p_depth, int p_pads) {
	std::string base = p_base;
	for (int d = 1; d <= p_depth; ++d) {
		std::vector<SynthPartition::Field> pads;
		for (int i = 0; i < p_pads; ++i) {
			pads.push_back(SynthPartition::Field{ "Pad" + std::to_string(i % 4), ebx_code::kUInt32, "", ebx_code::kClass, true });
		}
		const std::string name = p_prefix + std::to_string(d);
		r_part.declare(name, p_code, base, pads);
		base = name;
	}
	return base;
}

// A LinearTransform-shaped value type whose rows and their x/y/z sit p_depth types deep under
// p_pads padding fields per type; returns its name.
std::string padded_transform(SynthPartition &r_part, int p_depth, int p_pads) {
	using F = SynthPartition::Field;
	using namespace ebx_code;
	r_part.declare("V0", kValueType, "", { F{ "x", kFloat32 }, F{ "y", kFloat32 }, F{ "z", kFloat32 } });
	const std::string vec = padded_chain(r_part, "V", "V0", kValueType, p_depth, p_pads);
	r_part.declare("LT0", kValueType, "", { F{ "right", kValueType, vec }, F{ "up", kValueType, vec }, F{ "forward", kValueType, vec }, F{ "trans", kValueType, vec } });
	return padded_chain(r_part, "LT", "LT0", kValueType, p_depth, p_pads);
}

// What every hostile case below must stay under: the uncharged walks they reproduce took minutes
// to hours; the charged ones end in well under a second (dev) on the budget error.
constexpr double kHostileSeconds = 60.0;

} // namespace

TEST_SUITE("level_unit") {
	TEST_CASE("level_unit: child in parent is the row-vector product") {
		// Parent: 90 degrees about Y (right -> -Z, forward -> +X), scale 2, at (10, 0, 0).
		LevelTransform parent;
		parent.right = { 0.0f, 0.0f, -1.0f };
		parent.up = { 0.0f, 1.0f, 0.0f };
		parent.forward = { 1.0f, 0.0f, 0.0f };
		parent.trans = { 10.0f, 0.0f, 0.0f };
		parent.scale = 2.0f;
		LevelTransform child;
		child.trans = { 1.0f, 2.0f, 3.0f };
		const LevelTransform world = child.in_parent(parent);
		// p' = trans + scale * (x*right + y*up + z*forward) = (10,0,0) + 2*((0,0,-1) + (0,2,0) + (3,0,0))
		CHECK(world.trans == level::Vec3{ 16.0f, 4.0f, -2.0f });
		CHECK(world.right == parent.right);
		CHECK(world.scale == 2.0f);
		// The +0 start of the accumulation turns a -0 product into +0, as IW4L's loop does.
		LevelTransform negative_zero;
		negative_zero.right = { -0.0f, 1.0f, 0.0f };
		CHECK(bits(LevelTransform().rotate(negative_zero.right)[0]) == 0u);
		CHECK(LevelTransform().transform_point({ 1.0f, 2.0f, 3.0f }) == level::Vec3{ 1.0f, 2.0f, 3.0f });
	}

	TEST_CASE("level_unit: scale in the axes") {
		LevelTransform t;
		t.right = { 2.0f, 0.0f, 0.0f };
		t.up = { 0.0f, 2.0f, 0.0f };
		t.forward = { 0.0f, 0.0f, 2.0f };
		CHECK(t.is_uniform(1e-3f));
		CHECK(t.normalized().scale == 2.0f);
		CHECK(t.normalized().right == level::Vec3{ 1.0f, 0.0f, 0.0f });
		t.forward = { 0.0f, 0.0f, 59.8f };
		CHECK_FALSE(t.is_uniform(1e-3f));
		CHECK(t.axis_scales()[2] == 59.8f);
		CHECK(t.determinant() == 2.0f * 2.0f * 59.8f);
	}

	TEST_CASE("level_unit: interface constants") {
		const std::optional<Property> transform = Property::parse("LinearTransform ((1,0,0),(0,1,0),(0,0,1),(-194.6,64.6,-403.3))");
		REQUIRE(transform.has_value());
		REQUIRE(transform->as_transform().has_value());
		CHECK(transform->as_transform()->trans == level::Vec3{ -194.6f, 64.6f, -403.3f });
		CHECK(transform->as_vec3() == level::Vec3{ -194.6f, 64.6f, -403.3f });
		CHECK(Property::parse("Boolean True")->as_bool() == true);
		CHECK(Property::parse("Boolean false")->as_bool() == false);
		CHECK(Property::parse("Int32 55")->as_i64() == 55);
		CHECK(Property::parse("Int32 +7")->as_i64() == 7);
		CHECK(Property::parse("Float32 0.5")->as_f32() == 0.5f);
		CHECK(Property::parse("CString \"ID_H_MAP_CAFE\"")->as_str() == "ID_H_MAP_CAFE");
		CHECK(Property::parse("TeamId Team2")->as_team() == 2u);
		CHECK(Property::parse("TeamId TeamNeutral")->as_team() == 0u);
		CHECK(Property::parse("Vec3 (1, 2, 3)")->as_vec3() == level::Vec3{ 1.0f, 2.0f, 3.0f });
		// Unparsed values stay constants as written; empty means no constant.
		CHECK(Property::parse("LinearTransform ((1,0,0),(0,1,0),(0,0,1),(1,2))")->kind == Property::Kind::Constant);
		CHECK(Property::parse("Boolean maybe")->kind == Property::Kind::Constant);
		CHECK(Property::parse("Int32 12x")->kind == Property::Kind::Constant);
		CHECK(Property::parse("Unknown")->as_str() == "Unknown");
		CHECK_FALSE(Property::parse("   ").has_value());
		CHECK(field_id("Visible") == int32_t(ebx_name_hash("Visible")));
	}

	TEST_CASE("level_unit: bake split") {
		LevelTransform uniform;
		uniform.right = { 0.0f, 0.0f, -3.0f };
		uniform.up = { 0.0f, 3.0f, 0.0f };
		uniform.forward = { 3.0f, 0.0f, 0.0f };
		CHECK_FALSE(bake_split(uniform).linear.has_value());

		LevelTransform stretched;
		stretched.right = { 1.68f, 0.0f, 0.0f };
		stretched.up = { 0.0f, 1.68f, 0.0f };
		stretched.forward = { 0.0f, 0.0f, 59.8f };
		stretched.trans = { 5.0f, 6.0f, 7.0f };
		const BakeSplit split = bake_split(stretched);
		REQUIRE(split.linear.has_value());
		CHECK(split.placed.scale == 1.0f);
		CHECK(split.placed.trans == stretched.trans);
		const std::array<int32_t, 9> key = bake_key(*split.linear);
		CHECK(key[0] == int32_t(std::lround(1.68f * 4096.0f)));
		CHECK(key[8] == int32_t(std::lround(59.8f * 4096.0f)));
		CHECK(key[1] == 0);

		LevelTransform mirrored = uniform;
		mirrored.right = { 0.0f, 0.0f, 3.0f };
		CHECK(bake_split(mirrored).linear.has_value()); // negative determinant is baked

		LevelTransform degenerate;
		degenerate.right = { 0.0f, 0.0f, 0.0f };
		degenerate.up = { 0.0f, 0.0f, 0.0f };
		degenerate.forward = { 0.0f, 0.0f, 0.0f };
		CHECK_FALSE(bake_split(degenerate).linear.has_value()); // not invertible: placed as it is

		std::array<std::array<float, 3>, 3> huge{};
		huge[0][0] = 1e30f;
		huge[0][1] = -1e30f;
		huge[0][2] = std::numeric_limits<float>::quiet_NaN();
		const std::array<int32_t, 9> saturated = bake_key(huge);
		CHECK(saturated[0] == INT32_MAX);
		CHECK(saturated[1] == INT32_MIN);
		CHECK(saturated[2] == 0);
	}

	TEST_CASE("level_unit: modes and names") {
		CHECK(mode_matches("ConquestLarge0", "conquestlarge0"));
		CHECK_FALSE(mode_matches("ConquestLarge0", "ConquestSmall0"));
		CHECK(level_display_name("mp_subway") == "Operation Metro");
		CHECK(level_display_name("XP5_004") == "Sabalan Pipeline");
		CHECK(zone_name("MP_Subway") == "mp_subway");
		CHECK(zone_name("XP2_Palace") == "mp_xp2_palace");
	}

	TEST_CASE("level_unit: work budget is charged before and stays exhausted") {
		WorkBudget budget(100);
		CHECK(budget.charge(60).ok());
		CHECK(budget.charge(40).ok());
		CHECK(budget.spent() == 100);
		CHECK_FALSE(budget.charge(1).ok());
		CHECK(budget.exhausted());
		CHECK_FALSE(budget.charge(0).ok()); // sticky: nothing more passes
		CHECK_FALSE(budget.status().ok());
		CHECK(budget_error(budget.status().error()));
		WorkBudget huge(100);
		CHECK_FALSE(huge.charge(std::numeric_limits<size_t>::max()).ok());
		CHECK(huge.spent() == 0); // a charge past the limit is refused whole, never added
	}

	TEST_CASE("level_unit: inclusion settings charge their lists before scanning them") {
		// K references to one InclusionSettings whose Settings list holds N null slots: each check
		// pays N first, so the budget ends the loop after about limit / N checks.
		SynthPartition part(1);
		hostile_types(part);
		const uint32_t root = part.add("WorldPartData");
		(void)root;
		const uint32_t settings = part.add("SubWorldInclusionSettings");
		const uint32_t reference = part.add("SubWorldReferenceObjectData");
		part.set(reference, "AutoLoad", 1);
		part.set_ref(reference, "InclusionSettings", settings);
		constexpr uint32_t kNulls = 1u << 20;
		part.set(settings, "Settings", part.array("SubWorldInclusionSettings", "Settings", kNulls));
		// One setting listed K times, its EnabledOptions N names with the mode last (so every
		// setting is scanned to the end).
		const uint32_t shared_settings = part.add("SubWorldInclusionSettings");
		const uint32_t setting = part.add("SubWorldInclusionSetting");
		const uint32_t shared_reference = part.add("SubWorldReferenceObjectData");
		part.set(shared_reference, "AutoLoad", 1);
		part.set_ref(shared_reference, "InclusionSettings", shared_settings);
		constexpr uint32_t kOptions = 4096, kListed = 4096;
		const uint32_t other = part.string("Other0"), mode = part.string("TeamDeathMatch0");
		part.set(setting, "EnabledOptions",
				part.array("SubWorldInclusionSetting", "EnabledOptions", kOptions, [&](uint32_t p_i, uint32_t) { return p_i + 1 == kOptions ? mode : other; }));
		part.set(shared_settings, "Settings", part.array("SubWorldInclusionSettings", "Settings", kListed, [&](uint32_t, uint32_t) { return SynthPartition::ref(setting); }));
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		const EbxInstance r = *partition.instance(reference);
		const EbxInstance shared = *partition.instance(shared_reference);
		{
			WorkBudget budget;
			const Result<bool> one = includes(db, r, "TeamDeathMatch0", budget);
			REQUIRE(one.ok());
			CHECK(*one); // null settings constrain nothing
			CHECK(budget.spent() == kNulls);
		}
		size_t checks = 0;
		WorkBudget budget;
		const double seconds = timed([&] {
			for (; checks < 100000; ++checks) {
				if (!includes(db, r, "TeamDeathMatch0", budget).ok()) {
					break;
				}
			}
		});
		CHECK(checks == WorkBudget::kDefaultLimit / kNulls); // the first check the budget cannot pay fails
		CHECK(budget.exhausted());
		CHECK_FALSE(includes(db, r, "TeamDeathMatch0", budget).ok()); // and every one after it
		WorkBudget shared_budget;
		const Result<bool> fanned = includes(db, shared, "TeamDeathMatch0", shared_budget);
		REQUIRE_FALSE(fanned.ok());
		CHECK(budget_error(fanned.error()));
		MESSAGE("inclusion: ", checks, " checks of ", kNulls, " slots then the budget error in ", seconds, " s");
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level_unit: the sub-world search charges each container's slots") {
		// A layer listing one world part K times whose Objects are N null slots.
		SynthPartition part(2);
		hostile_types(part);
		const uint32_t root = part.add("WorldPartData");
		const uint32_t layer = part.add("WorldPartReferenceObjectData");
		const uint32_t target = part.add("WorldPartData");
		part.set_ref(layer, "Blueprint", target);
		part.set(root, "Objects", part.array("WorldPartData", "Objects", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(layer); }));
		part.set(target, "Objects", part.array("WorldPartData", "Objects", 1u << 20));
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		WorkBudget budget;
		std::vector<EbxInstance> references;
		Status searched;
		const double seconds = timed([&] { searched = subworld_references(db, partition.primary(), budget, references); });
		REQUIRE_FALSE(searched.ok());
		CHECK(budget_error(searched.error()));
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level_unit: hidden parts charge every component list") {
		// An entity listing one PartComponentData 4,096 times (the walk's cap) whose HealthStates are
		// N null slots: uncharged, one call scanned 4,096 x N slots.
		SynthPartition part(3);
		hostile_types(part);
		const uint32_t entity = part.add("StaticModelEntityData");
		const uint32_t component = part.add("PartComponentData");
		part.set(entity, "Components", part.array("StaticModelEntityData", "Components", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(component); }));
		part.set(component, "HealthStates", part.array("PartComponentData", "HealthStates", 1u << 20));
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		WorkBudget budget;
		Result<std::optional<std::vector<uint16_t>>> parts = std::optional<std::vector<uint16_t>>();
		const double seconds = timed([&] { parts = hidden_parts(db, partition.primary(), budget); });
		REQUIRE_FALSE(parts.ok());
		CHECK(budget_error(parts.error()));
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level_unit: level info charges categories that share one mode list") {
		// K GameMode categories sharing one N-slot Mode array (null names, read as empty ones):
		// uncharged, K x N slots. Charged, the first category pays N, and its names end the read on
		// the mode-count cap (or the budget, whichever comes first).
		SynthPartition root_part(4);
		hostile_types(root_part);
		root_part.declare("LevelData", ebx_code::kClass, "DataContainer", {});
		const uint32_t root = root_part.add("LevelData");
		(void)root;
		SynthPartition description(5);
		hostile_types(description);
		const uint32_t asset = description.add("LevelDescriptionAsset");
		const uint32_t game_mode = description.string("GameMode");
		const uint32_t modes = description.array("LevelDescriptionInclusionCategory", "Mode", 1u << 16, [&](uint32_t, uint32_t) { return kNullText; });
		description.set(asset, "Categories",
				description.array("LevelDescriptionAsset", "Categories", 1u << 16, [&](uint32_t, uint32_t p_word) { return p_word == 0 ? game_mode : modes; }));
		EbxDb db;
		const EbxPartition &level = insert(db, "levels/hostile/hostile", root_part);
		insert(db, "levels/hostile/hostile/description_win32", description);
		WorkBudget budget;
		Result<LevelInfo> info = LevelInfo();
		const double seconds = timed([&] { info = read_level_info(db, level.primary(), "levels/hostile/hostile", "Hostile", budget); });
		REQUIRE_FALSE(info.ok());
		INFO(info.error().message);
		CHECK((budget_error(info.error()) || info.error().message.find("game modes") != std::string::npos));
		CHECK(budget.spent() < (size_t(1) << 18)); // the categories, one category's slots, the names kept
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level_unit: start-hidden checks pay for the connections into Visible") {
		// M connections into X.Visible from a BoolEntityData that starts true, and K placements of
		// X: uncharged, every check walked the scope's whole connection list.
		SynthPartition part(6);
		hostile_types(part);
		const uint32_t blueprint = part.add("WorldPartData");
		const uint32_t reference = part.add("ReferenceObjectData");
		const uint32_t flag = part.add("BoolEntityData");
		part.set(flag, "DefaultValue", 1);
		constexpr uint32_t kConnections = 1u << 16;
		part.set(blueprint, "PropertyConnections",
				part.array("WorldPartData", "PropertyConnections", kConnections,
						connections({ { SynthPartition::ref(flag), SynthPartition::ref(reference), ebx_name_hash("Value"), ebx_name_hash("Visible") } })));
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		World world(db, std::make_shared<WorkBudget>());
		const Result<uint32_t> scope = world.scopes.push(partition.primary(), std::nullopt, std::nullopt, LevelTransform::identity(), 0, 0);
		REQUIRE(scope.ok());
		CHECK(world.budget->spent() == kConnections); // the list read once, charged first
		PlacedObject placed;
		placed.entity = *partition.instance(reference);
		placed.reference = *partition.instance(reference);
		placed.scope = *scope;
		size_t checks = 0;
		const double seconds = timed([&] {
			for (; checks < 100000; ++checks) {
				const Result<bool> hidden = starts_hidden(world, placed);
				if (!hidden.ok()) {
					break;
				}
				CHECK_FALSE(*hidden);
			}
		});
		CHECK(checks == WorkBudget::kDefaultLimit / kConnections - 1); // the push paid for one list
		CHECK(budget_error(starts_hidden(world, placed).error()));
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level_unit: strings decode without scanning them") {
		// N slots naming one L-byte string. A decode that scanned to the NUL cost N x L at parse
		// (validation decodes every slot) and again on every read: 2^16 x 1 MB = 64 GB here.
		SynthPartition part(20);
		hostile_types(part);
		const uint32_t asset = part.add("LevelDescriptionAsset");
		constexpr uint32_t kSlots = 1u << 16;
		constexpr size_t kLength = size_t(1) << 20;
		const uint32_t text = part.string(std::string(kLength, 'm'));
		const uint32_t game_mode = part.string("GameMode");
		const uint32_t modes = part.array("LevelDescriptionInclusionCategory", "Mode", kSlots, [&](uint32_t, uint32_t) { return text; });
		part.set(asset, "Categories", part.array("LevelDescriptionAsset", "Categories", 1, [&](uint32_t, uint32_t p_word) { return p_word == 0 ? game_mode : modes; }));
		const std::vector<uint8_t> bytes = part.bytes();
		Result<EbxPartition> parsed = make_error("not parsed");
		const double parse_seconds = timed([&] { parsed = EbxPartition::parse(bytes); });
		REQUIRE(parsed.ok());
		size_t decoded = 0, total = 0;
		const double read_seconds = timed([&] {
			const std::optional<EbxValue> categories = parsed->primary().get("Categories");
			const OptionalEbxArray list = categories ? categories->as_array() : OptionalEbxArray();
			REQUIRE(list);
			const std::optional<EbxValue> mode_list = list->get(0)->get("Mode");
			const OptionalEbxArray mode_array = mode_list ? mode_list->as_array() : OptionalEbxArray();
			REQUIRE(mode_array);
			for (const EbxValue v : *mode_array) {
				if (const std::optional<std::string_view> t = v.as_str()) {
					++decoded;
					total += t->size();
				}
			}
		});
		CHECK(decoded == kSlots);
		CHECK(total == size_t(kSlots) * kLength);
		MESSAGE("2^16 slots of one 1 MB string: parse ", parse_seconds, " s, decode ", read_seconds, " s");
		CHECK(parse_seconds + read_seconds < kHostileSeconds);
		// Offsets that are not a string's start still decode as the rest of that string, or fail.
		const std::optional<std::optional<std::string_view>> tail = parsed->string(text + 1);
		REQUIRE(tail);
		REQUIRE(*tail);
		CHECK((*tail)->size() == kLength - 1);
		CHECK_FALSE(parsed->string(uint32_t(1u << 30)));
		const std::optional<std::optional<std::string_view>> null_text = parsed->string(kNullText);
		REQUIRE(null_text);
		CHECK_FALSE(*null_text);
	}

	TEST_CASE("level_unit: level info charges every mode name it copies") {
		// One GameMode category whose Mode array names one 1 MB string N times: uncharged, N copies
		// of 1 MB (N = 512 took 512 MB for 513 units).
		SynthPartition root_part = level_root(21);
		SynthPartition description(22);
		hostile_types(description);
		const uint32_t asset = description.add("LevelDescriptionAsset");
		const uint32_t game_mode = description.string("GameMode");
		const uint32_t text = description.string(std::string(size_t(1) << 20, 'm'));
		const uint32_t modes = description.array("LevelDescriptionInclusionCategory", "Mode", 1u << 16, [&](uint32_t, uint32_t) { return text; });
		description.set(asset, "Categories",
				description.array("LevelDescriptionAsset", "Categories", 1, [&](uint32_t, uint32_t p_word) { return p_word == 0 ? game_mode : modes; }));
		EbxDb db;
		const EbxPartition &level = insert(db, "levels/hostile/hostile", root_part);
		insert(db, "levels/hostile/hostile/description_win32", description);
		WorkBudget budget;
		Result<LevelInfo> info = LevelInfo();
		const double seconds = timed([&] { info = read_level_info(db, level.primary(), "levels/hostile/hostile", "Hostile", budget); });
		REQUIRE_FALSE(info.ok());
		CHECK(budget_error(info.error()));
		CHECK(seconds < kHostileSeconds);

		// A few long names are fine; more than kMaxModes names fail on the count.
		SynthPartition counted(23);
		hostile_types(counted);
		const uint32_t counted_asset = counted.add("LevelDescriptionAsset");
		const uint32_t counted_game_mode = counted.string("GameMode"), name = counted.string("ConquestLarge0");
		const uint32_t named = counted.array("LevelDescriptionInclusionCategory", "Mode", 8192, [&](uint32_t, uint32_t) { return name; });
		counted.set(counted_asset, "Categories",
				counted.array("LevelDescriptionAsset", "Categories", 1, [&](uint32_t, uint32_t p_word) { return p_word == 0 ? counted_game_mode : named; }));
		EbxDb counted_db;
		const EbxPartition &counted_level = insert(counted_db, "levels/hostile/hostile", root_part);
		insert(counted_db, "levels/hostile/hostile/description_win32", counted);
		WorkBudget counted_budget;
		const Result<LevelInfo> many = read_level_info(counted_db, counted_level.primary(), "levels/hostile/hostile", "Hostile", counted_budget);
		REQUIRE_FALSE(many.ok());
		CHECK(many.error().message.find("game modes") != std::string::npos);
	}

	TEST_CASE("level_unit: inclusion settings charge the names they read") {
		// K settings listing one setting whose criterion name and only EnabledOptions entry are one
		// 4 MB string: each check charges the text, so the budget ends the scan after a few.
		SynthPartition part(24);
		hostile_types(part);
		part.declare("SubWorldInclusionCriterion", ebx_code::kClass, "DataContainer", { SynthPartition::Field{ "Name", ebx_code::kCString } });
		part.add("WorldPartData");
		const uint32_t settings = part.add("SubWorldInclusionSettings");
		const uint32_t setting = part.add("SubWorldInclusionSetting");
		const uint32_t criterion = part.add("SubWorldInclusionCriterion");
		const uint32_t reference = part.add("SubWorldReferenceObjectData");
		const uint32_t text = part.string(std::string(size_t(4) << 20, 'g'));
		part.set(criterion, "Name", text);
		part.set_ref(setting, "Criterion", criterion);
		part.set(setting, "EnabledOptions", part.array("SubWorldInclusionSetting", "EnabledOptions", 1, [&](uint32_t, uint32_t) { return text; }));
		part.set(settings, "Settings", part.array("SubWorldInclusionSettings", "Settings", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(setting); }));
		part.set(reference, "AutoLoad", 1);
		part.set_ref(reference, "InclusionSettings", settings);
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		{
			// A long criterion name that is not GameMode skips the setting: charged all the same.
			WorkBudget budget;
			Result<bool> included = false;
			const double seconds = timed([&] { included = includes(db, *partition.instance(reference), "TeamDeathMatch0", budget); });
			REQUIRE_FALSE(included.ok());
			CHECK(budget_error(included.error()));
			CHECK(seconds < kHostileSeconds);
		}
	}

	TEST_CASE("level_unit: resolver hops through a sub-world reference charge its BundleName") {
		// R a SubWorldReferenceObjectData with a 1 MB BundleName; R.F <- R.F three times and
		// R.Excluded <- R.F. Every hop looks R up as a sub-world by name: each one is charged the
		// name, so the first resolution already ends on the budget.
		SynthPartition part(25);
		hostile_types(part);
		const uint32_t blueprint = part.add("WorldPartData");
		const uint32_t r = part.add("SubWorldReferenceObjectData");
		part.set(r, "BundleName", part.string(std::string(size_t(1) << 20, 'b')));
		const uint32_t f = 0x12345u, excluded = ebx_name_hash("Excluded");
		const std::array<uint32_t, 4> loop = { SynthPartition::ref(r), SynthPartition::ref(r), f, f };
		part.set(blueprint, "PropertyConnections",
				part.array("WorldPartData", "PropertyConnections", 4, connections({ loop, loop, loop, { SynthPartition::ref(r), SynthPartition::ref(r), f, excluded } })));
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		World world(db, std::make_shared<WorkBudget>());
		const Result<uint32_t> scope = world.scopes.push(partition.primary(), std::nullopt, std::nullopt, LevelTransform::identity(), 0, 0);
		REQUIRE(scope.ok());
		Result<std::optional<Property>> resolved = std::optional<Property>();
		const double seconds = timed([&] { resolved = world.scopes.property(*scope, *partition.instance(r), field_id("Excluded")); });
		REQUIRE_FALSE(resolved.ok());
		CHECK(budget_error(resolved.error()));
		CHECK(world.scopes.hops() < 128); // about limit / text_units(1 MB)
		CHECK(seconds < kHostileSeconds);
		const Result<std::optional<EbxInstance>> after = world.scopes.referenced_blueprint(*partition.instance(r));
		REQUIRE_FALSE(after.ok());
		CHECK(budget_error(after.error()));
	}

	TEST_CASE("level_unit: field lookups take the most derived field, data_field the first") {
		// One search in the type's flattened table (EbxPartition::lookup) must give what the walk
		// over the base chain gave: get() the most derived declaration (the first within its type),
		// data_field the first in fields() order (base types first).
		using F = SynthPartition::Field;
		using namespace ebx_code;
		SynthPartition part(30);
		part.declare("Base", kClass, "DataContainer", { F{ "Excluded", kUInt32 }, F{ "A", kUInt32 } });
		part.declare("Mid", kClass, "Base", { F{ "B", kUInt32 }, F{ "B", kUInt32 } });
		part.declare("Top", kClass, "Mid", { F{ "Excluded", kUInt32 }, F{ "C", kUInt32 } });
		std::string deep = "DataContainer";
		for (int d = 1; d <= 30; ++d) {
			const std::string name = "D" + std::to_string(d);
			part.declare(name, kClass, deep, d == 1 ? std::vector<F>{ F{ "Deep", kUInt32 } } : std::vector<F>{ F{ "Pad" + std::to_string(d), kUInt32 } });
			deep = name;
		}
		const uint32_t top = part.add("Top");
		part.set_as(top, "Base", "Excluded", 11);
		part.set(top, "Excluded", 22);
		part.set(top, "A", 33);
		part.set(top, "B", 44); // the first B
		part.set(top, "C", 55);
		const uint32_t bottom = part.add(deep);
		part.set(bottom, "Deep", 77);
		part.set(bottom, "Pad30", 78);
		EbxDb db;
		const EbxPartition &p = insert(db, "levels/lookup/part", part);
		const EbxInstance t = *p.instance(top);
		CHECK(t.get("Excluded")->as_u32() == 22u);
		CHECK(data_field(t, int32_t(ebx_name_hash("Excluded")))->as_u32() == 11u);
		CHECK(t.get("A")->as_u32() == 33u);
		CHECK(t.get("B")->as_u32() == 44u);
		CHECK(data_field(t, int32_t(ebx_name_hash("B")))->as_u32() == 44u);
		CHECK(t.get("C")->as_u32() == 55u);
		CHECK(data_field(t, int32_t(ebx_name_hash("C")))->as_u32() == 55u);
		CHECK_FALSE(t.get("Missing"));
		CHECK_FALSE(t.get("$"));
		CHECK_FALSE(data_field(t, int32_t(ebx_name_hash("Missing"))));
		for (const EbxField &field : t.fields()) {
			if (field.name == "Excluded") {
				CHECK(field.value.as_u32() == 11u); // fields() meets Base's first, as data_field does
				break;
			}
		}
		const EbxInstance b = *p.instance(bottom);
		CHECK(b.is_a("D1"));
		CHECK(b.get("Deep")->as_u32() == 77u);
		CHECK(b.get("Pad30")->as_u32() == 78u);
		CHECK(data_field(b, int32_t(ebx_name_hash("Deep")))->as_u32() == 77u);
	}

	TEST_CASE("level_unit: type layouts past the parser's caps are rejected") {
		using F = SynthPartition::Field;
		const auto wide = [](uint32_t p_seed, int p_levels, int p_derived) {
			// p_levels types of 250 fields each (251 descriptors per level with the base), then
			// p_derived types each adding one field to the last.
			SynthPartition part(p_seed);
			std::string base = "DataContainer";
			for (int l = 1; l <= p_levels; ++l) {
				std::vector<F> fields;
				for (int i = 0; i < 250; ++i) {
					fields.push_back(F{ "W" + std::to_string(l) + "_" + std::to_string(i), ebx_code::kUInt32 });
				}
				const std::string name = "Wide" + std::to_string(l);
				part.declare(name, ebx_code::kClass, base, fields);
				base = name;
			}
			for (int d = 0; d < p_derived; ++d) {
				part.declare("Derived" + std::to_string(d), ebx_code::kClass, base, { F{ "Own" + std::to_string(d), ebx_code::kUInt32 } });
			}
			part.add(base);
			return EbxPartition::parse(part.bytes());
		};
		const Result<EbxPartition> fits = wide(31, 4, 4); // 1,004 descriptors along the chain, about 6.5k lookups
		CHECK_MESSAGE(fits.ok(), (fits.ok() ? std::string() : fits.error().message));
		const Result<EbxPartition> long_chain = wide(32, 5, 0); // 1,255 descriptors
		REQUIRE_FALSE(long_chain.ok());
		CHECK(long_chain.error().message.find("along its base chain") != std::string::npos);
		const Result<EbxPartition> many = wide(33, 4, 64); // 64 x 1,005 lookups from about 1,200 descriptors
		REQUIRE_FALSE(many.ok());
		CHECK(many.error().message.find("field lookups") != std::string::npos);
		// The verifier's deep form: value types 30 deep with 254 fields each (7,620 descriptors).
		SynthPartition deep(34);
		padded_transform(deep, 30, 254);
		deep.add("DataContainer");
		const Result<EbxPartition> rejected = EbxPartition::parse(deep.bytes());
		REQUIRE_FALSE(rejected.ok());
		CHECK(rejected.error().message.find("along its base chain") != std::string::npos);
	}

	TEST_CASE("level_unit: partition checks pay for wide overlapping types") {
		// B0 has 255 overlapping 4-byte fields, each Bk 255 overlapping Bk-1 (k = 1..6), and a Holder
		// carries one B6 beside a 16 MB string that buys check budget. The check ends on its budget
		// either way; when an object paid 1 whatever its width, each unit cost 255 field scans and
		// this partition took 88 s to fail.
		using F = SynthPartition::Field;
		using namespace ebx_code;
		SynthPartition part(35);
		std::vector<F> leaf;
		for (int i = 0; i < 255; ++i) {
			leaf.push_back(F{ "Pad" + std::to_string(i), kUInt32, "", kClass, true });
		}
		part.declare("B0", kValueType, "", leaf);
		std::string prev = "B0";
		for (int l = 1; l <= 6; ++l) {
			std::vector<F> fields;
			for (int i = 0; i < 255; ++i) {
				fields.push_back(F{ "Pad" + std::to_string(i), kValueType, prev, kClass, true });
			}
			const std::string name = "B" + std::to_string(l);
			part.declare(name, kValueType, "", fields);
			prev = name;
		}
		part.declare("Holder", kClass, "DataContainer", { F{ "V", kValueType, prev }, F{ "S", kCString } });
		const uint32_t holder = part.add("Holder");
		part.set_string(holder, "S", std::string(size_t(16) << 20, 's'));
		const std::vector<uint8_t> bytes = part.bytes();
		Result<EbxPartition> parsed = make_error("not parsed");
		const double seconds = timed([&] { parsed = EbxPartition::parse(bytes); });
		MESSAGE("16 MB partition of 255-wide overlapping types: ", (parsed.ok() ? std::string("ok") : parsed.error().message), " in ", seconds, " s");
		REQUIRE_FALSE(parsed.ok());
		CHECK(parsed.error().message.find("overlap") != std::string::npos);
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level_unit: self-referencing connection loops end on the budget") {
		// R.F <- R.F three times and R.Excluded <- R.F: each resolution is a ternary tree 16 levels
		// deep, cut at 2^16 hops, and the total budget ends a run of them.
		SynthPartition part(7);
		hostile_types(part);
		const uint32_t blueprint = part.add("WorldPartData");
		const uint32_t r = part.add("WorldPartReferenceObjectData");
		const uint32_t f = 0x12345u, excluded = ebx_name_hash("Excluded");
		const std::array<uint32_t, 4> loop = { SynthPartition::ref(r), SynthPartition::ref(r), f, f };
		part.set(blueprint, "PropertyConnections",
				part.array("WorldPartData", "PropertyConnections", 4, connections({ loop, loop, loop, { SynthPartition::ref(r), SynthPartition::ref(r), f, excluded } })));
		EbxDb db;
		const EbxPartition &partition = insert(db, "levels/hostile/part", part);
		World world(db, std::make_shared<WorkBudget>());
		const Result<uint32_t> scope = world.scopes.push(partition.primary(), std::nullopt, std::nullopt, LevelTransform::identity(), 0, 0);
		REQUIRE(scope.ok());
		const EbxInstance target = *partition.instance(r);
		const Result<std::optional<Property>> first = world.scopes.property(*scope, target, field_id("Excluded"));
		REQUIRE(first.ok());
		CHECK(world.scopes.capped_resolutions() >= 1);
		CHECK(world.scopes.hops() == (size_t(1) << 16));
		CHECK(world.budget->spent() == 4 + (size_t(1) << 16)); // the connection list, then the hops
		size_t resolutions = 1;
		const double seconds = timed([&] {
			for (; resolutions < 100000; ++resolutions) {
				if (!world.scopes.property(*scope, target, field_id("Excluded")).ok()) {
					break;
				}
			}
		});
		CHECK(resolutions == (WorkBudget::kDefaultLimit - 4) >> 16);
		CHECK(world.budget->exhausted());
		const Result<std::optional<Property>> after = world.scopes.property(*scope, target, field_id("Excluded"));
		REQUIRE_FALSE(after.ok());
		CHECK(budget_error(after.error()));
		CHECK(seconds < kHostileSeconds);
	}
}

TEST_SUITE("level") {
	TEST_CASE("level: Metro TeamDeathMatch0 (handoff §9)") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_Subway", "TeamDeathMatch0");
		const LoadedLevel &level = *t.level;
		const World &world = *t.world;
		CHECK(level.info.id == "MP_Subway");
		CHECK(level.info.display_name == "Operation Metro");
		CHECK(level.stats.partitions == 12315); // sections/03 §1: level + mode sub-world
		CHECK(level.stats.unresolved_imports == 0);
		CHECK(level.stats.failed.empty());
		REQUIRE(level.bundles.subworlds.size() == 1);
		CHECK(bundle_key(level.bundles.subworlds[0]) == "levels/mp_subway/teamdeathmatch");
		CHECK(world.objects.size() == 13280);
		CHECK(world.subworlds.size() == 2);
		CHECK(world.scopes.size() == 447);
		CHECK(world.stats.excluded == 9); // 8 of them through interface constants (gate door variants)
		CHECK(world.stats.excluded_overrides == 8);
		CHECK(world.stats.unresolved_objects == 0);
		CHECK(world.stats.depth_limited == 0);
		CHECK(world.stats.steps < (size_t(1) << 15));
		CHECK(world.stats.capped_resolutions == 0);
		CHECK(world.stats.work < WorkBudget::kDefaultLimit / 64); // load + walk: far below the budget
		CHECK(count_mesh_objects(world) == 10873);
		// Static model groups (sections/03 §2.3): the level's and the TDM sub-world's.
		REQUIRE(world.groups.size() == 2);
		const StaticModelGroup &main = world.groups[1];
		CHECK(main.members == 499);
		CHECK(main.instances == 7869);
		CHECK(main.explicit_count == 877);
		CHECK(main.havok_placed == 6992);
		CHECK(main.subparts == 6992);
		CHECK(main.part_mismatches == 0);
		CHECK(world.groups[0].instances == 20);
		// Composite placements that hide destroyed-state parts, and backdrops that start hidden
		// (sections/03 §2.4: backdrop_park + backdrop_city 42 + 22).
		CHECK(composite_hiding(level, world) == 710);
		CHECK(count_start_hidden(world) == 64);

		const MeshCensus census = census_of(level.db(), level.mount(), world);
		CHECK(census.failures.empty());
		CHECK(census.placements == 10873);
		CHECK(census.scene_meshes == 842);
		CHECK(census.model_meshes == 762);
		CHECK(census.world_meshes == 80);
		CHECK(census.scene_instances == 10571);
		CHECK(census.meshes.size() == 903);

		const std::vector<CollisionPlacement> collision = collision_of(level, world);
		size_t groups = 0, objects = 0, present = 0;
		for (const CollisionPlacement &c : collision) {
			groups += c.kind == CollisionPlacement::Kind::Group ? 1 : 0;
			objects += c.kind == CollisionPlacement::Kind::Object && !c.res.empty() ? 1 : 0;
			present += c.present ? 1 : 0;
		}
		CHECK(groups == 2);
		CHECK(objects == 2120);
		CHECK(present == groups + objects);
		// Everything this level x mode did (load, walk, census, collision, the checks above) used a
		// few percent of the one budget.
		CHECK(level.budget->spent() < WorkBudget::kDefaultLimit / 8);
		MESSAGE("Metro TDM work: load + walk ", world.stats.work, ", in all ", level.budget->spent(), " of ", level.budget->limit());
	}

	TEST_CASE("level: Grand Bazaar ConquestLarge0 (handoff §9)") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_001", "ConquestLarge0");
		const World &world = *t.world;
		CHECK(count_mesh_objects(world) == 5855);
		REQUIRE(world.groups.size() == 1);
		CHECK(world.groups[0].members == 381);
		CHECK(world.groups[0].instances == 3646);
		CHECK(world.groups[0].havok_placed == 3292);
		CHECK(world.groups[0].part_mismatches == 0);
		CHECK(composite_hiding(*t.level, world) == 368);
		// Vehicles of the mode (sections/04 / IW4L core report): 8 VehicleSpawnReferences.
		size_t vehicles = 0;
		for (const PlacedObject &o : world.objects) {
			vehicles += o.kind == ObjectKind::VehicleSpawnReference ? 1 : 0;
		}
		CHECK(vehicles == 8);

		const MeshCensus current = census_of(t.level->db(), t.level->mount(), world);
		CHECK(current.failures.empty());
		CHECK(current.scene_instances == 5783);
		CHECK(current.scene_meshes == 613); // empty-Materials entries fall back to the default
		CensusOptions legacy;
		legacy.legacy_variations = true;
		const MeshCensus handoff = census_of(t.level->db(), t.level->mount(), world, legacy);
		CHECK(handoff.scene_meshes == 620);
		CHECK(handoff.model_meshes == 579);
		CHECK(handoff.world_meshes == 41);
		CHECK(handoff.scene_instances == 5783);
	}

	TEST_CASE("level: Caspian Border ConquestSmall0") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_007", "ConquestSmall0");
		const World &world = *t.world;
		CHECK(t.level->info.display_name == "Caspian Border");
		CHECK(count_mesh_objects(world) == 6847);
		REQUIRE(world.groups.size() == 1);
		CHECK(world.groups[0].members == 170); // sections/03 §1 table
		CHECK(world.groups[0].instances == 4444);
		CHECK(world.groups[0].havok_placed == 4264);
		CHECK(composite_hiding(*t.level, world) == 538); // sections/03 §2.5
		const MeshCensus census = census_of(t.level->db(), t.level->mount(), world);
		CHECK(census.failures.empty());
		CHECK(census.placements == 6847);
		CHECK(census.scene_meshes == 304);
		CHECK(census.scene_instances == 6846);
		// Handoff §9's 20,311 / 2,277 / 12,725 are IW-engine scene counts (see the file header).
	}

	TEST_CASE("level: mutated level partitions fail or traverse, never crash") {
		// Seeded damage to the level's own partitions (LevelData, layers, sub-world, variation
		// databases): values that still pass the EBX validator reach the traversal, the census and
		// the collision placements (hostile counts, references, transforms, connection graphs).
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_Subway", "TeamDeathMatch0");
		const LoadedLevel &level = *t.level;
		std::vector<size_t> candidates;
		for (size_t i = 0; i < level.db().size(); ++i) {
			if (level.db().name_at(i).rfind("levels/mp_subway/", 0) == 0) {
				candidates.push_back(i);
			}
		}
		REQUIRE(candidates.size() > 10);
		size_t root_index = 0;
		for (size_t i = 0; i < level.db().size(); ++i) {
			if (level.db().name_at(i) == level.root_name) {
				root_index = i;
			}
		}
		constexpr uint32_t kSeeds = 96;
		size_t mutated = 0, traversed = 0, failed = 0;
		for (uint32_t seed = 0; seed < kSeeds; ++seed) {
			std::mt19937 rng(seed * 2654435761u + 17u);
			OverlayDb overlay(level.db());
			for (int pick = 0; pick < 4; ++pick) {
				const size_t index = (pick == 0 && seed % 2 == 0) ? root_index : candidates[rng() % candidates.size()];
				const EbxPartition &original = level.db().partition_at(index);
				std::vector<uint8_t> bytes(original.bytes().begin(), original.bytes().end());
				const size_t edits = 1 + rng() % 3;
				for (size_t e = 0; e < edits && bytes.size() > 96; ++e) {
					const size_t at = 80 + rng() % (bytes.size() - 84);
					static constexpr uint32_t kValues[] = { 0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x3F800000u, 0x7F800000u, 0x7FC00000u, 0xFF7FFFFFu };
					const uint32_t value = rng() % 3 == 0 ? uint32_t(rng()) : kValues[rng() % std::size(kValues)];
					std::memcpy(bytes.data() + at, &value, 4);
				}
				Result<EbxPartition> parsed = EbxPartition::parse(std::move(bytes));
				if (!parsed) {
					continue;
				}
				++mutated;
				overlay.add(level.db().name_at(index), std::make_unique<EbxPartition>(std::move(*parsed)));
			}
			const EbxPartition *root = overlay.partition_by_name(level.root_name);
			REQUIRE(root != nullptr);
			Result<std::unique_ptr<World>> world = World::traverse(overlay, root->primary(), level.mount(), level.mode);
			if (!world) {
				++failed;
				continue;
			}
			++traversed;
			CHECK((*world)->objects.size() < (size_t(1) << 23));
			const Result<std::vector<CollisionPlacement>> collision = collision_placements(overlay, level.mount(), **world);
			if (collision.ok()) {
				CHECK(collision->size() <= (*world)->objects.size() + (*world)->groups.size());
			} else {
				CHECK(collision.error().message.find("work steps") != std::string::npos); // only the budget fails it
			}
			if (seed % 8 == 0) {
				const Result<MeshCensus> census = mesh_census(overlay, level.mount(), **world);
				if (census.ok()) {
					CHECK(census->scene_instances <= census->placements + 2 * (*world)->objects.size());
				} else {
					CHECK(census.error().message.find("work steps") != std::string::npos);
				}
			}
		}
		MESSAGE("mutated partitions that parsed: ", mutated, ", traversals: ", traversed, " ok / ", failed, " failed");
		CHECK(mutated > 0);
		CHECK(traversed + failed == kSeeds);
	}

	TEST_CASE("level: Metro ConquestSmall0 composites (sections/03 \"Metro 702\")") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_Subway", "ConquestSmall0");
		CHECK(composite_hiding(*t.level, *t.world) == 702);
	}

	TEST_CASE("level: self-listing WorldPart fails on the step budget, never hangs") {
		// A WorldPartData listing a reference to itself twice: 2^48 visits that place nothing, so
		// only the step budget stops it. With one reference it is a chain cut at the depth limit.
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		Result<LevelMount> mount = install->mount_level("MP_Subway");
		REQUIRE(mount.ok());
		const auto run = [&](const std::vector<uint32_t> &p_objects, uint32_t p_members) {
			EbxDb db;
			Result<bool> inserted = db.parse_insert("levels/hostile/part", self_listing_part(p_objects, p_members));
			REQUIRE_MESSAGE(inserted.ok(), (inserted.ok() ? std::string() : inserted.error().message));
			const EbxPartition *part = db.partition_by_name("levels/hostile/part");
			REQUIRE(part != nullptr);
			REQUIRE(part->primary().is_a("WorldPartData"));
			return World::traverse(db, part->primary(), *mount, "TeamDeathMatch0");
		};
		{
			Result<std::unique_ptr<World>> world = run({ 1 }, 0);
			REQUIRE(world.ok());
			CHECK((*world)->objects.empty());
			CHECK((*world)->stats.depth_limited == 1);
		}
		{
			Result<std::unique_ptr<World>> world = run({ 1, 1 }, 0);
			REQUIRE_FALSE(world.ok());
			CHECK(world.error().message.find("steps") != std::string::npos);
		}
		{
			// With a group in the loop the 2^16 group cap is reached before the step budget.
			Result<std::unique_ptr<World>> world = run({ 1, 2, 1 }, 0);
			REQUIRE_FALSE(world.ok());
			CHECK(world.error().message.find("static model groups") != std::string::npos);
		}
	}

	TEST_CASE("level: static model group members cost steps, placed or not") {
		// Members that place nothing still cost their lookups, so each costs a step: one group with
		// 1,000 empty members takes at least 1,000 steps, and [self, self, group of 200,000 empty
		// members] fails on the step budget after about 20 group expansions instead of running for
		// hours (uncharged, it allowed about 2^22 expansions of O(members) work each).
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		Result<LevelMount> mount = install->mount_level("MP_Subway");
		REQUIRE(mount.ok());
		const auto run = [&](const std::vector<uint32_t> &p_objects, uint32_t p_members) {
			EbxDb db;
			Result<bool> inserted = db.parse_insert("levels/hostile/part", self_listing_part(p_objects, p_members));
			REQUIRE_MESSAGE(inserted.ok(), (inserted.ok() ? std::string() : inserted.error().message));
			const EbxPartition *part = db.partition_by_name("levels/hostile/part");
			REQUIRE(part != nullptr);
			return World::traverse(db, part->primary(), *mount, "TeamDeathMatch0");
		};
		{
			Result<std::unique_ptr<World>> world = run({ 2 }, 1000);
			REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
			REQUIRE((*world)->groups.size() == 1);
			CHECK((*world)->groups[0].members == 1000);
			CHECK((*world)->groups[0].instances == 0);
			CHECK((*world)->stats.steps >= 1000);
		}
		{
			Result<std::unique_ptr<World>> world = run({ 1, 1, 2 }, 200000);
			REQUIRE_FALSE(world.ok());
			CHECK(world.error().message.find("steps") != std::string::npos);
		}
	}

	TEST_CASE("level: hostile partition graphs end on the work budget, never hang") {
		// Each verifier repro and the generic shapes behind them, built as small synthetic partitions
		// and walked over Metro's mount (only static model group res would be read from it).
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		Result<LevelMount> mount = install->mount_level("MP_Subway");
		REQUIRE(mount.ok());
		const auto walk = [&](const EbxDb &p_db) { return World::traverse(p_db, p_db.partition_by_name("levels/hostile/part")->primary(), *mount, "TeamDeathMatch0"); };
		const auto expect_budget_error = [&](const char *p_what, const EbxDb &p_db) {
			Result<std::unique_ptr<World>> world = std::unique_ptr<World>();
			const double seconds = timed([&] { world = walk(p_db); });
			MESSAGE(std::string(p_what), ": ", (world.ok() ? std::string("ok") : world.error().message), " in ", seconds, " s");
			REQUIRE_FALSE(world.ok());
			CHECK(budget_error(world.error()));
			CHECK(seconds < kHostileSeconds);
		};
		const uint32_t f = 0x12345u, excluded = ebx_name_hash("Excluded");

		{
			// Round 3: Objects [R] x 10,000, R a WorldPartReferenceObjectData with no Blueprint whose
			// Excluded is fed by R.F, itself fed twice by R.F. Each excluded() check is a capped 2^16-hop
			// resolution; uncharged between them, N = 10,000 ran 84 s and succeeded with 655M hops.
			SynthPartition part(10);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t r = part.add("WorldPartReferenceObjectData");
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 10000, [&](uint32_t, uint32_t) { return SynthPartition::ref(r); }));
			part.set(root, "PropertyConnections",
					part.array("WorldPartData", "PropertyConnections", 3,
							connections({ { SynthPartition::ref(r), SynthPartition::ref(r), f, f }, { SynthPartition::ref(r), SynthPartition::ref(r), f, f },
									{ SynthPartition::ref(r), SynthPartition::ref(r), f, excluded } })));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			expect_budget_error("connection self-loops x 10,000", db);
		}
		{
			// K references to one world part whose Objects are N null slots (K x N slots).
			SynthPartition part(11);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t p = part.add("WorldPartReferenceObjectData");
			const uint32_t q = part.add("WorldPartData");
			part.set_ref(p, "Blueprint", q);
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(p); }));
			part.set(q, "Objects", part.array("WorldPartData", "Objects", 1u << 20));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			expect_budget_error("4,096 references to 2^20 null objects", db);
		}
		{
			// K entries into one sub-world whose MeshVariationDatabase lists N null entries: the
			// database is read (and charged) on every entry.
			SynthPartition part(12);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t s = part.add("SubWorldReferenceObjectData");
			part.set(s, "AutoLoad", 1);
			part.set_string(s, "BundleName", "levels/hostile/sub");
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(s); }));
			SynthPartition sub(13);
			hostile_types(sub);
			sub.add("WorldPartData");
			SynthPartition variations(14);
			hostile_types(variations);
			const uint32_t database = variations.add("MeshVariationDatabase");
			variations.set(database, "Entries", variations.array("MeshVariationDatabase", "Entries", 1u << 20));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			insert(db, "levels/hostile/sub", sub);
			insert(db, "levels/hostile/sub/meshvariationdb_win32", variations);
			expect_budget_error("4,096 sub-world entries, 2^20 variation slots", db);
		}
		{
			// A self-referencing loop through a prefab's interface: K references X to a prefab whose
			// object Y has Excluded <- Descriptor.F, and the level feeds X.F from X.F twice.
			SynthPartition part(15);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t x = part.add("ReferenceObjectData");
			const uint32_t prefab = part.add("PrefabBlueprint");
			const uint32_t descriptor = part.add("InterfaceDescriptorData");
			const uint32_t y = part.add("WorldPartReferenceObjectData");
			part.set_ref(x, "Blueprint", prefab);
			part.set_ref(prefab, "Descriptor", descriptor);
			part.set(prefab, "Objects", part.array("PrefabBlueprint", "Objects", 1, [&](uint32_t, uint32_t) { return SynthPartition::ref(y); }));
			part.set(prefab, "PropertyConnections",
					part.array("PrefabBlueprint", "PropertyConnections", 1, connections({ { SynthPartition::ref(descriptor), SynthPartition::ref(y), f, excluded } })));
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(x); }));
			part.set(root, "PropertyConnections",
					part.array("WorldPartData", "PropertyConnections", 2,
							connections({ { SynthPartition::ref(x), SynthPartition::ref(x), f, f }, { SynthPartition::ref(x), SynthPartition::ref(x), f, f } })));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			expect_budget_error("interface self-loop x 4,096 prefabs", db);
		}
		{
			// K placements whose entity lists one PartComponentData 4,096 times (HealthStates: N null
			// slots) and whose physics data lists N null rigid bodies: the walk is cheap, the census and
			// the collision placements run out of budget instead of scanning K x 4,096 x N slots.
			const auto build = [&](EbxDb &r_db) {
				SynthPartition part(16);
				hostile_types(part);
				const uint32_t root = part.add("WorldPartData");
				const uint32_t entity = part.add("StaticModelEntityData");
				const uint32_t asset = part.add("CompositeMeshAsset");
				const uint32_t component = part.add("PartComponentData");
				const uint32_t physics = part.add("PhysicsEntityData");
				part.set_ref(entity, "Mesh", asset);
				part.set_ref(entity, "PhysicsData", physics);
				part.set_string(asset, "Name", "levels/hostile/part");
				part.set(entity, "Components",
						part.array("StaticModelEntityData", "Components", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(component); }));
				part.set(component, "HealthStates", part.array("PartComponentData", "HealthStates", 1u << 20));
				part.set(physics, "RigidBodies", part.array("PhysicsEntityData", "RigidBodies", 1u << 20));
				part.set(root, "Objects", part.array("WorldPartData", "Objects", 64, [&](uint32_t, uint32_t) { return SynthPartition::ref(entity); }));
				insert(r_db, "levels/hostile/part", part);
			};
			EbxDb db;
			build(db);
			Result<std::unique_ptr<World>> census_world = walk(db);
			REQUIRE_MESSAGE(census_world.ok(), (census_world.ok() ? std::string() : census_world.error().message));
			CHECK((*census_world)->objects.size() == 64);
			Result<MeshCensus> census = MeshCensus();
			double seconds = timed([&] { census = mesh_census(db, *mount, **census_world); });
			REQUIRE_FALSE(census.ok());
			CHECK(budget_error(census.error()));
			CHECK(seconds < kHostileSeconds);
			CHECK_FALSE(starts_hidden(**census_world, (*census_world)->objects[0]).ok()); // the World's budget stays spent
			Result<std::unique_ptr<World>> collision_world = walk(db);
			REQUIRE(collision_world.ok());
			Result<std::vector<CollisionPlacement>> collision = std::vector<CollisionPlacement>();
			seconds = timed([&] { collision = collision_placements(db, *mount, **collision_world); });
			REQUIRE_FALSE(collision.ok());
			CHECK(budget_error(collision.error()));
			CHECK(seconds < kHostileSeconds);
		}
		{
			// Round 5, string length: Objects [S] x N, S a SubWorldReferenceObjectData with AutoLoad
			// false and an L-byte BundleName. Each slot decoded (memchr) and used the name for 1 unit:
			// N = 262,144, L = 4 MB ran 62.5 s and succeeded.
			SynthPartition part(17);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t s = part.add("SubWorldReferenceObjectData");
			part.set(s, "BundleName", part.string(std::string(size_t(4) << 20, 'n')));
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 1u << 18, [&](uint32_t, uint32_t) { return SynthPartition::ref(s); }));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			expect_budget_error("2^18 slots naming one 4 MB BundleName", db);
		}
		{
			// The fan-out form: Objects [R, R, S x 64] with R a world part reference back to the
			// root (2^48 paths, cut by the budget) and S as above with a 1 MB name (202 s before).
			SynthPartition part(18);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t r = part.add("WorldPartReferenceObjectData");
			const uint32_t s = part.add("SubWorldReferenceObjectData");
			part.set_ref(r, "Blueprint", root);
			part.set(s, "BundleName", part.string(std::string(size_t(1) << 20, 'n')));
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 66, [&](uint32_t p_i, uint32_t) { return SynthPartition::ref(p_i < 2 ? r : s); }));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			expect_budget_error("self-listing fan-out over a 1 MB BundleName", db);
		}
		{
			// Collision: 64 placements whose physics lists one asset with a 4 MB Name 4,096 times.
			// Names over 1,024 bytes are skipped, but only after they are charged.
			SynthPartition part(19);
			hostile_types(part);
			const uint32_t root = part.add("WorldPartData");
			const uint32_t entity = part.add("StaticModelEntityData");
			const uint32_t mesh = part.add("CompositeMeshAsset");
			const uint32_t physics = part.add("PhysicsEntityData");
			const uint32_t asset = part.add("CompositeMeshAsset");
			part.set_ref(entity, "Mesh", mesh);
			part.set_ref(entity, "PhysicsData", physics);
			part.set(asset, "Name", part.string(std::string(size_t(4) << 20, 'c')));
			part.set(physics, "ScaledAssets", part.array("PhysicsEntityData", "ScaledAssets", 4096, [&](uint32_t, uint32_t) { return SynthPartition::ref(asset); }));
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 64, [&](uint32_t, uint32_t) { return SynthPartition::ref(entity); }));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			Result<std::unique_ptr<World>> world = walk(db);
			REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
			Result<std::vector<CollisionPlacement>> collision = std::vector<CollisionPlacement>();
			const double seconds = timed([&] { collision = collision_placements(db, *mount, **world); });
			MESSAGE("collision over 64 x 4,096 4 MB names: ", (collision.ok() ? std::string("ok") : collision.error().message), " in ", seconds, " s");
			REQUIRE_FALSE(collision.ok());
			CHECK(budget_error(collision.error()));
			CHECK(seconds < kHostileSeconds);
		}
	}

	TEST_CASE("level: deep and wide type layouts cost what shallow ones do") {
		// Round 7: Objects [R] x 8, R.Blueprint the root (a fan-out the budget cuts), R a world part
		// reference 27 types deep whose BlueprintTransform is a LinearTransform 30 types deep with
		// rows 30 types deep, every type 32 descriptors wide. Each R slot reads Excluded, Blueprint,
		// BlueprintTransform and its 16 row fields; walking base chains, 254-wide types made one unit
		// cost about 130k descriptor steps (197 s for 360 KB). Field lookups are one search in a
		// table flattened at parse, so the deep form must run at the shallow form's rate.
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		Result<LevelMount> mount = install->mount_level("MP_Subway");
		REQUIRE(mount.ok());
		using F = SynthPartition::Field;
		constexpr size_t kLimit = size_t(1) << 20;
		const auto fan_out = [&](uint32_t p_seed, int p_depth, int p_pads) {
			SynthPartition part(p_seed);
			hostile_types(part);
			const std::string transform = padded_transform(part, p_depth, p_pads);
			const std::string base = padded_chain(part, "R", "WorldPartReferenceObjectData", ebx_code::kClass, p_depth == 0 ? 0 : 27, p_pads);
			part.declare("RTop", ebx_code::kClass, base, { F{ "BlueprintTransform", ebx_code::kValueType, transform } });
			const uint32_t root = part.add("WorldPartData");
			const uint32_t r = part.add("RTop");
			part.set_ref(r, "Blueprint", root);
			part.set(root, "Objects", part.array("WorldPartData", "Objects", 8, [&](uint32_t, uint32_t) { return SynthPartition::ref(r); }));
			EbxDb db;
			insert(db, "levels/hostile/part", part);
			auto budget = std::make_shared<WorkBudget>(kLimit);
			Result<std::unique_ptr<World>> world = std::unique_ptr<World>();
			const double seconds = timed([&] { world = World::traverse(db, db.partition_by_name("levels/hostile/part")->primary(), *mount, "TeamDeathMatch0", budget); });
			MESSAGE("fan-out, types ", p_depth, " deep x ", p_pads, " wide: ", (world.ok() ? std::string("ok") : world.error().message), " in ", seconds, " s");
			REQUIRE_FALSE(world.ok());
			CHECK(budget_error(world.error()));
			return seconds;
		};
		const double shallow = fan_out(40, 0, 0);
		const double deep = fan_out(41, 30, 32);
		CHECK(deep < 3 * shallow + 0.5);
		CHECK(deep < kHostileSeconds);

		// Objects [R, R, E x 64] with E a plain entity 29 types deep whose Transform is the deep
		// LinearTransform: every E is emitted, so the walk ends on the placed-object cap (103 s and
		// 302 MB at the old 2^20 cap with 254-wide types).
		SynthPartition part(42);
		hostile_types(part);
		const std::string transform = padded_transform(part, 30, 32);
		const std::string base = padded_chain(part, "E", "DataContainer", ebx_code::kClass, 29, 32);
		part.declare("ETop", ebx_code::kClass, base, { F{ "Transform", ebx_code::kValueType, transform } });
		const uint32_t root = part.add("WorldPartData");
		const uint32_t r = part.add("WorldPartReferenceObjectData");
		const uint32_t e = part.add("ETop");
		part.set_ref(r, "Blueprint", root);
		part.set(root, "Objects", part.array("WorldPartData", "Objects", 66, [&](uint32_t p_i, uint32_t) { return SynthPartition::ref(p_i < 2 ? r : e); }));
		EbxDb db;
		insert(db, "levels/hostile/part", part);
		Result<std::unique_ptr<World>> world = std::unique_ptr<World>();
		const double seconds = timed([&] { world = World::traverse(db, db.partition_by_name("levels/hostile/part")->primary(), *mount, "TeamDeathMatch0"); });
		MESSAGE("deep entities emitted: ", (world.ok() ? std::string("ok") : world.error().message), " in ", seconds, " s");
		REQUIRE_FALSE(world.ok());
		CHECK(world.error().message.find("placed objects") != std::string::npos);
		CHECK(seconds < kHostileSeconds);
	}

	TEST_CASE("level: modes and unknown levels") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		Result<LevelInfo> info = level_info(*install, "MP_Subway");
		REQUIRE(info.ok());
		CHECK(std::find(info->modes.begin(), info->modes.end(), "TeamDeathMatch0") != info->modes.end());
		CHECK(std::find(info->modes.begin(), info->modes.end(), "ConquestLarge0") != info->modes.end());
		CHECK_FALSE(load_level(install, "MP_999", "ConquestLarge0").ok());
		// A mode the level does not offer still loads the level with no mode sub-world.
		Result<std::unique_ptr<LoadedLevel>> none = load_level(install, "MP_Subway", "NoSuchMode0");
		REQUIRE(none.ok());
		CHECK((*none)->bundles.subworlds.empty());
	}
}

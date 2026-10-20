#include <doctest/doctest.h>

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/res/mesh_set.h"
#include "test_support.h"

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace bf3::core::res;

namespace {

struct Fnv {
	uint64_t h = 0xcbf29ce484222325ull;
	void bytes(const void *p_data, size_t p_len) {
		const uint8_t *b = static_cast<const uint8_t *>(p_data);
		for (size_t i = 0; i < p_len; i++) {
			h ^= b[i];
			h *= 0x00000100000001b3ull;
		}
	}
	void u16(uint16_t v) {
		const uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
		bytes(b, 2);
	}
	void u32(uint32_t v) {
		const uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
		bytes(b, 4);
	}
	void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
};

void put32(std::vector<uint8_t> &r_b, size_t p_at, uint32_t p_v) {
	for (int k = 0; k < 4; k++) {
		r_b[p_at + k] = uint8_t(p_v >> (8 * k));
	}
}
void put16(std::vector<uint8_t> &r_b, size_t p_at, uint16_t p_v) {
	r_b[p_at] = uint8_t(p_v);
	r_b[p_at + 1] = uint8_t(p_v >> 8);
}

std::vector<uint8_t> synthetic_mesh(ResMeta &r_meta) {
	std::vector<uint8_t> b(0x220, 0);
	put32(b, 0x08, 1);
	put32(b, 0x0C, 1);
	put32(b, 0x30, 0x70);
	put32(b, 0x58, 0x1B0);
	put32(b, 0x68, hash_quick("test/mesh"));
	std::memcpy(&b[0x1B0], "test/Mesh", 10);
	const size_t lod = 0x70;
	put32(b, lod + 0x04, 1);
	put32(b, lod + 0x08, 0x110);
	put32(b, lod + 0x10, 1);
	put32(b, lod + 0x14, 0x1A4);
	put32(b, lod + 0x40, 1);
	put32(b, lod + 0x48, 6);
	put32(b, lod + 0x4C, 24);
	const size_t sub = 0x110;
	put32(b, sub + 0x14, 1);
	put32(b, sub + 0x20, 3);
	b[sub + 0x24] = 8;
	b[sub + 0x25] = 3;
	for (int e = 0; e < 16; e++) {
		b[sub + 0x30 + 4 * e + 2] = 0xFF;
	}
	b[sub + 0x30] = 1;
	b[sub + 0x31] = 8;
	b[sub + 0x32] = 0;
	b[sub + 0x30 + 0x40] = 8;
	b[sub + 0x30 + 0x48] = 1;
	b[sub + 0x30 + 0x49] = 1;
	const uint16_t halves[3][4] = { { 0x3C00, 0x4000, 0xB800, 0x3C00 }, { 0, 0x3C00, 0, 0x3C00 }, { 0x4400, 0, 0x3555, 0x3C00 } };
	for (int v = 0; v < 3; v++) {
		for (int k = 0; k < 4; k++) {
			put16(b, 0x200 + 8 * v + 2 * k, halves[v][k]);
		}
	}
	put16(b, 0x218, 0);
	put16(b, 0x21A, 1);
	put16(b, 0x21C, 2);
	const uint32_t meta[4] = { 0x200, 32, 0, 0x00940070 };
	std::memcpy(r_meta.data(), meta, 16);
	return b;
}

struct Sample {
	std::string name;
	uint32_t res_type = 0;
	ResMeta meta{};
	uint32_t view = 0;
	std::vector<uint8_t> file;
	Bytes payload;
};

std::string data_dir() {
	char *value = nullptr;
	size_t len = 0;
	std::string out;
	if (_dupenv_s(&value, &len, "OPENBF3_RES_DATA") == 0 && value != nullptr) {
		out = value;
		std::free(value);
	}
	return out;
}

bool read_file(const std::string &p_path, std::vector<uint8_t> &r_out) {
	r_out.clear();
	FILE *f = nullptr;
	if (fopen_s(&f, p_path.c_str(), "rb") != 0 || f == nullptr) {
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long long size = _ftelli64(f);
	std::fseek(f, 0, SEEK_SET);
	r_out.resize(size_t(size));
	const size_t got = size > 0 ? std::fread(r_out.data(), 1, size_t(size), f) : 0;
	std::fclose(f);
	return got == size_t(size);
}

bool load_sample(const std::string &p_path, Sample &r_s) {
	if (!read_file(p_path, r_s.file) || r_s.file.size() < 44 || std::memcmp(r_s.file.data(), "BF3R", 4) != 0) {
		return false;
	}
	const uint8_t *b = r_s.file.data();
	auto u32 = [b](size_t at) { return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24); };
	r_s.res_type = u32(8);
	std::memcpy(r_s.meta.data(), b + 12, 16);
	r_s.view = u32(28);
	const size_t name_len = u32(32);
	if (36 + name_len + 8 > r_s.file.size()) {
		return false;
	}
	r_s.name.assign(reinterpret_cast<const char *>(b + 36), name_len);
	const size_t at = 36 + name_len;
	const uint64_t len = uint64_t(u32(at)) | (uint64_t(u32(at + 4)) << 32);
	if (at + 8 + len > r_s.file.size()) {
		return false;
	}
	r_s.payload = Bytes(b + at + 8, size_t(len));
	return true;
}

std::map<std::string, std::string> load_index(const std::string &p_data, const std::string &p_tag) {
	std::map<std::string, std::string> out;
	std::vector<uint8_t> index;
	if (!read_file(p_data + "/" + p_tag + "/index.tsv", index)) {
		return out;
	}
	std::string text(index.begin(), index.end());
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find('\n', at);
		if (end == std::string::npos) {
			end = text.size();
		}
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		const size_t tab1 = line.find('\t');
		const size_t tab2 = line.find('\t', tab1 + 1);
		if (tab1 != std::string::npos && tab2 != std::string::npos) {
			out[line.substr(tab1 + 1, tab2 - tab1 - 1)] = p_data + "/" + p_tag + "/res/" + line.substr(0, tab1);
		}
	}
	return out;
}

bool chunk(const std::string &p_data, const ChunkId &p_id, std::vector<uint8_t> &r_out) {
	return read_file(p_data + "/chunks/" + p_id.to_hex() + ".chunk", r_out);
}

bool data_or_skip(std::string &r_data) {
	r_data = data_dir();
	if (r_data.empty() || load_index(r_data, "subway").empty()) {
		bf3::test::skip_install_case("OPENBF3_RES_DATA does not point at an exported res sample");
		return false;
	}
	return true;
}

std::string lower(std::string p_text) {
	for (char &c : p_text) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
	return p_text;
}

uint32_t xorshift(uint32_t &r_state) {
	r_state ^= r_state << 13;
	r_state ^= r_state >> 17;
	r_state ^= r_state << 5;
	return r_state;
}

struct SyntheticHavok {
	std::vector<uint8_t> data;
	std::vector<std::pair<uint32_t, uint32_t>> arrays;
	std::vector<std::pair<uint32_t, uint32_t>> pointers;
	std::vector<std::pair<uint32_t, std::string>> objects;
	std::vector<uint32_t> roots;

	uint32_t alloc(size_t p_bytes) {
		const uint32_t at = uint32_t((data.size() + 15) / 16 * 16);
		data.resize(at + p_bytes, 0);
		return at;
	}
	uint32_t object(const char *p_class, size_t p_bytes) {
		const uint32_t at = alloc(p_bytes);
		objects.emplace_back(at, p_class);
		return at;
	}
	void array(uint32_t p_field, uint32_t p_elements, uint32_t p_count) {
		put32(data, p_field + 4, p_count);
		arrays.emplace_back(p_field, p_elements);
	}
	void pointer(uint32_t p_slot, uint32_t p_object) { pointers.emplace_back(p_slot, p_object); }

	uint32_t list(const std::vector<uint32_t> &p_children) {
		const uint32_t at = object("hkpListShape", 48);
		const uint32_t entries = alloc(16 * p_children.size());
		array(at + 24, entries, uint32_t(p_children.size()));
		for (size_t i = 0; i < p_children.size(); i++) {
			pointer(entries + uint32_t(16 * i), p_children[i]);
		}
		return at;
	}
	uint32_t box(float p_half) {
		const uint32_t at = object("hkpBoxShape", 48);
		for (int k = 0; k < 3; k++) {
			put32(data, at + 32 + 4 * k, std::bit_cast<uint32_t>(p_half));
		}
		return at;
	}

	std::vector<uint8_t> res(ResMeta &r_meta) {

		const uint32_t contents = alloc(16);
		const uint32_t variants = alloc(12);
		const uint32_t container = object("HavokPhysicsContainer", 32);
		const uint32_t shapes = alloc(4 * roots.size());
		array(contents, variants, 1);
		pointer(variants + 8, container);
		array(container + 8, shapes, uint32_t(roots.size()));
		for (size_t i = 0; i < roots.size(); i++) {
			pointer(shapes + uint32_t(4 * i), roots[i]);
		}
		alloc(16);

		std::vector<uint8_t> pack(160, 0);
		const uint8_t magic[8] = { 0x57, 0xE0, 0xE0, 0x57, 0x10, 0xC0, 0xC0, 0x10 };
		std::memcpy(pack.data(), magic, 8);
		pack[16] = 4;
		pack[17] = 1;
		put32(pack, 20, 2);
		put32(pack, 24, 1);
		put32(pack, 28, contents);
		std::memcpy(&pack[40], "hk_2010.2.0-r1", 14);

		const uint32_t names_at = uint32_t(pack.size());
		std::map<std::string, uint32_t> keys;
		for (const auto &[offset, cls] : objects) {
			if (!keys.contains(cls)) {
				const size_t at = pack.size();
				pack.resize(at + 5 + cls.size() + 1, 0);
				pack[at + 4] = 0x09;
				std::memcpy(&pack[at + 5], cls.data(), cls.size());
				keys[cls] = uint32_t(at + 5 - names_at);
			}
		}
		pack.push_back(0xFF);
		const uint32_t names_len = uint32_t(pack.size() - names_at);
		pack.resize((pack.size() + 15) / 16 * 16, 0);
		const uint32_t data_at = uint32_t(pack.size());
		pack.insert(pack.end(), data.begin(), data.end());
		for (const auto &[offset, cls] : objects) {
			const size_t at = pack.size();
			pack.resize(at + 12, 0);
			put32(pack, at, offset);
			put32(pack, at + 8, keys[cls]);
		}
		const uint32_t table_len = uint32_t(12 * objects.size());
		std::vector<uint8_t> fixups(8 * arrays.size() + 12 * pointers.size(), 0);
		for (size_t i = 0; i < arrays.size(); i++) {
			put32(fixups, 8 * i, arrays[i].first);
			put32(fixups, 8 * i + 4, arrays[i].second);
		}
		for (size_t i = 0; i < pointers.size(); i++) {
			put32(fixups, 8 * arrays.size() + 12 * i, pointers[i].first);
			put32(fixups, 8 * arrays.size() + 12 * i + 8, pointers[i].second);
		}
		auto section = [&](size_t p_index, const char *p_tag, const std::array<uint32_t, 7> &p_fields) {
			const size_t at = 64 + 48 * p_index;
			std::memcpy(&pack[at], p_tag, std::strlen(p_tag));
			for (size_t k = 0; k < 7; k++) {
				put32(pack, at + 20 + 4 * k, p_fields[k]);
			}
		};
		section(0, "__classnames__", { names_at, 0, 0, 0, 0, 0, names_len });
		section(1, "__data__", { data_at, uint32_t(fixups.size()), uint32_t(8 * arrays.size()), uint32_t(data.size()),
									   uint32_t(data.size()) + table_len, 0, 0 });

		std::vector<uint8_t> res(0x40, 0);
		put32(res, 0x08, 0x40);
		res.insert(res.end(), pack.begin(), pack.end());
		const size_t tail = res.size();
		res.resize(tail + 8, 0);
		put32(res, tail, uint32_t(fixups.size()));
		res.insert(res.end(), fixups.begin(), fixups.end());
		const uint32_t meta[4] = { 0x40, uint32_t(pack.size()), 0, uint32_t(8 + fixups.size()) };
		std::memcpy(r_meta.data(), meta, 16);
		return res;
	}
};

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

}

TEST_SUITE("res_unit") {
	TEST_CASE("res: halves, hashes, error texts") {
		CHECK(half_to_f32(0x3C00) == 1.0f);
		CHECK(half_to_f32(0xC000) == -2.0f);
		CHECK(half_to_f32(0x0001) == 5.9604645e-08f);
		CHECK(half_to_f32(0x7C00) > 1e30f);
		CHECK(hash_quick("") == 5381u);
		CHECK(hash_quick("wep_root") == 0xF7A6EE7Eu);
		CHECK(ResError::truncated(4, 8, 10).message() == "needed 8 bytes at 4 of a 10-byte payload");
		CHECK(ResError::malformed("packfile magic", 0).message() == "malformed packfile magic at 0");
		CHECK(ResError::unsupported("mesh type", 0).message() == "unsupported mesh type 0x0");
		CHECK(ResError::unsupported("lod count", 0x1F).message() == "unsupported lod count 0x1f");
	}

	TEST_CASE("res: synthetic MeshSet") {
		ResMeta meta;
		std::vector<uint8_t> b = synthetic_mesh(meta);
		MeshSet mesh;
		REQUIRE(MeshSet::parse(b, meta, mesh).ok());
		CHECK(mesh.kind == MeshKind::Rigid);
		CHECK(mesh.name == "test/Mesh");
		CHECK(mesh.name_hash == hash_quick(lower(mesh.name)));
		REQUIRE(mesh.lods.size() == 1);
		const MeshLod &lod = mesh.lods[0];
		CHECK(lod.geometry.is_inline);
		CHECK(lod.geometry.offset == 0x200);
		REQUIRE(lod.subsets.size() == 1);
		CHECK(lod.subsets[0].category == SubsetCategory::Opaque);
		Bytes geometry;
		REQUIRE(lod.inline_geometry(b, geometry).ok());
		DecodedSubset d;
		REQUIRE(lod.decode_subset(geometry, 0, d).ok());
		REQUIRE(d.positions.size() == 3);
		CHECK(d.positions[0] == Vec3{ 1.0f, 2.0f, -0.5f });
		CHECK(d.positions[2][0] == 4.0f);
		CHECK(d.indices == std::vector<uint32_t>{ 0, 1, 2 });
		CHECK(!d.normals);
		CHECK(!d.uv0);

		MeshSet cut;
		const ResError e = MeshSet::parse(Bytes(b.data(), 0x100), meta, cut);
		CHECK(e.kind == ResError::Kind::Truncated);
		CHECK(lod.decode_subset(geometry.first(20), 0, d).message() == "needed 24 bytes at 0 of a 20-byte payload");
		put16(b, 0x21C, 7);
		REQUIRE(lod.inline_geometry(b, geometry).ok());
		CHECK(lod.decode_subset(geometry, 0, d).message() == "malformed index beyond the subset's vertices at 7");
		std::vector<uint8_t> bad = b;
		put32(bad, 0x08, 6);
		CHECK(MeshSet::parse(bad, meta, cut).message() == "unsupported lod count 0x6");
		ResMeta bad_meta = meta;
		bad_meta[4] = 48;
		CHECK(MeshSet::parse(b, bad_meta, cut).message() == "malformed inline geometry size at 512");

		for (const uint8_t format : { uint8_t(8), uint8_t(0) }) {
			std::vector<uint8_t> hostile = b;
			put32(hostile, 0x110 + 0x20, 0xFFFFFFFFu);
			hostile[0x110 + 0x24] = 0;
			hostile[0x110 + 0x31] = format;
			MeshSet huge;
			REQUIRE(MeshSet::parse(hostile, meta, huge).ok());
			REQUIRE(huge.lods[0].inline_geometry(hostile, geometry).ok());
			CHECK(huge.lods[0].decode_subset(geometry, 0, d).message() == "malformed vertex element outside its stride at 0");
		}

		{
			MeshLod parts_lod;
			parts_lod.vertex_data_size = 4;
			MeshSubset sub;
			sub.vertex_count = 1;
			sub.vertex_stride = 4;
			sub.primitive_type = PrimitiveType::TriangleList;
			sub.declaration.element_count = 2;
			sub.declaration.elements[0] = { VertexUsage::Position, VertexFormat::Float, 0, 0 };
			sub.declaration.elements[1] = { VertexUsage::BoneIndices, VertexFormat::None, 4, 0 };
			sub.part_mask[0] = 1;
			parts_lod.subsets.push_back(sub);
			const std::vector<uint8_t> one = { 0x00, 0x00, 0x80, 0x3F };
			DecodedSubset pd;
			CHECK(parts_lod.decode_subset(one, 0, pd).message() == "unsupported part index vertex format 0x0");

			parts_lod.subsets[0].declaration.elements[1] = { VertexUsage::BoneIndices, VertexFormat::UByte4, 0, 0 };
			const std::vector<uint8_t> two = { 0x05, 0x00, 0x00, 0x00 };
			REQUIRE(parts_lod.decode_subset(two, 0, pd).ok());
			REQUIRE(pd.parts);
			CHECK(*pd.parts == std::vector<uint16_t>{ 5 });
		}
	}

	TEST_CASE("res: synthetic DxTexture, mips and ranged views") {
		std::vector<uint8_t> h(128, 0);
		put32(h, 0x00, 10);
		put32(h, 0x08, 0);
		put32(h, 0x0C, DxTexture::kFlagSrgb);
		put16(h, 0x10, 8);
		put16(h, 0x12, 8);
		put16(h, 0x14, 1);
		h[0x1A] = 2;
		h[0x1B] = 1;
		put32(h, 0x2C, 32);
		put32(h, 0x30, 8);
		put32(h, 0x68, 40);
		std::memcpy(&h[0x70], "World", 5);
		DxTexture t;
		REQUIRE(DxTexture::parse(h, t).ok());
		CHECK(t.is_srgb());
		CHECK(texture_pixel_format(t.format) == PixelFormat::Bc1);
		CHECK(t.texture_group == "World");
		CHECK(t.logical_offset(1) == 32);
		std::vector<uint8_t> chunk(40, 0);

		put16(chunk, 0, 0xF800);
		put16(chunk, 2, 0x001F);
		std::vector<MipSlice> mips;
		REQUIRE(t.split_mips(chunk, 0, mips).ok());
		REQUIRE(mips.size() == 2);
		CHECK(mips[0].row_pitch == 16);
		CHECK(mips[1].width == 4);
		std::vector<std::array<float, 4>> texels;
		REQUIRE(mips[0].texels(t.format, texels));
		REQUIRE(texels.size() == 64);
		CHECK(texels[0] == std::array<float, 4>{ 1.0f, 0.0f, 0.0f, 1.0f });

		REQUIRE(t.split_mips(Bytes(chunk).subspan(32), 32, mips).ok());
		REQUIRE(mips.size() == 1);
		CHECK(mips[0].mip == 1);
		CHECK(t.split_mips(chunk, 16, mips).message() == "malformed logical offset between mips at 16");
		CHECK(t.split_mips(Bytes(chunk).first(30), 0, mips).message() == "needed 32 bytes at 0 of a 30-byte payload");
		put32(h, 0x00, 9);
		CHECK(DxTexture::parse(h, t).message() == "unsupported texture version 0x9");

		const std::vector<uint8_t> tiny(16, 0x80);
		for (const TextureFormat format : { TextureFormat::L8, TextureFormat::Argb8888, TextureFormat::Dxt1, TextureFormat::Dxt5 }) {
			MipSlice slice;
			slice.width = 65535;
			slice.height = 65535;
			slice.data = tiny;
			CHECK(!slice.texels(format, texels));
			CHECK(texels.empty());
		}

		MipSlice small;
		small.width = 3;
		small.height = 2;
		small.data = Bytes(tiny).first(7);
		REQUIRE(small.texels(TextureFormat::L8, texels));
		CHECK(texels.size() == 6);
		small.data = Bytes(tiny).first(6);
		CHECK(!small.texels(TextureFormat::L8, texels));
	}

	TEST_CASE("res: convex tessellation and planes") {
		Shape box;
		box.type = ShapeType::Box;
		box.half_extents = { 1.0f, 2.0f, 3.0f };
		box.radius = 0.05f;
		const std::vector<Tri> tris = havok_triangles(box, Transform::identity());
		CHECK(tris.size() == 12);
		double volume = 0.0;
		for (const Tri &t : tris) {
			volume += vec::dot(t.v[0], vec::cross(t.v[1], t.v[2])) / 6.0;
		}
		CHECK(volume == doctest::Approx(48.0));
		const std::vector<ConvexPiece> pieces = havok_convex_pieces(box, Transform::from_translation({ 10.0f, 0.0f, 0.0f }));
		REQUIRE(pieces.size() == 1);
		CHECK(pieces[0].points.size() == 8);
		REQUIRE(pieces[0].planes.size() == 6);
		CHECK(pieces[0].planes[0] == Vec4{ 1.0f, 0.0f, 0.0f, -11.0f });
		CHECK(pieces[0].radius == 0.05f);

		Shape capsule;
		capsule.type = ShapeType::Capsule;
		capsule.a = { 0.0f, 0.0f, 0.0f };
		capsule.b = { 0.0f, 1.0f, 0.0f };
		capsule.radius = 0.5f;
		volume = 0.0;
		const std::vector<Tri> round = havok_triangles(capsule, Transform::identity());
		CHECK(round.size() > 50);
		for (const Tri &t : round) {
			volume += vec::dot(t.v[0], vec::cross(t.v[1], t.v[2])) / 6.0;
		}
		CHECK(volume > 0.9);
		CHECK(volume < 1.309);

		HavokPhysicsData hk;
		std::vector<uint8_t> junk(96, 0);
		CHECK(!HavokPhysicsData::parse(junk, ResMeta{}, hk).ok());
	}

	TEST_CASE("res: synthetic havok packfile, shared shapes") {

		SyntheticHavok b;
		const uint32_t box = b.box(0.5f);
		b.roots = { b.list({ box, box }) };
		ResMeta meta;
		const std::vector<uint8_t> res = b.res(meta);
		HavokPhysicsData hk;
		REQUIRE(HavokPhysicsData::parse(res, meta, hk).ok());
		CHECK(hk.packfile.version == "hk_2010.2.0-r1");
		std::vector<Shape> shapes;
		REQUIRE(hk.shapes(shapes).ok());
		REQUIRE(shapes.size() == 1);
		REQUIRE(shapes[0].type == ShapeType::List);
		REQUIRE(shapes[0].children.size() == 2);
		CHECK(shapes[0].children[1].type == ShapeType::Box);
		CHECK(havok_triangles(shapes[0], Transform::identity()).size() == 24);
		std::vector<SubpartTransform> group;
		CHECK(hk.group_subparts(group).ok());
		CHECK(group.empty());
	}

	TEST_CASE("res: hostile havok shape graphs fail fast") {

		auto run = [](SyntheticHavok &p_b, std::string &r_shapes_error, std::string &r_group_error) {
			ResMeta meta;
			const std::vector<uint8_t> res = p_b.res(meta);
			HavokPhysicsData hk;
			REQUIRE(HavokPhysicsData::parse(res, meta, hk).ok());
			std::vector<Shape> shapes;
			std::vector<SubpartTransform> group;
			r_shapes_error = hk.shapes(shapes).message();
			CHECK(shapes.empty());
			r_group_error = hk.group_subparts(group).message();
			CHECK(group.empty());
		};
		std::string se, ge;

		{
			const auto t0 = std::chrono::steady_clock::now();
			SyntheticHavok b;
			const uint32_t self = b.object("hkpListShape", 48);
			const uint32_t entries = b.alloc(32);
			b.array(self + 24, entries, 2);
			b.pointer(entries, self);
			b.pointer(entries + 16, self);
			b.roots = { self };
			run(b, se, ge);
			CHECK(se == "malformed shape cycle at " + std::to_string(self));
			CHECK(ge == "malformed shape cycle at " + std::to_string(self));
			CHECK(seconds_since(t0) < 5.0);
		}
		{
			SyntheticHavok b;
			const uint32_t a = b.object("hkpListShape", 48);
			const uint32_t a_entries = b.alloc(16);
			const uint32_t c = b.list({ a });
			b.array(a + 24, a_entries, 1);
			b.pointer(a_entries, c);
			const uint32_t mopp = b.object("hkpMoppBvTreeShape", 80);
			b.pointer(mopp + 68, a);
			b.roots = { mopp };
			run(b, se, ge);
			CHECK(se == "malformed shape cycle at " + std::to_string(a));
			CHECK(ge == "malformed shape cycle at " + std::to_string(a));
		}

		for (const uint32_t length : { 65u, 66u, 1000u }) {
			SyntheticHavok b;
			uint32_t next = b.box(1.0f);
			for (uint32_t i = 1; i < length; i++) {
				const uint32_t mopp = b.object("hkpMoppBvTreeShape", 80);
				b.pointer(mopp + 68, next);
				next = mopp;
			}
			b.roots = { next };
			ResMeta meta;
			const std::vector<uint8_t> res = b.res(meta);
			HavokPhysicsData hk;
			REQUIRE(HavokPhysicsData::parse(res, meta, hk).ok());
			std::vector<Shape> shapes;
			std::vector<SubpartTransform> group;
			const ResError e = hk.shapes(shapes);
			const ResError g = hk.group_subparts(group);
			if (length == 65) {
				CHECK(e.ok());
				CHECK(g.ok());
				CHECK(havok_triangles(shapes[0], Transform::identity()).size() == 12);
			} else {
				CHECK(e.message().rfind("malformed shape nesting at ", 0) == 0);
				CHECK(g.message().rfind("malformed shape nesting at ", 0) == 0);
			}
		}

		{
			const auto t0 = std::chrono::steady_clock::now();
			SyntheticHavok b;
			uint32_t next = b.box(1.0f);
			for (int level = 0; level < 12; level++) {
				next = b.list(std::vector<uint32_t>(16, next));
			}
			b.roots = { next };
			run(b, se, ge);
			CHECK(se.rfind("malformed shape tree size at ", 0) == 0);
			CHECK(ge.rfind("malformed shape tree size at ", 0) == 0);
			MESSAGE("wide DAG rejected in " << seconds_since(t0) << " s");
			CHECK(seconds_since(t0) < 20.0);
		}

		{
			const auto t0 = std::chrono::steady_clock::now();
			SyntheticHavok b;
			const uint32_t convex = b.object("hkpConvexVerticesShape", 96);
			const uint32_t blocks = b.alloc(48 * 256);
			b.array(convex + 64, blocks, 256);
			put32(b.data, convex + 76, 1024);
			b.roots = { b.list(std::vector<uint32_t>(4096, convex)) };
			run(b, se, ge);
			CHECK(se.rfind("malformed convex hull work at ", 0) == 0);
			CHECK(ge.empty());
			CHECK(seconds_since(t0) < 5.0);
		}
	}

	TEST_CASE("res: overlapping subsets and empty mips") {

		MeshLod lod;
		lod.vertex_data_size = 12;
		lod.index_data_size = 6;
		MeshSubset sub;
		sub.vertex_count = 3;
		sub.vertex_stride = 4;
		sub.primitive_count = 1;
		sub.primitive_type = PrimitiveType::TriangleList;
		sub.declaration.element_count = 1;
		sub.declaration.elements[0] = { VertexUsage::Position, VertexFormat::Float, 0, 0 };
		std::vector<uint8_t> geometry(18, 0);
		put16(geometry, 14, 1);
		put16(geometry, 16, 2);
		lod.subsets.assign(8, sub);
		std::vector<DecodedSubset> out;
		REQUIRE(lod.decode(geometry, SubsetFilter::All, out).ok());
		CHECK(out.size() == 8);
		lod.subsets.assign(9, sub);
		CHECK(lod.decode(geometry, SubsetFilter::All, out).message() == "malformed subset ranges overlap at 8");
		CHECK(out.empty());
		DecodedSubset one;
		CHECK(lod.decode_subset(geometry, 8, one).ok());

		std::vector<uint8_t> h(128, 0);
		put32(h, 0x00, 10);
		put16(h, 0x10, 8);
		put16(h, 0x12, 8);
		put16(h, 0x14, 1);
		h[0x1A] = 2;
		put32(h, 0x2C, 32);
		DxTexture t;
		REQUIRE(DxTexture::parse(h, t).ok());
		std::vector<uint8_t> chunk(40, 0);
		std::vector<MipSlice> mips;
		CHECK(t.split_mips(chunk, 0, mips).message() == "malformed empty mip at 1");
	}
}

TEST_SUITE("res") {
	TEST_CASE("res: whole sample decodes") {
		std::string data;
		if (!data_or_skip(data)) {
			return;
		}
		struct Expect {
			const char *tag;
			size_t mesh, tex, havok, destruction;
		};
		const Expect expect[] = { { "subway", 1341, 1717, 730, 9 }, { "skybar", 824, 1138, 253, 23 } };
		for (const Expect &x : expect) {
			const auto index = load_index(data, x.tag);
			if (index.empty()) {
				MESSAGE("no export for " << x.tag);
				continue;
			}
			std::map<uint32_t, size_t> counts;
			size_t name_hash_ok = 0, subsets = 0, triangles = 0, mips = 0, roots = 0;
			std::vector<uint8_t> bytes;
			for (const auto &[name, path] : index) {
				Sample s;
				REQUIRE(load_sample(path, s));
				counts[s.res_type]++;
				if (s.res_type == kResTypeMeshSet) {
					MeshSet mesh;
					REQUIRE_MESSAGE(MeshSet::parse(s.payload, s.meta, mesh).ok(), name);
					name_hash_ok += mesh.name_hash == hash_quick(lower(mesh.name)) ? 1 : 0;
					for (const MeshLod &lod : mesh.lods) {
						Bytes g;
						if (lod.geometry.is_inline) {
							REQUIRE(lod.inline_geometry(s.payload, g).ok());
						} else {
							REQUIRE(chunk(data, lod.geometry.chunk, bytes));
							g = Bytes(bytes);
						}
						std::vector<DecodedSubset> out;
						REQUIRE_MESSAGE(lod.decode(g, SubsetFilter::All, out).ok(), name);
						subsets += out.size();
						for (const DecodedSubset &d : out) {
							triangles += d.indices.size() / 3;
						}
					}
				} else if (s.res_type == kResTypeDxTexture) {
					DxTexture tex;
					REQUIRE_MESSAGE(DxTexture::parse(s.payload, tex).ok(), name);
					std::vector<MipSlice> slices;
					REQUIRE(chunk(data, tex.chunk_id, bytes));
					REQUIRE_MESSAGE(tex.split_mips(bytes, 0, slices).ok(), name);
					mips += slices.size();
					if (s.view != 0 && s.view != UINT32_MAX) {
						CHECK(tex.first_mip_at(s.view) >= 0);
						CHECK(tex.split_mips(Bytes(bytes).subspan(s.view), s.view, slices).ok());
					}
				} else {
					HavokPhysicsData hk;
					REQUIRE_MESSAGE(HavokPhysicsData::parse(s.payload, s.meta, hk).ok(), name);
					std::vector<Shape> shapes;
					REQUIRE_MESSAGE(hk.shapes(shapes).ok(), name);
					roots += shapes.size();
				}
			}
			CHECK(counts[kResTypeMeshSet] == x.mesh);
			CHECK(counts[kResTypeDxTexture] == x.tex);
			CHECK(counts[kResTypeHavokPhysicsData] == x.havok);
			CHECK(counts[kResTypeHavokDestructionPhysicsData] == x.destruction);
			CHECK(name_hash_ok == x.mesh);
			MESSAGE(std::string(x.tag) << ": " << subsets << " subsets, " << triangles << " triangles, " << mips << " mip images, " << roots << " havok roots");
		}
	}

	TEST_CASE("res: M9 and 1p arms skinned meshes") {
		std::string data;
		if (!data_or_skip(data)) {
			return;
		}
		const auto index = load_index(data, "subway");
		Sample s;
		REQUIRE(load_sample(index.at("weapons/m9/m9_1p_mesh"), s));
		MeshSet mesh;
		REQUIRE(MeshSet::parse(s.payload, s.meta, mesh).ok());
		CHECK(mesh.kind == MeshKind::Skinned);
		CHECK(mesh.name_hash == 0x4B2BE8E6u);
		REQUIRE(mesh.lods.size() == 1);
		const MeshLod &lod = mesh.lods[0];
		REQUIRE(lod.bones.size() == 5);
		CHECK(lod.bones[0].index == 1);
		CHECK(lod.bones[0].name_hash == hash_quick("wep_root"));
		CHECK(lod.bones[1].name_hash == hash_quick("wep_trigger"));
		CHECK(lod.bones[2].name_hash == hash_quick("wep_slide"));
		CHECK(lod.bones[3].name_hash == hash_quick("wep_mag"));
		REQUIRE(lod.subsets.size() == 2);
		CHECK(lod.subsets[1].is_z_only());
		std::vector<uint8_t> bytes;
		REQUIRE(chunk(data, lod.geometry.chunk, bytes));
		CHECK(bytes.size() == 302560);
		DecodedSubset d;
		REQUIRE(lod.decode_subset(bytes, 0, d).ok());
		CHECK(d.positions.size() == 4606);
		CHECK(d.indices.size() == 11970);
		Fnv pos, bi;
		for (const Vec3 &p : d.positions) {
			for (float x : p) {
				pos.f32(x);
			}
		}
		REQUIRE(d.bone_indices);
		REQUIRE(d.bone_weights);
		for (const auto &lanes : *d.bone_indices) {
			for (uint16_t x : lanes) {
				bi.u16(x);
			}
		}
		CHECK(pos.h == 0x77b4970e13982fe5ull);
		CHECK(bi.h == 0xcd7f6e35d791a4c5ull);

		size_t resolved = 0;
		for (size_t v = 0; v < d.positions.size(); v++) {
			const int64_t skeleton = lod.skeleton_bone(0, (*d.bone_indices)[v][0]);
			resolved += skeleton >= 0 && lod.find_bone(uint32_t(skeleton)) != nullptr ? 1 : 0;
		}
		CHECK(resolved == d.positions.size());

		REQUIRE(load_sample(index.at("characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh"), s));
		REQUIRE(MeshSet::parse(s.payload, s.meta, mesh).ok());
		REQUIRE(mesh.lods.size() == 1);
		CHECK(mesh.lods[0].bones.size() == 69);
		CHECK(mesh.lods[0].bones[0].index == 6);
		CHECK(mesh.lods[0].bones[0].name_hash == hash_quick("hips"));
		CHECK(mesh.lods[0].subsets.size() == 6);
	}

	TEST_CASE("res: texture and havok anchors") {
		std::string data;
		if (!data_or_skip(data)) {
			return;
		}
		const auto index = load_index(data, "subway");
		Sample s;
		REQUIRE(load_sample(index.at("weapons/m9/m9_d"), s));
		DxTexture tex;
		REQUIRE(DxTexture::parse(s.payload, tex).ok());
		CHECK(tex.format == TextureFormat::Dxt1);
		CHECK(tex.is_srgb());
		CHECK(tex.width == 2048);
		CHECK(tex.mip_count == 12);
		CHECK(tex.mip_base == 2);
		CHECK(tex.texture_group == "Weapon");
		CHECK(s.view == 2621440);
		CHECK(tex.first_mip_at(s.view) == 2);
		std::vector<uint8_t> bytes;
		REQUIRE(chunk(data, tex.chunk_id, bytes));
		std::vector<MipSlice> mips;
		REQUIRE(tex.split_mips(bytes, 0, mips).ok());
		REQUIRE(mips.size() == 12);
		Fnv m0;
		m0.bytes(mips[0].data.data(), mips[0].data.size());
		CHECK(m0.h == 0x17a315d5aec7c6adull);

		REQUIRE(load_sample(index.at("architecture/parisceiling_01/parisceiling_01_physics_0_win32"), s));
		HavokPhysicsData hk;
		REQUIRE(HavokPhysicsData::parse(s.payload, s.meta, hk).ok());
		CHECK(hk.packfile.version == "hk_2010.2.0-r1");
		CHECK(hk.material_flags_and_indices.size() == 3);
		std::vector<Shape> shapes;
		REQUIRE(hk.shapes(shapes).ok());
		REQUIRE(shapes.size() == 2);
		CHECK(shapes[0].type == ShapeType::ConvexTranslate);
		CHECK(shapes[0].children[0].type == ShapeType::Box);
		CHECK(shapes[1].type == ShapeType::MoppBvTree);
		CHECK(shapes[1].children[0].type == ShapeType::CompressedMesh);
		CHECK(havok_triangles(shapes[0], Transform::identity()).size() == 12);
		CHECK(havok_triangles(shapes[1], Transform::identity()).size() == 18);

		REQUIRE(load_sample(index.at("architecture/parisstairwell_01/parisstairwell_facade_02_destruction_physics_0_win32"), s));
		CHECK(s.res_type == kResTypeHavokDestructionPhysicsData);
		REQUIRE(HavokPhysicsData::parse(s.payload, s.meta, hk).ok());
		CHECK(hk.part_count == 13);
		REQUIRE(hk.shapes(shapes).ok());
		REQUIRE(shapes.size() == 1);
		CHECK(shapes[0].children[0].type == ShapeType::ExtendedMesh);
		CHECK(havok_triangles(shapes[0], Transform::identity()).size() == 1672);
		CHECK(havok_convex_pieces(shapes[0], Transform::identity()).size() == 142);
		std::vector<std::array<uint32_t, 2>> pairs;
		REQUIRE(hk.part_connections(pairs).ok());
		CHECK(pairs.size() == 22);
	}

	TEST_CASE("res: hostile havok storage subpart") {
		std::string data;
		if (!data_or_skip(data)) {
			return;
		}

		HavokPhysicsData good;
		uint32_t subpart = 0, mesh = 0;
		bool found = false;
		for (const auto &[name, path] : load_index(data, "subway")) {
			Sample s;
			good = HavokPhysicsData();
			if (!load_sample(path, s) || (s.res_type != kResTypeHavokPhysicsData && s.res_type != kResTypeHavokDestructionPhysicsData) ||
					!HavokPhysicsData::parse(s.payload, s.meta, good).ok()) {
				continue;
			}
			for (const PackObject &o : good.packfile.objects) {
				uint32_t storage_at = 0, storage_count = 0, triangles_at = 0, triangle_count = 0;
				if (o.class_name == "hkpStorageExtendedMeshShape" && good.packfile.array(o.offset + 240, storage_at, storage_count).ok() &&
						good.packfile.array(o.offset + 184, triangles_at, triangle_count).ok() && storage_count > 0 && triangle_count > 0 &&
						good.packfile.pointer(storage_at, mesh)) {
					subpart = triangles_at;
					found = true;
					break;
				}
			}
			if (found) {
				break;
			}
		}
		REQUIRE(found);
		if (!found) {
			return;
		}
		std::vector<Shape> shapes;
		REQUIRE(good.shapes(shapes).ok());
		const uint8_t striding = good.packfile.data[subpart + 46];
		REQUIRE((striding >= 1 && striding <= 3));
		const uint32_t width = striding == 1 ? 1u : (striding == 2 ? 2u : 4u);
		const uint32_t field = mesh + (striding == 1 ? 20u : (striding == 2 ? 32u : 44u));
		uint32_t indices_at = 0, index_count = 0;
		REQUIRE(good.packfile.array(field, indices_at, index_count).ok());
		REQUIRE(index_count >= 3);
		HavokPhysicsData bad = good;
		put32(bad.packfile.data, subpart + 20, 0x7FFFFFFFu);
		put16(bad.packfile.data, subpart + 44, 0);
		put32(bad.packfile.data, field + 4, 0x7FFFFFFFu);
		CHECK(bad.packfile.array(field, indices_at, index_count).message() == "malformed array count beyond data at " + std::to_string(field));
		CHECK(bad.shapes(shapes).message() == "malformed array count beyond data at " + std::to_string(field));
		CHECK(shapes.empty());

		REQUIRE(good.packfile.array(field, indices_at, index_count).ok());
		const uint32_t backed = uint32_t(bad.packfile.data.size() - indices_at);
		put32(bad.packfile.data, field + 4, backed);
		const std::string got = bad.shapes(shapes).message();
		if (width > 1) {
			CHECK(got.rfind("needed ", 0) == 0);
		} else {
			CHECK(got == "malformed triangle index beyond storage at " + std::to_string(backed));
		}

		size_t arrays = 0;
		for (uint32_t f = 0; f + 8 <= good.packfile.data.size(); f += 4) {
			uint32_t at = 0, count = 0;
			if (!good.packfile.array(f, at, count).ok() || count == 0) {
				continue;
			}
			arrays++;
			HavokPhysicsData probe = good;
			put32(probe.packfile.data, f + 4, 0x7FFFFFFFu);
			CHECK(!probe.packfile.array(f, at, count).ok());
			probe.shapes(shapes);
		}
		MESSAGE(arrays << " hostile array counts refused");
		CHECK(arrays > 3);
	}

	TEST_CASE("res: corrupted payloads fail cleanly") {
		std::string data;
		if (!data_or_skip(data)) {
			return;
		}

		const auto index = load_index(data, "subway");
		uint32_t seed = 0x9E3779B9u;
		std::map<uint32_t, size_t> taken;
		size_t runs = 0, failures = 0;
		std::vector<uint8_t> bytes;
		for (const auto &[name, path] : index) {
			Sample s;
			REQUIRE(load_sample(path, s));
			if (taken[s.res_type]++ >= 40 || s.payload.empty()) {
				continue;
			}
			const bool have_chunk = [&] {
				if (s.res_type == kResTypeDxTexture) {
					DxTexture t;
					return DxTexture::parse(s.payload, t).ok() && chunk(data, t.chunk_id, bytes);
				}
				return false;
			}();
			for (int round = 0; round < 64; round++) {
				std::vector<uint8_t> damaged(s.payload.begin(), s.payload.end());
				const int flips = 1 + int(xorshift(seed) % 4);
				for (int k = 0; k < flips; k++) {
					damaged[xorshift(seed) % damaged.size()] ^= uint8_t(1u << (xorshift(seed) % 8));
				}
				damaged.resize(damaged.size() - (xorshift(seed) % 3 == 0 ? xorshift(seed) % damaged.size() : 0));
				runs++;
				if (s.res_type == kResTypeMeshSet) {
					MeshSet mesh;
					if (!MeshSet::parse(damaged, s.meta, mesh).ok()) {
						failures++;
						continue;
					}
					for (const MeshLod &lod : mesh.lods) {
						Bytes g;
						if (lod.geometry.is_inline) {
							lod.inline_geometry(damaged, g);
						}
						std::vector<DecodedSubset> out;
						lod.decode(g, SubsetFilter::All, out);
					}
				} else if (s.res_type == kResTypeDxTexture) {
					DxTexture t;
					std::vector<MipSlice> mips;
					if (!DxTexture::parse(damaged, t).ok()) {
						failures++;
						continue;
					}
					if (have_chunk && t.split_mips(bytes, 0, mips).ok()) {
						std::vector<std::array<float, 4>> texels;
						mips.back().texels(t.format, texels);
					}
				} else {
					HavokPhysicsData hk;
					std::vector<Shape> shapes;
					if (!HavokPhysicsData::parse(damaged, s.meta, hk).ok() || !hk.shapes(shapes).ok()) {
						failures++;
						continue;
					}
					for (const Shape &shape : shapes) {
						havok_triangles(shape, Transform::identity());
					}
					std::vector<std::array<uint32_t, 2>> pairs;
					std::vector<SubpartTransform> group;
					hk.part_connections(pairs);
					hk.group_subparts(group);
				}
			}
		}
		MESSAGE(runs << " damaged decodes, " << failures << " rejected");
		CHECK(runs > 0);
	}
}

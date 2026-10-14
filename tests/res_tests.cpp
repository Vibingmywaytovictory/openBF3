// bf3::core::res decoders. Suite "res_unit": synthetic payloads, always run. Suite "res": the
// MeshSet / DxTexture / Havok sample exported from the user's install by the parity oracle
// (README "Resource decoders"), found through OPENBF3_RES_DATA; skipped (exit 77) without it.
// Asserts numbers and hashes only; no BF3 data lives in the repo.
#include <doctest/doctest.h>

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/res/mesh_set.h"
#include "test_support.h"

#include <bit>
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

// A rigid MeshSet: one LOD, one opaque subset of 3 Half4-position vertices (stride 8) and one
// u16 triangle, geometry inline at 0x200.
std::vector<uint8_t> synthetic_mesh(ResMeta &r_meta) {
	std::vector<uint8_t> b(0x220, 0);
	put32(b, 0x08, 1); // lodCount
	put32(b, 0x0C, 1);
	put32(b, 0x30, 0x70); // lods[0]
	put32(b, 0x58, 0x1B0); // name
	put32(b, 0x68, hash_quick("test/mesh"));
	std::memcpy(&b[0x1B0], "test/Mesh", 10);
	const size_t lod = 0x70;
	put32(b, lod + 0x04, 1); // subsets
	put32(b, lod + 0x08, 0x110);
	put32(b, lod + 0x10, 1); // Opaque: [0]
	put32(b, lod + 0x14, 0x1A4);
	put32(b, lod + 0x40, 1);
	put32(b, lod + 0x48, 6); // index bytes
	put32(b, lod + 0x4C, 24); // vertex bytes
	const size_t sub = 0x110;
	put32(b, sub + 0x14, 1); // one triangle
	put32(b, sub + 0x20, 3);
	b[sub + 0x24] = 8; // stride
	b[sub + 0x25] = 3; // triangle list
	for (int e = 0; e < 16; e++) {
		b[sub + 0x30 + 4 * e + 2] = 0xFF;
	}
	b[sub + 0x30] = 1; // Position
	b[sub + 0x31] = 8; // Half4
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

// ---------------------------------------------------------------------------------------------
// Sample data (exported by the oracle; see README)

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

// Res file paths of one exported level, by res name.
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

} // namespace

TEST_SUITE("res_unit") {
	TEST_CASE("res: halves, hashes, error texts") {
		CHECK(half_to_f32(0x3C00) == 1.0f);
		CHECK(half_to_f32(0xC000) == -2.0f);
		CHECK(half_to_f32(0x0001) == 5.9604645e-08f); // smallest subnormal
		CHECK(half_to_f32(0x7C00) > 1e30f);
		CHECK(hash_quick("") == 5381u);
		CHECK(hash_quick("wep_root") == 0xF7A6EE7Eu); // Wep_Root in the M9 skeleton
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

		// Damaged inputs: errors, never reads outside the spans.
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
		bad_meta[4] = 48; // inline size disagrees with the LOD
		CHECK(MeshSet::parse(b, bad_meta, cut).message() == "malformed inline geometry size at 512");
	}

	TEST_CASE("res: synthetic DxTexture, mips and ranged views") {
		std::vector<uint8_t> h(128, 0);
		put32(h, 0x00, 10);
		put32(h, 0x08, 0); // DXT1
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
		// Mip 0 block 0: colour0 red (0xF800) > colour1 blue, every index 0 -> opaque red.
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
		// A ranged view that starts at mip 1 (logicalOffset 32), and one between mips.
		REQUIRE(t.split_mips(Bytes(chunk).subspan(32), 32, mips).ok());
		REQUIRE(mips.size() == 1);
		CHECK(mips[0].mip == 1);
		CHECK(t.split_mips(chunk, 16, mips).message() == "malformed logical offset between mips at 16");
		CHECK(t.split_mips(Bytes(chunk).first(30), 0, mips).message() == "needed 32 bytes at 0 of a 30-byte payload");
		put32(h, 0x00, 9);
		CHECK(DxTexture::parse(h, t).message() == "unsupported texture version 0x9");
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
		CHECK(volume == doctest::Approx(48.0)); // closed, outward (counter-clockwise) faces
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
		CHECK(volume > 0.9); // low-poly capsule, inside the exact 1.309 m^3
		CHECK(volume < 1.309);

		HavokPhysicsData hk;
		std::vector<uint8_t> junk(96, 0);
		CHECK(!HavokPhysicsData::parse(junk, ResMeta{}, hk).ok());
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
					if (s.view != 0 && s.view != UINT32_MAX) { // ranged bundle view: logicalOffset = mips before firstMip
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
			CHECK(name_hash_ok == x.mesh); // MeshSet.nameHash == hashQuick(lowercase(name))
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
		// Every weighted lane resolves through the subset's slots to a bone of the table.
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
		CHECK(s.view == 2621440); // ranged in the level bundle: mips 2.. (logicalOffset = mip0 + mip1)
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

	TEST_CASE("res: corrupted payloads fail cleanly") {
		std::string data;
		if (!data_or_skip(data)) {
			return;
		}
		// Random byte damage on real payloads: every call must return (ok or an error) without
		// reading outside its spans. 40 res of each kind x 64 mutations.
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

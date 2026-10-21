#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/res/mesh_set.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/text.h"

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
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
	void bytes(Bytes p_b) { bytes(p_b.data(), p_b.size()); }
	void u8(uint8_t v) { bytes(&v, 1); }
	void u16(uint16_t v) {
		const uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
		bytes(b, 2);
	}
	void u32(uint32_t v) {
		const uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
		bytes(b, 4);
	}
	void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
	void f3(const Vec3 &v) {
		for (float x : v) {
			f32(x);
		}
	}
	void f4(const Vec4 &v) {
		for (float x : v) {
			f32(x);
		}
	}
	void xf(const Transform &t) {
		for (const Vec3 &r : t.axes()) {
			f3(r);
		}
		f3(t.trans);
	}
	std::string hex() const {
		char text[20];
		std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
		return text;
	}
};

using bf3::core::fmt;

std::string fb(float v) {
	return fmt("%08x", std::bit_cast<uint32_t>(v));
}
std::string f3s(const Vec3 &v) {
	return fb(v[0]) + "," + fb(v[1]) + "," + fb(v[2]);
}
std::string hexflag(uint32_t v) {
	return fmt("0x%x", v);
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

struct Res {
	std::string name;
	uint32_t res_type = 0;
	ResMeta meta{};
	uint32_t view = 0;
	std::vector<uint8_t> file;
	Bytes payload;
};

bool load_res(const std::string &p_path, Res &r_res) {
	if (!read_file(p_path, r_res.file) || r_res.file.size() < 44 || std::memcmp(r_res.file.data(), "BF3R", 4) != 0) {
		return false;
	}
	const uint8_t *b = r_res.file.data();
	auto u32 = [b](size_t at) { return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24); };
	r_res.res_type = u32(8);
	std::memcpy(r_res.meta.data(), b + 12, 16);
	r_res.view = u32(28);
	const size_t name_len = u32(32);
	if (36 + name_len + 8 > r_res.file.size()) {
		return false;
	}
	r_res.name.assign(reinterpret_cast<const char *>(b + 36), name_len);
	const size_t at = 36 + name_len;
	const uint64_t len = uint64_t(u32(at)) | (uint64_t(u32(at + 4)) << 32);
	if (at + 8 + len > r_res.file.size()) {
		return false;
	}
	r_res.payload = Bytes(b + at + 8, size_t(len));
	return true;
}

std::string g_data;
const bf3::core::LevelMount *g_mount = nullptr;

bool load_chunk(const ChunkId &p_id, std::vector<uint8_t> &r_out, uint32_t *r_view = nullptr) {
	if (g_mount != nullptr) {
		bf3::core::Result<RawChunk> chunk = read_raw_chunk(*g_mount, p_id);
		if (!chunk) {
			std::fprintf(stderr, "chunk %s: %s\n", p_id.to_hex().c_str(), chunk.error().message.c_str());
			r_out.clear();
			return false;
		}
		r_out = std::move(chunk->bytes);
		if (r_view != nullptr) {
			*r_view = chunk->view_offset;
		}
		return true;
	}
	return read_file(g_data + "/chunks/" + p_id.to_hex() + ".chunk", r_out);
}

bool load_install_res(const std::string &p_name, Res &r_res) {
	bf3::core::Result<RawRes> raw = read_raw_res(*g_mount, p_name);
	if (!raw) {
		std::fprintf(stderr, "res %s: %s\n", p_name.c_str(), raw.error().message.c_str());
		return false;
	}
	r_res.name = raw->name;
	r_res.res_type = raw->res_type;
	r_res.meta = raw->res_meta;
	r_res.view = UINT32_MAX;
	r_res.file = std::move(raw->payload);
	r_res.payload = Bytes(r_res.file.data(), r_res.file.size());
	return true;
}

template <typename T, typename F>
std::string opt_h(const std::optional<std::vector<T>> &p_v, F p_f) {
	if (!p_v) {
		return "-";
	}
	Fnv h;
	for (const T &x : *p_v) {
		p_f(h, x);
	}
	return h.hex();
}

void mesh_summary(std::string &o, const Res &r) {
	MeshSet mesh;
	const ResError pe = MeshSet::parse(r.payload, r.meta, mesh);
	if (!pe.ok()) {
		o += "mesh error=" + pe.message() + "\n";
		return;
	}
	o += fmt("mesh kind=%d flags=%s lods=%zu hash=%08x bmin=%s bmax=%s name=%s short=%s\n", int(mesh.kind), hexflag(mesh.flags).c_str(),
			mesh.lods.size(), mesh.name_hash, f3s(mesh.bounds_min).c_str(), f3s(mesh.bounds_max).c_str(), mesh.name.c_str(), mesh.short_name.c_str());
	for (size_t li = 0; li < mesh.lods.size(); li++) {
		const MeshLod &lod = mesh.lods[li];
		const std::string geom = lod.geometry.is_inline ? fmt("inline:%llu+%llu", (unsigned long long)lod.geometry.offset, (unsigned long long)lod.geometry.len)
														: "chunk:" + lod.geometry.chunk.to_hex();
		Fnv ch;
		for (const auto &list : lod.category_subsets) {
			ch.u32(uint32_t(list.size()));
			ch.bytes(list.data(), list.size());
		}
		o += fmt("lod %zu flags=%s ib=%d v=%u i=%u geom=%s subsets=%zu cats=%zu,%zu,%zu,%zu:%s hash=%08x name=%s short=%s\n", li,
				hexflag(lod.flags).c_str(), index_format_bytes(lod.index_format), lod.vertex_data_size, lod.index_data_size, geom.c_str(),
				lod.subsets.size(), lod.category_subsets[0].size(), lod.category_subsets[1].size(), lod.category_subsets[2].size(),
				lod.category_subsets[3].size(), ch.hex().c_str(), lod.name_hash, lod.name.c_str(), lod.short_name.c_str());
		if (!lod.parts.empty()) {
			Fnv h;
			for (const MeshPart &p : lod.parts) {
				h.f3(p.bounds_min);
				h.f3(p.bounds_max);
				h.xf(p.transform);
			}
			o += fmt("parts %zu %s\n", lod.parts.size(), h.hex().c_str());
		}
		if (!lod.bones.empty()) {
			Fnv h;
			std::string first;
			for (size_t k = 0; k < lod.bones.size(); k++) {
				h.u32(lod.bones[k].index);
				h.u32(lod.bones[k].name_hash);
				if (k < 4) {
					first += fmt("%s%u:%08x", k ? "," : "", lod.bones[k].index, lod.bones[k].name_hash);
				}
			}
			o += fmt("bones %zu %s first=%s\n", lod.bones.size(), h.hex().c_str(), first.c_str());
		}
		std::vector<uint8_t> chunk;
		Bytes geometry;
		bool have = false;
		if (lod.geometry.is_inline) {
			have = lod.inline_geometry(r.payload, geometry).ok();
		} else if (load_chunk(lod.geometry.chunk, chunk)) {
			geometry = Bytes(chunk.data(), chunk.size());
			have = true;
		}
		o += have ? fmt("geom len=%zu\n", geometry.size()) : std::string("geom missing\n");
		for (size_t si = 0; si < lod.subsets.size(); si++) {
			const MeshSubset &s = lod.subsets[si];
			Fnv sh, rh;
			for (uint16_t x : s.bone_slots) {
				sh.u16(x);
			}
			for (float x : s.tex_coord_ratios) {
				rh.f32(x);
			}
			const VertexDeclaration &d = s.declaration;
			std::string decl;
			for (size_t k = 0; k < 16; k++) {
				const VertexElement &e = d.elements[k];
				decl += fmt("%s%u:%u:%u:%u", k ? ";" : "", unsigned(e.usage), unsigned(e.format), unsigned(e.offset), unsigned(e.stream));
			}
			std::string pm;
			for (size_t k = 0; k < 6; k++) {
				pm += fmt("%s%x", k ? "," : "", s.part_mask[k]);
			}
			o += fmt("sub %zu mat=%u prim=%u pc=%u si=%u vo=%u vc=%u stride=%u bpv=%u slots=%zu:%s cat=%s pmask=%s decl=%s ec=%u sc=%u strides=%u,%u,%u,%u ratios=%s mname=%s\n",
					si, s.material_index, unsigned(s.primitive_type), s.primitive_count, s.start_index, s.vertex_offset, s.vertex_count,
					unsigned(s.vertex_stride), unsigned(s.bones_per_vertex), s.bone_slots.size(), sh.hex().c_str(),
					s.category ? fmt("%d", int(*s.category)).c_str() : "-", pm.c_str(), decl.c_str(), unsigned(d.element_count),
					unsigned(d.stream_count), unsigned(d.stream_strides[0]), unsigned(d.stream_strides[1]), unsigned(d.stream_strides[2]),
					unsigned(d.stream_strides[3]), rh.hex().c_str(), s.material_name.c_str());
			if (!have) {
				continue;
			}
			DecodedSubset dec;
			const ResError de = lod.decode_subset(geometry, si, dec);
			if (!de.ok()) {
				o += "dec error=" + de.message() + "\n";
				continue;
			}
			Fnv ph, ih;
			for (const Vec3 &p : dec.positions) {
				ph.f3(p);
			}
			for (uint32_t i : dec.indices) {
				ih.u32(i);
			}
			auto f3h = [](Fnv &h, const Vec3 &v) { h.f3(v); };
			auto f2h = [](Fnv &h, const Vec2 &v) {
				h.f32(v[0]);
				h.f32(v[1]);
			};
			auto idx_h = [](Fnv &h, const std::array<uint16_t, 4> &v) {
				for (uint16_t x : v) {
					h.u16(x);
				}
			};
			auto w_h = [](Fnv &h, const Vec4 &v) { h.f4(v); };
			std::string bones;
			if (dec.bone_indices) {
				bones += " bi=" + opt_h(dec.bone_indices, idx_h);
			}
			if (dec.bone_indices2) {
				bones += " bi2=" + opt_h(dec.bone_indices2, idx_h);
			}
			if (dec.bone_weights) {
				bones += " bw=" + opt_h(dec.bone_weights, w_h);
			}
			if (dec.bone_weights2) {
				bones += " bw2=" + opt_h(dec.bone_weights2, w_h);
			}
			o += fmt("dec n=%zu pos=%s nrm=%s tan=%s bs=%s uv0=%s uv1=%s uv2=%s rad=%s col=%s part=%s%s idx=%zu:%s\n", dec.positions.size(),
					ph.hex().c_str(), opt_h(dec.normals, f3h).c_str(), opt_h(dec.tangents, f3h).c_str(),
					opt_h(dec.binormal_signs, [](Fnv &h, float v) { h.f32(v); }).c_str(), opt_h(dec.uv0, f2h).c_str(), opt_h(dec.uv1, f2h).c_str(),
					opt_h(dec.uv2, f2h).c_str(), opt_h(dec.radiosity_uv, f2h).c_str(),
					opt_h(dec.colors, [](Fnv &h, const std::array<uint8_t, 4> &c) { h.bytes(c.data(), 4); }).c_str(),
					opt_h(dec.parts, [](Fnv &h, uint16_t v) { h.u16(v); }).c_str(), bones.c_str(), dec.indices.size(), ih.hex().c_str());
		}
	}
}

const char *pixel_name(PixelFormat p) {
	switch (p) {
		case PixelFormat::Bc1:
			return "bc1";
		case PixelFormat::Bc2:
			return "bc2";
		case PixelFormat::Bc3:
			return "bc3";
		case PixelFormat::Bc4:
			return "bc4";
		case PixelFormat::Bc5:
			return "bc5";
		case PixelFormat::Rgba8:
			return "rgba8";
		case PixelFormat::L8:
			return "l8";
		case PixelFormat::L16:
			return "l16";
		default:
			return "-";
	}
}

void texture_summary(std::string &o, const Res &r) {
	DxTexture tex;
	const ResError pe = DxTexture::parse(r.payload, tex);
	if (!pe.ok()) {
		o += "tex error=" + pe.message() + "\n";
		return;
	}
	std::string sizes;
	for (size_t k = 0; k < tex.mip_sizes.size(); k++) {
		sizes += fmt("%s%u", k ? "," : "", tex.mip_sizes[k]);
	}
	o += fmt("tex kind=%u fmt=%u pixel=%s normal=%d flags=%s srgb=%d %ux%ux%u slices=%u faces=%u mips=%u base=%u chunk=%s chain=%u hash=%08x group=%s sizes=%s\n",
			unsigned(tex.kind), unsigned(tex.format), pixel_name(texture_pixel_format(tex.format)), int(texture_format_is_normal_map(tex.format)),
			hexflag(tex.flags).c_str(), int(tex.is_srgb()), unsigned(tex.width), unsigned(tex.height), unsigned(tex.depth),
			unsigned(tex.slice_count), tex.faces(), unsigned(tex.mip_count), unsigned(tex.mip_base), tex.chunk_id.to_hex().c_str(),
			tex.mip_chain_size, tex.name_hash, tex.texture_group.c_str(), sizes.c_str());
	if (tex.chunk_id.is_zero()) {
		return;
	}
	std::vector<uint8_t> chunk;
	uint32_t view = r.view;
	if (!load_chunk(tex.chunk_id, chunk, &view)) {
		o += "chunk missing\n";
		return;
	}
	o += fmt("chunk len=%zu\n", chunk.size());
	const Bytes whole(chunk.data(), chunk.size());
	std::vector<MipSlice> mips;
	const ResError se = tex.split_mips(whole, 0, mips);
	if (!se.ok()) {
		o += "split error=" + se.message() + "\n";
	} else {
		std::vector<std::array<float, 4>> texels;
		for (const MipSlice &m : mips) {
			Fnv h;
			h.bytes(m.data);
			o += fmt("mip f=%u m=%u %ux%ux%u pitch=%u len=%zu %s\n", m.face, unsigned(m.mip), m.width, m.height, m.depth, m.row_pitch,
					m.data.size(), h.hex().c_str());
			if (m.face == 0 && m.width * m.height <= 65536 && m.texels(tex.format, texels)) {
				Fnv th;
				for (const auto &t : texels) {
					th.f4(t);
				}
				o += fmt("texels m=%u %s\n", unsigned(m.mip), th.hex().c_str());
			}
		}
	}
	if (view != UINT32_MAX && view != 0) {
		const uint64_t lo = view;
		const int first_mip = tex.first_mip_at(lo);
		const std::string first = first_mip < 0 ? std::string("-") : fmt("%d", first_mip);
		if (lo > chunk.size()) {
			o += fmt("view lo=%llu beyond chunk\n", (unsigned long long)lo);
		} else {
			const ResError ve = tex.split_mips(whole.subspan(size_t(lo)), lo, mips);
			if (!ve.ok()) {
				o += fmt("view lo=%llu first=%s error=%s\n", (unsigned long long)lo, first.c_str(), ve.message().c_str());
			} else {
				Fnv h;
				for (const MipSlice &m : mips) {
					h.u32(m.face);
					h.u8(m.mip);
					h.u32(uint32_t(m.data.size()));
					h.bytes(m.data);
				}
				o += fmt("view lo=%llu first=%s n=%zu %s\n", (unsigned long long)lo, first.c_str(), mips.size(), h.hex().c_str());
			}
		}
	}
}

char shape_letter(ShapeType t) {
	switch (t) {
		case ShapeType::MoppBvTree:
			return 'M';
		case ShapeType::List:
			return 'L';
		case ShapeType::ConvexTranslate:
			return 'T';
		case ShapeType::ConvexTransform:
			return 'X';
		case ShapeType::Box:
			return 'B';
		case ShapeType::Sphere:
			return 'S';
		case ShapeType::Capsule:
			return 'C';
		case ShapeType::Cylinder:
			return 'Y';
		case ShapeType::ConvexVertices:
			return 'V';
		case ShapeType::CompressedMesh:
			return 'Q';
		case ShapeType::ExtendedMesh:
			return 'E';
		default:
			return 'U';
	}
}

void shape_walk(const Shape &s, std::string &letters, size_t &nodes, Fnv &h) {
	nodes++;
	const char c = shape_letter(s.type);
	letters.push_back(c);
	h.u8(uint8_t(c));
	switch (s.type) {
		case ShapeType::MoppBvTree:
			shape_walk(s.children[0], letters, nodes, h);
			break;
		case ShapeType::List:
			h.u32(uint32_t(s.children.size()));
			for (const Shape &child : s.children) {
				shape_walk(child, letters, nodes, h);
			}
			for (uint32_t x : s.collision_filter_infos) {
				h.u32(x);
			}
			break;
		case ShapeType::ConvexTranslate:
			h.f3(s.translation);
			shape_walk(s.children[0], letters, nodes, h);
			break;
		case ShapeType::ConvexTransform:
			h.xf(s.transform);
			shape_walk(s.children[0], letters, nodes, h);
			break;
		case ShapeType::Box:
			h.f3(s.half_extents);
			h.f32(s.radius);
			h.u32(s.user_data);
			break;
		case ShapeType::Sphere:
			h.f32(s.radius);
			h.u32(s.user_data);
			break;
		case ShapeType::Capsule:
			h.f3(s.a);
			h.f3(s.b);
			h.f32(s.radius);
			h.u32(s.user_data);
			break;
		case ShapeType::Cylinder:
			h.f3(s.a);
			h.f3(s.b);
			h.f32(s.cylinder_radius);
			h.f32(s.radius);
			h.u32(s.user_data);
			break;
		case ShapeType::ConvexVertices:
			h.u32(uint32_t(s.points.size()));
			for (const Vec3 &p : s.points) {
				h.f3(p);
			}
			h.u32(uint32_t(s.planes.size()));
			for (const Vec4 &p : s.planes) {
				h.f4(p);
			}
			h.f32(s.radius);
			h.u32(s.user_data);
			break;
		case ShapeType::CompressedMesh: {
			const CompressedMesh &m = *s.compressed;
			h.f32(m.error);
			h.f3(m.bounds_min);
			h.f3(m.bounds_max);
			h.u32(uint32_t(m.big_vertices.size()));
			for (const Vec3 &p : m.big_vertices) {
				h.f3(p);
			}
			h.u32(uint32_t(m.big_triangles.size()));
			for (const BigTriangle &t : m.big_triangles) {
				for (uint16_t i : t.indices) {
					h.u16(i);
				}
				h.u32(t.part);
				h.u16(t.material);
				h.u32(uint32_t(t.transform_index));
			}
			h.u32(uint32_t(m.chunks.size()));
			for (const CompressedChunk &ck : m.chunks) {
				h.f3(ck.offset);
				h.u32(uint32_t(ck.vertices.size()));
				for (const auto &v : ck.vertices) {
					for (uint16_t x : v) {
						h.u16(x);
					}
				}
				h.u32(uint32_t(ck.indices.size()));
				for (uint16_t x : ck.indices) {
					h.u16(x);
				}
				h.u32(uint32_t(ck.strip_lengths.size()));
				for (uint16_t x : ck.strip_lengths) {
					h.u16(x);
				}
				h.u32(ck.part);
				h.u32(ck.material);
				h.u32(uint32_t(ck.reference));
				h.u32(uint32_t(ck.transform_index));
			}
			h.u32(uint32_t(m.transforms.size()));
			for (const Transform &t : m.transforms) {
				h.xf(t);
			}
			h.u32(m.convex_piece_count);
			break;
		}
		case ShapeType::ExtendedMesh: {
			const ExtendedMesh &m = *s.extended;
			h.u32(uint32_t(m.triangle_subparts.size()));
			for (const TrianglesSubpart &t : m.triangle_subparts) {
				h.xf(t.transform);
				h.u32(uint32_t(t.vertices.size()));
				for (const Vec3 &p : t.vertices) {
					h.f3(p);
				}
				h.u32(uint32_t(t.triangles.size()));
				for (const auto &tr : t.triangles) {
					for (uint32_t x : tr) {
						h.u32(x);
					}
				}
				h.u32(uint32_t(t.materials.size()));
				for (uint32_t x : t.materials) {
					h.u32(x);
				}
				h.u32(t.user_data);
			}
			h.u32(uint32_t(m.shape_subparts.size()));
			for (const ShapesSubpart &sp : m.shape_subparts) {
				h.f4(sp.rotation);
				h.f3(sp.translation);
				h.u32(sp.user_data);
				h.u32(uint32_t(sp.children.size()));
				for (const Shape &child : sp.children) {
					shape_walk(child, letters, nodes, h);
				}
			}
			break;
		}
		default:
			h.u32(uint32_t(s.class_name.size()));
			h.bytes(s.class_name.data(), s.class_name.size());
			break;
	}
}

void havok_summary(std::string &o, const Res &r) {
	HavokPhysicsData hk;
	const ResError pe = HavokPhysicsData::parse(r.payload, r.meta, hk);
	if (!pe.ok()) {
		o += "havok error=" + pe.message() + "\n";
		return;
	}
	Fnv th, ah, mh, fh, ch, dh;
	for (const Vec3 &p : hk.part_translations) {
		th.f3(p);
	}
	for (const auto &a : hk.local_aabbs) {
		ah.f3(a[0]);
		ah.f3(a[1]);
	}
	mh.bytes(hk.material_indices.data(), hk.material_indices.size());
	for (uint32_t x : hk.material_flags_and_indices) {
		fh.u32(x);
	}
	for (const PackObject &obj : hk.packfile.objects) {
		ch.u32(obj.offset);
		ch.bytes(obj.class_name.data(), obj.class_name.size());
		ch.u8(0);
	}
	dh.bytes(hk.packfile.data.data(), hk.packfile.data.size());
	o += fmt("havok parts=%u trans=%zu:%s aabbs=%zu:%s mats=%zu:%s mfi=%zu:%s scale=%s mcu=%u hmi=%u ver=%s objs=%zu:%s data=%zu:%s contents=%u\n",
			hk.part_count, hk.part_translations.size(), th.hex().c_str(), hk.local_aabbs.size(), ah.hex().c_str(), hk.material_indices.size(),
			mh.hex().c_str(), hk.material_flags_and_indices.size(), fh.hex().c_str(), fb(hk.scale).c_str(), unsigned(hk.material_count_used),
			unsigned(hk.highest_material_index), hk.packfile.version.c_str(), hk.packfile.objects.size(), ch.hex().c_str(),
			hk.packfile.data.size(), dh.hex().c_str(), hk.packfile.contents_offset);
	std::vector<Shape> shapes;
	const ResError se = hk.shapes(shapes);
	if (!se.ok()) {
		o += "roots error=" + se.message() + "\n";
	} else {
		o += fmt("roots %zu\n", shapes.size());
		for (size_t i = 0; i < shapes.size(); i++) {
			std::string letters;
			size_t nodes = 0;
			Fnv h;
			shape_walk(shapes[i], letters, nodes, h);
			if (letters.size() > 48) {
				letters.resize(48);
				letters += "...";
			}
			const std::vector<Tri> tris = havok_triangles(shapes[i], Transform::identity());
			Fnv tri_h;
			for (const Tri &t : tris) {
				for (const Vec3 &v : t.v) {
					tri_h.f3(v);
				}
				tri_h.u32(t.material);
				tri_h.u32(t.part);
				tri_h.u32(t.user_data);
			}
			const std::vector<ConvexPiece> pieces = havok_convex_pieces(shapes[i], Transform::identity());
			Fnv cp_h;
			for (const ConvexPiece &p : pieces) {
				cp_h.u32(uint32_t(p.points.size()));
				for (const Vec3 &v : p.points) {
					cp_h.f3(v);
				}
				cp_h.u32(uint32_t(p.planes.size()));
				for (const Vec4 &v : p.planes) {
					cp_h.f4(v);
				}
				cp_h.f32(p.radius);
				cp_h.u32(p.part);
				cp_h.u32(p.user_data);
			}
			o += fmt("root %zu nodes=%zu tree=%s th=%s tris=%zu:%s convex=%zu:%s\n", i, nodes, letters.c_str(), h.hex().c_str(), tris.size(),
					tri_h.hex().c_str(), pieces.size(), cp_h.hex().c_str());
		}
	}
	std::vector<std::array<uint32_t, 2>> conn;
	const ResError ce = hk.part_connections(conn);
	if (!ce.ok()) {
		o += "conn error=" + ce.message() + "\n";
	} else if (!conn.empty()) {
		Fnv h;
		for (const auto &p : conn) {
			h.u32(p[0]);
			h.u32(p[1]);
		}
		o += fmt("conn %zu:%s\n", conn.size(), h.hex().c_str());
	}
	std::vector<SubpartTransform> group;
	const ResError ge = hk.group_subparts(group);
	if (!ge.ok()) {
		o += "group error=" + ge.message() + "\n";
	} else if (!group.empty()) {
		Fnv h;
		for (const SubpartTransform &s : group) {
			h.f4(s.rotation);
			h.f3(s.translation);
			h.u32(s.user_data);
		}
		o += fmt("group %zu:%s\n", group.size(), h.hex().c_str());
	}
}

double decode_only(const Res &r) {
	using Clock = std::chrono::steady_clock;
	std::vector<std::vector<uint8_t>> chunks;
	if (r.res_type == kResTypeMeshSet) {
		MeshSet mesh;
		if (MeshSet::parse(r.payload, r.meta, mesh).ok()) {
			for (const MeshLod &lod : mesh.lods) {
				chunks.emplace_back();
				if (!lod.geometry.is_inline) {
					load_chunk(lod.geometry.chunk, chunks.back());
				}
			}
		}
	} else if (r.res_type == kResTypeDxTexture) {
		DxTexture tex;
		chunks.emplace_back();
		if (DxTexture::parse(r.payload, tex).ok() && !tex.chunk_id.is_zero()) {
			load_chunk(tex.chunk_id, chunks.back());
		}
	}
	const auto t0 = Clock::now();
	if (r.res_type == kResTypeMeshSet) {
		MeshSet mesh;
		if (MeshSet::parse(r.payload, r.meta, mesh).ok()) {
			std::vector<DecodedSubset> out;
			for (size_t i = 0; i < mesh.lods.size(); i++) {
				const MeshLod &lod = mesh.lods[i];
				Bytes geometry(chunks[i].data(), chunks[i].size());
				if (lod.geometry.is_inline) {
					lod.inline_geometry(r.payload, geometry);
				}
				lod.decode(geometry, SubsetFilter::All, out);
			}
		}
	} else if (r.res_type == kResTypeDxTexture) {
		DxTexture tex;
		std::vector<MipSlice> mips;
		if (DxTexture::parse(r.payload, tex).ok()) {
			tex.split_mips(Bytes(chunks[0].data(), chunks[0].size()), 0, mips);
		}
	} else {
		HavokPhysicsData hk;
		std::vector<Shape> shapes;
		if (HavokPhysicsData::parse(r.payload, r.meta, hk).ok() && hk.shapes(shapes).ok()) {
			for (const Shape &s : shapes) {
				havok_triangles(s, Transform::identity());
			}
		}
	}
	return std::chrono::duration<double>(Clock::now() - t0).count();
}

void trunc_report(std::string &o, const Res &r) {
	o += fmt("== %s %08x\n", r.name.c_str(), r.res_type);
	const size_t len = r.payload.size();
	std::vector<size_t> cuts{ len / 4, len / 2, 3 * len / 4, len > 0 ? len - 1 : 0 };
	cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
	for (size_t cut : cuts) {
		const Bytes p = r.payload.first(cut);
		ResError e;
		std::string ok;
		if (r.res_type == kResTypeMeshSet) {
			MeshSet m;
			e = MeshSet::parse(p, r.meta, m);
			ok = fmt("lods=%zu", m.lods.size());
		} else if (r.res_type == kResTypeDxTexture) {
			DxTexture t;
			e = DxTexture::parse(p, t);
			ok = fmt("mips=%u", unsigned(t.mip_count));
		} else {
			HavokPhysicsData h;
			std::vector<Shape> shapes;
			e = HavokPhysicsData::parse(p, r.meta, h);
			if (e.ok()) {
				e = h.shapes(shapes);
			}
			ok = fmt("roots=%zu", shapes.size());
		}
		o += e.ok() ? fmt("cut %zu ok %s\n", cut, ok.c_str()) : fmt("cut %zu error=%s\n", cut, e.message().c_str());
	}
	if (r.res_type == kResTypeMeshSet) {
		MeshSet mesh;
		if (!MeshSet::parse(r.payload, r.meta, mesh).ok()) {
			return;
		}
		for (size_t li = 0; li < mesh.lods.size(); li++) {
			const MeshLod &lod = mesh.lods[li];
			std::vector<uint8_t> chunk;
			Bytes g;
			if (lod.geometry.is_inline) {
				if (!lod.inline_geometry(r.payload, g).ok()) {
					continue;
				}
			} else if (load_chunk(lod.geometry.chunk, chunk)) {
				g = Bytes(chunk.data(), chunk.size());
			} else {
				continue;
			}
			for (size_t cut : { g.size() / 2, std::min<size_t>(lod.vertex_data_size, g.size()) }) {
				std::string parts;
				for (size_t si = 0; si < lod.subsets.size(); si++) {
					DecodedSubset d;
					const ResError e = lod.decode_subset(g.first(cut), si, d);
					parts += si ? "|" : "";
					parts += e.ok() ? fmt("ok%zu", d.indices.size()) : e.message();
				}
				o += fmt("lod %zu cut %zu: %s\n", li, cut, parts.c_str());
			}
		}
	} else if (r.res_type == kResTypeDxTexture) {
		DxTexture tex;
		std::vector<uint8_t> chunk;
		if (!DxTexture::parse(r.payload, tex).ok() || tex.chunk_id.is_zero() || !load_chunk(tex.chunk_id, chunk)) {
			return;
		}
		const Bytes whole(chunk.data(), chunk.size());
		std::vector<MipSlice> mips;
		const size_t cut = chunk.size() / 2;
		ResError e = tex.split_mips(whole.first(cut), 0, mips);
		o += e.ok() ? fmt("split cut %zu ok %zu\n", cut, mips.size()) : fmt("split cut %zu error=%s\n", cut, e.message().c_str());
		const uint64_t lo = tex.logical_offset(1);
		if (lo < chunk.size()) {
			e = tex.split_mips(whole.subspan(size_t(lo)), lo + 4, mips);
			o += e.ok() ? fmt("split lo+4 ok %zu\n", mips.size()) : fmt("split lo+4 error=%s\n", e.message().c_str());
		}
	}
}

const char *kBoneNames[] = { "Reference", "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head", "HeadEnd",
	"LeftShoulder", "LeftArm", "LeftArmRoll", "LeftForeArm", "LeftForeArmRoll", "LeftHand", "LeftHandThumb1", "LeftHandThumb2",
	"LeftHandThumb3", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", "LeftHandMiddle1", "LeftHandRing1", "LeftHandPinky1",
	"RightShoulder", "RightArm", "RightArmRoll", "RightForeArm", "RightForeArmRoll", "RightHand", "RightHandThumb1",
	"RightHandIndex1", "RightHandMiddle1", "RightHandRing1", "RightHandPinky1", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
	"RightUpLeg", "RightLeg", "RightFoot", "RightToeBase", "Wep_Root", "Wep_Trigger", "Wep_Slide", "Wep_Mag", "Wep_Bolt",
	"Wep_Hammer", "Wep_Grip", "Wep_Mag_Release", "Wep_Belt", "Wep_Scope", "Wep_Aim", "Wep_Barrel", "Wep_Extra1", "Wep_Bullet1",
	"Camera", "CameraBase", "CameraJoint", "IK_Joint_LeftHand", "IK_Joint_RightHand", "Trajectory", "TrajectoryEnd", "AITrajectory",
	"Wep_Muzzle", "Wep_Stock", "Wep_Charge", "Wep_Safety", "Wep_SlideStop", "Wep_Magazine", "Wep_Gizmo", "Wep_Root_Gizmo" };

void skin_report(std::string &o, const Res &r) {
	if (r.res_type != kResTypeMeshSet) {
		return;
	}
	MeshSet mesh;
	if (!MeshSet::parse(r.payload, r.meta, mesh).ok() || mesh.kind != MeshKind::Skinned) {
		return;
	}
	o += fmt("skin %s lods=%zu\n", r.name.c_str(), mesh.lods.size());
	for (size_t li = 0; li < mesh.lods.size(); li++) {
		const MeshLod &lod = mesh.lods[li];
		uint32_t max_skel = 0;
		for (const SkeletonBone &b : lod.bones) {
			max_skel = std::max(max_skel, b.index);
		}
		std::string named;
		for (const SkeletonBone &b : lod.bones) {
			for (const char *n : kBoneNames) {
				const std::string lower = bf3::core::to_lower_ascii(n);
				if (hash_quick(n) == b.name_hash) {
					named += fmt(" %u=%s", b.index, n);
				} else if (hash_quick(lower) == b.name_hash) {
					named += fmt(" %u=lc:%s", b.index, n);
				}
			}
		}
		size_t slot_not_in_table = 0, max_slot_value = 0, max_lane = 0, bad_slot = 0, weight_not_one = 0, vertices = 0;
		std::string bpv, formats;
		std::vector<uint8_t> chunk;
		Bytes g;
		bool have = lod.geometry.is_inline ? lod.inline_geometry(r.payload, g).ok() : load_chunk(lod.geometry.chunk, chunk);
		if (!lod.geometry.is_inline && have) {
			g = Bytes(chunk.data(), chunk.size());
		}
		for (size_t si = 0; si < lod.subsets.size(); si++) {
			const MeshSubset &s = lod.subsets[si];
			for (uint16_t v : s.bone_slots) {
				max_slot_value = std::max<size_t>(max_slot_value, v);
				bool in_table = false;
				for (const SkeletonBone &b : lod.bones) {
					in_table = in_table || b.index == v;
				}
				slot_not_in_table += in_table ? 0 : 1;
			}
			bpv += fmt("%s%u", si ? "," : "", unsigned(s.bones_per_vertex));
			const VertexElement *bi = s.declaration.find(VertexUsage::BoneIndices);
			const VertexElement *bw = s.declaration.find(VertexUsage::BoneWeights);
			if (si == 0) {
				formats = fmt("bi=%d bw=%d", bi ? int(bi->format) : -1, bw ? int(bw->format) : -1);
			}
			DecodedSubset d;
			if (!have || !lod.decode_subset(g, si, d).ok() || !d.bone_indices) {
				continue;
			}
			for (size_t v = 0; v < d.positions.size(); v++) {
				vertices++;
				float sum = 0.0f;
				for (size_t k = 0; k < 4; k++) {
					const float w = d.bone_weights ? (*d.bone_weights)[v][k] : (k == 0 ? 1.0f : 0.0f);
					sum += w;
					if (w > 0.0f) {
						const uint16_t slot = (*d.bone_indices)[v][k];
						max_lane = std::max<size_t>(max_lane, slot);
						if (slot >= s.bone_slots.size()) {
							bad_slot++;
						}
					}
				}
				if (std::abs(sum - 1.0f) > 0.01f) {
					weight_not_one++;
				}
			}
		}
		o += fmt("  lod %zu bones=%zu max_skeleton_index=%u slots_max_value=%zu max_weighted_lane=%zu lane_beyond_slots=%zu slot_not_in_table=%zu verts=%zu weight_sum_off=%zu bpv=%s %s named:%s\n",
				li, lod.bones.size(), max_skel, max_slot_value, max_lane, bad_slot, slot_not_in_table, vertices, weight_not_one, bpv.c_str(), formats.c_str(),
				named.c_str());
	}
}

const char *type_label(uint32_t p_type) {
	switch (p_type) {
		case kResTypeMeshSet:
			return "MeshSet";
		case kResTypeDxTexture:
			return "DxTexture";
		case kResTypeHavokPhysicsData:
			return "HavokPhysicsData";
		default:
			return "HavokDestructionPhysicsData";
	}
}

struct Section {
	std::string tag;
	std::unique_ptr<bf3::core::LevelMount> mount;
	std::vector<std::string> keys;
};

bool load_item(const Section &p_section, const std::string &p_key, Res &r_res) {
	g_mount = p_section.mount.get();
	return g_mount != nullptr ? load_install_res(p_key, r_res) : load_res(p_key, r_res);
}

}

int main(int argc, char **argv) {
	int arg = 1;
	bool time_pass = false;
	bool trunc = false;
	bool skin = false;
	if (arg < argc && std::strcmp(argv[arg], "--time") == 0) {
		time_pass = true;
		arg++;
	} else if (arg < argc && std::strcmp(argv[arg], "--trunc") == 0) {
		trunc = true;
		arg++;
	} else if (arg < argc && std::strcmp(argv[arg], "--skin") == 0) {
		skin = true;
		arg++;
	}
	const bool install_mode = arg < argc && std::strcmp(argv[arg], "--install") == 0;
	if (install_mode) {
		arg++;
	}
	if (argc - arg < 2 || (install_mode && (argc - arg) % 2 != 0)) {
		std::fprintf(stderr, "usage: resdump [--time | --trunc | --skin] <data_dir> <tag>...\n"
							 "       resdump [--time | --trunc | --skin] --install <superbundle> <tag> [<superbundle> <tag>]...\n");
		return 2;
	}
	_setmode(_fileno(stdout), _O_BINARY);
	std::vector<Section> sections;
	std::shared_ptr<bf3::core::Install> install;
	if (install_mode) {
		bf3::core::InstallReader reader;
		if (!reader.locate({})) {
			std::fprintf(stderr, "no install: %s\n", reader.describe().c_str());
			return 1;
		}
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> opened = bf3::core::Install::open(reader);
		if (!opened) {
			std::fprintf(stderr, "%s\n", opened.error().message.c_str());
			return 1;
		}
		install = *opened;
		for (; arg + 1 < argc; arg += 2) {
			bf3::core::Result<bf3::core::LevelMount> mount = install->mount_superbundle(argv[arg]);
			if (!mount) {
				std::fprintf(stderr, "%s: %s\n", argv[arg], mount.error().message.c_str());
				return 1;
			}
			const bf3::core::Status loaded = mount->load_all_bundles();
			if (!loaded) {
				std::fprintf(stderr, "%s: %s\n", argv[arg], loaded.error().message.c_str());
				return 1;
			}
			Section section;
			section.tag = argv[arg + 1];
			section.mount = std::make_unique<bf3::core::LevelMount>(std::move(*mount));
			section.keys = decodable_res_names(*section.mount);
			sections.push_back(std::move(section));
		}
	} else {
		g_data = argv[arg++];
		for (; arg < argc; arg++) {
			Section section;
			section.tag = argv[arg];
			std::vector<uint8_t> index;
			if (!read_file(g_data + "/" + section.tag + "/index.tsv", index)) {
				std::fprintf(stderr, "no index for %s\n", section.tag.c_str());
				return 1;
			}
			size_t line_start = 0;
			for (size_t i = 0; i <= index.size(); i++) {
				if (i < index.size() && index[i] != '\n') {
					continue;
				}
				std::string line(reinterpret_cast<const char *>(index.data()) + line_start, i - line_start);
				line_start = i + 1;
				if (!line.empty()) {
					section.keys.push_back(g_data + "/" + section.tag + "/res/" + line.substr(0, line.find('\t')));
				}
			}
			sections.push_back(std::move(section));
		}
	}
	using Clock = std::chrono::steady_clock;
	std::map<std::string, std::pair<size_t, double>> timing, decode_timing;
	size_t unreadable = 0;
	for (const Section &section : sections) {
		std::fprintf(stdout, "## %s\n", section.tag.c_str());
		for (const std::string &key : section.keys) {
			Res r;
			if (!load_item(section, key, r)) {
				if (!install_mode) {
					std::fprintf(stderr, "bad res file %s\n", key.c_str());
					return 1;
				}
				unreadable++;
				continue;
			}
			if (trunc || skin) {
				std::string o;
				if (trunc) {
					trunc_report(o, r);
				} else {
					skin_report(o, r);
				}
				std::fwrite(o.data(), 1, o.size(), stdout);
				continue;
			}
			const auto t0 = Clock::now();
			std::string o = fmt("== %s %s len=%zu\n", r.name.c_str(), type_label(r.res_type), r.payload.size());
			if (r.res_type == kResTypeMeshSet) {
				mesh_summary(o, r);
			} else if (r.res_type == kResTypeDxTexture) {
				texture_summary(o, r);
			} else {
				havok_summary(o, r);
			}
			auto &t = timing[type_label(r.res_type)];
			t.first++;
			t.second += std::chrono::duration<double>(Clock::now() - t0).count();
			std::fwrite(o.data(), 1, o.size(), stdout);
		}
	}
	for (const auto &[label, t] : timing) {
		std::fprintf(stderr, "%s: %zu res, %.3f s (decode + hash)\n", label.c_str(), t.first, t.second);
	}
	if (unreadable > 0) {
		std::fprintf(stderr, "%zu res could not be read\n", unreadable);
	}
	if (time_pass) {
		for (const Section &section : sections) {
			for (const std::string &key : section.keys) {
				Res r;
				if (!load_item(section, key, r)) {
					continue;
				}
				auto &t = decode_timing[type_label(r.res_type)];
				t.first++;
				t.second += decode_only(r);
			}
		}
		for (const auto &[label, t] : decode_timing) {
			std::fprintf(stderr, "%s: %zu res, %.3f s (decode only)\n", label.c_str(), t.first, t.second);
		}
	}
	return 0;
}

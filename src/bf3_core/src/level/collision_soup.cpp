// The collision soup (collision_soup.h): placements -> Havok leaves -> world-space triangles with
// material, surface, body and flags -> 64 m tiles. The leaf walk and the per-leaf material and
// part rules are ported from IW4L level_bf3 collision/havok.rs (`leaves`, `decode`,
// `subpart_mesh`) and the owner, body and health-state rules from collision/mod.rs (`group`,
// `objects`, `intact`, `class`), Apache-2.0. Float operations keep the Rust order, so the triangles
// are bit-identical to resource_bf3::triangles on the same leaves.
#include "bf3/core/level/collision_soup.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace bf3::core::level {

namespace {

using res::Shape;
using res::ShapeType;
using res::Transform;
using res::TrianglesSubpart;

constexpr uint32_t kMaxLeafDepth = 64; // havok.rs MAX_DEPTH
constexpr double kMaxCoordinate = 1e7; // metres
constexpr int64_t kTileMm3 = int64_t(64) * 1000 * 3; // a 64 m tile in summed millimetre centroids

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

void set_stage(SoupProgress *p_progress, int p_stage, size_t p_total) {
	if (p_progress != nullptr) {
		p_progress->done = 0;
		p_progress->total = p_total;
		p_progress->stage = p_stage;
	}
}

void tick(SoupProgress *p_progress) {
	if (p_progress != nullptr) {
		p_progress->done.fetch_add(1, std::memory_order_relaxed);
	}
}

struct LeafRef {
	const Shape *shape = nullptr; // a primitive or compressed mesh, or null for a triangle subpart
	const TrianglesSubpart *triangles = nullptr;
	Transform frame;
	std::optional<uint32_t> subpart;
	uint32_t first_part = 0;
};

// havok.rs `leaves`.
void leaves(const Shape &p_shape, const Transform &p_frame, std::optional<std::pair<uint32_t, uint32_t>> p_subpart, uint32_t p_depth,
		std::vector<LeafRef> &r_out) {
	if (p_depth > kMaxLeafDepth) {
		return;
	}
	const auto leaf = [&](const Shape *p_leaf, const TrianglesSubpart *p_triangles) {
		LeafRef r;
		r.shape = p_leaf;
		r.triangles = p_triangles;
		r.frame = p_frame;
		if (p_subpart) {
			r.subpart = p_subpart->first;
			r.first_part = p_subpart->second;
		}
		r_out.push_back(r);
	};
	switch (p_shape.type) {
		case ShapeType::MoppBvTree:
		case ShapeType::List:
			for (const Shape &child : p_shape.children) {
				leaves(child, p_frame, p_subpart, p_depth + 1, r_out);
			}
			break;
		case ShapeType::ConvexTranslate:
			for (const Shape &child : p_shape.children) {
				leaves(child, p_frame.then_inner(Transform::from_translation(p_shape.translation)), p_subpart, p_depth + 1, r_out);
			}
			break;
		case ShapeType::ConvexTransform:
			for (const Shape &child : p_shape.children) {
				leaves(child, p_frame.then_inner(p_shape.transform), p_subpart, p_depth + 1, r_out);
			}
			break;
		case ShapeType::ExtendedMesh:
			if (p_shape.extended) {
				const std::vector<res::ShapesSubpart> &parts = p_shape.extended->shape_subparts;
				for (size_t index = 0; index < parts.size(); ++index) {
					const Transform placed = p_frame.then_inner(parts[index].transform());
					for (const Shape &child : parts[index].children) {
						leaves(child, placed, std::make_pair(uint32_t(index), parts[index].user_data), p_depth + 1, r_out);
					}
				}
				for (const TrianglesSubpart &part : p_shape.extended->triangle_subparts) {
					leaf(nullptr, &part);
				}
			}
			break;
		case ShapeType::Unsupported:
			break;
		default:
			leaf(&p_shape, nullptr);
			break;
	}
}

struct RawTri {
	std::array<res::Vec3, 3> v{};
	uint32_t material = 0;
	uint32_t part = 0;
};

// havok.rs `decode` without the welding: the raw triangles of one leaf with their material value
// and physics part.
void leaf_triangles(const LeafRef &p_leaf, const res::HavokPhysicsData &p_havok, std::vector<RawTri> &r_out) {
	r_out.clear();
	const std::vector<uint32_t> &flags = p_havok.material_flags_and_indices;
	const auto material = [&](uint32_t p_index) { return p_index < flags.size() ? flags[p_index] : 0u; };
	if (p_leaf.triangles != nullptr) {
		const TrianglesSubpart &subpart = *p_leaf.triangles;
		const Transform placed = p_leaf.frame.then_inner(subpart.transform);
		for (size_t index = 0; index < subpart.triangles.size(); ++index) {
			const std::array<uint32_t, 3> &t = subpart.triangles[index];
			if (t[0] < subpart.vertices.size() && t[1] < subpart.vertices.size() && t[2] < subpart.vertices.size()) {
				RawTri raw;
				for (int k = 0; k < 3; ++k) {
					raw.v[k] = placed.transform_point(subpart.vertices[t[k]]);
				}
				raw.material = material(index < subpart.materials.size() ? subpart.materials[index] : 0u);
				r_out.push_back(raw);
			}
		}
		return;
	}
	const Shape &shape = *p_leaf.shape;
	const std::vector<res::Tri> tris = res::havok_triangles(shape, p_leaf.frame);
	if (shape.type == ShapeType::CompressedMesh) {
		for (const res::Tri &t : tris) {
			r_out.push_back(RawTri{ t.v, material(t.material), t.part });
		}
		return;
	}
	switch (shape.type) {
		case ShapeType::Box:
		case ShapeType::Sphere:
		case ShapeType::Capsule:
		case ShapeType::Cylinder:
		case ShapeType::ConvexVertices:
			break;
		default:
			return;
	}
	const uint32_t user_data = shape.user_data;
	const uint32_t part = user_data & 0xFF;
	const uint32_t local = (user_data >> 8) & 0xFF;
	uint32_t index = local;
	if (local >= flags.size()) {
		const uint64_t slot = uint64_t(p_leaf.first_part) + part; // Rust adds in u32; retail never wraps
		index = slot < p_havok.material_indices.size() ? uint32_t(p_havok.material_indices[size_t(slot)]) : 0u;
	}
	const uint32_t value = material(index);
	for (const res::Tri &t : tris) {
		r_out.push_back(RawTri{ t.v, value, part });
	}
}

// Who a leaf belongs to: its health states (null = every part intact) and the mesh name the
// surface fallback and the invisible-blocker flag read.
struct Owner {
	const std::vector<bool> *intact = nullptr;
	std::string_view name;
	uint8_t flags = 0; // kSoupInvisible
};

bool intact(const Owner &p_owner, uint32_t p_part) {
	return p_owner.intact == nullptr || p_part >= p_owner.intact->size() || (*p_owner.intact)[p_part];
}

bool invisible_name(std::string_view p_name) {
	std::string lower(p_name);
	for (char &c : lower) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
	return lower.find("invisible") != std::string::npos || lower.find("collisionmesh") != std::string::npos;
}

uint8_t surface_of(const SurfaceMaterials &p_materials, uint32_t p_value, std::string_view p_name) {
	uint8_t s = 0;
	if (p_materials.lookup(p_value, s) == SurfaceMaterials::Lookup::Surface) {
		return s;
	}
	const std::optional<uint8_t> named = classify_surface(p_name);
	if (named && *named == surface::kFoliage && (p_value & kMaterialSeeThrough) == 0) {
		return surface::kBark;
	}
	return named.value_or(surface::kConcrete);
}

uint8_t body_kind(std::string_view p_name) {
	if (p_name == "RBTypeRaycast") {
		return uint8_t(CollisionBody::Raycast);
	}
	if (p_name == "RBTypeCharacter") {
		return uint8_t(CollisionBody::Character);
	}
	return uint8_t(CollisionBody::Collision);
}

// Everything one placement's parallel build needs, gathered on the calling thread.
struct Job {
	const res::HavokPhysicsData *havok = nullptr;
	const std::vector<Shape> *shapes = nullptr;
	Transform frame;
	std::vector<uint8_t> bodies; // per root
	Owner owner; // objects: every leaf
	const std::unordered_map<uint32_t, Owner> *members = nullptr; // groups: owner by subpart
};

// One placement's leaves cut into pieces of at most kLeavesPerPiece, so a single huge placement
// (XP3_Alborz's static model group: 2.3 M triangles) spreads over every core.
constexpr size_t kLeavesPerPiece = 64;

struct LeafItem {
	uint8_t body = 0;
	LeafRef leaf;
};

struct Piece {
	uint32_t placement = 0;
	size_t first = 0; // into the placement's items
	size_t count = 0;
	std::vector<SoupTriangle> triangles;
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
};

void collect_items(const Job &p_job, std::vector<LeafItem> &r_items) {
	std::vector<LeafRef> refs;
	for (size_t root = 0; root < p_job.shapes->size(); ++root) {
		const uint8_t body = root < p_job.bodies.size() ? p_job.bodies[root] : uint8_t(CollisionBody::Collision);
		refs.clear();
		leaves((*p_job.shapes)[root], p_job.frame, std::nullopt, 0, refs);
		for (const LeafRef &leaf : refs) {
			r_items.push_back(LeafItem{ body, leaf });
		}
	}
}

void build_piece(const Job &p_job, const std::vector<LeafItem> &p_items, const SurfaceMaterials &p_materials, Piece &r_piece) {
	static const Owner kNobody;
	std::vector<RawTri> raws;
	for (size_t i = r_piece.first; i < r_piece.first + r_piece.count; ++i) {
		const LeafItem &item = p_items[i];
		const Owner *owner = &p_job.owner;
		if (p_job.members != nullptr) {
			owner = &kNobody;
			if (item.leaf.subpart) {
				const auto it = p_job.members->find(*item.leaf.subpart);
				if (it != p_job.members->end()) {
					owner = &it->second;
				}
			}
		}
		leaf_triangles(item.leaf, *p_job.havok, raws);
		for (const RawTri &raw : raws) {
			if (!intact(*owner, raw.part)) {
				++r_piece.destroyed;
				continue;
			}
			std::array<int64_t, 9> q{};
			if (!soup_quantise(raw.v, q)) {
				++r_piece.nonfinite;
				continue;
			}
			if (soup_degenerate(q)) {
				++r_piece.degenerate;
				continue;
			}
			SoupTriangle t;
			t.v = raw.v;
			t.material = raw.material;
			t.placement = r_piece.placement;
			t.surface = surface_of(p_materials, raw.material, owner->name);
			t.body = item.body;
			t.flags = uint8_t(((raw.material & kMaterialSeeThrough) != 0 ? kSoupSeeThrough : 0) | owner->flags);
			r_piece.triangles.push_back(t);
		}
	}
}

struct Built {
	std::vector<SoupTriangle> triangles;
	std::vector<std::array<int32_t, 2>> tiles; // per triangle
	std::vector<uint64_t> hashes; // per triangle
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = kFnvOffset;
};

// The placement's pieces in order: its triangles, their tiles and hashes, its stream hash.
void finish_placement(std::vector<Piece *> &p_pieces, Built &r_built) {
	size_t count = 0;
	for (const Piece *piece : p_pieces) {
		count += piece->triangles.size();
	}
	r_built.triangles.reserve(count);
	r_built.tiles.reserve(count);
	r_built.hashes.reserve(count);
	for (Piece *piece : p_pieces) {
		r_built.destroyed += piece->destroyed;
		r_built.degenerate += piece->degenerate;
		r_built.nonfinite += piece->nonfinite;
		for (const SoupTriangle &t : piece->triangles) {
			std::array<int64_t, 9> q{};
			soup_quantise(t.v, q); // kept triangles quantise (checked when built)
			uint64_t h = kFnvOffset;
			soup_hash_bytes(h, q, t.material, t.surface, t.body, t.flags);
			soup_hash_bytes(r_built.hash, q, t.material, t.surface, t.body, t.flags);
			r_built.triangles.push_back(t);
			r_built.tiles.push_back(soup_tile_of(q));
			r_built.hashes.push_back(h);
		}
		piece->triangles = std::vector<SoupTriangle>();
	}
}

Status owner_states(const EbxInstance &p_entity, WorkBudget &r_budget, std::map<std::pair<const EbxPartition *, uint32_t>, std::vector<bool>> &r_cache,
		const std::vector<bool> *&r_states) {
	const auto key = std::make_pair(&p_entity.partition(), p_entity.index());
	auto it = r_cache.find(key);
	if (it == r_cache.end()) {
		std::vector<bool> states;
		BF3_TRY(r_budget.charge(array_length(p_entity.data(), "PhysicsPartInfos")));
		if (const std::optional<EbxValue> infos = p_entity.get("PhysicsPartInfos")) {
			const OptionalEbxArray array = infos->as_array();
			if (array) {
				for (const EbxValue info : *array) {
					const std::optional<EbxValue> state = info.get("HealthStateIndex");
					const std::optional<int64_t> index = state ? state->as_i64() : std::nullopt;
					states.push_back(!index || *index == 0);
				}
			}
		}
		it = r_cache.emplace(key, std::move(states)).first;
	}
	r_states = &it->second;
	return Status::success();
}

// --- Terrain (collision_soup.h SoupTerrain; the oracle's `terrain`).

constexpr size_t kMaxTerrainSide = res::kMaxGridSide; // IW4L collision/terrain.rs MAX_SIDE

// The heightfield grid of a terrain asset at the depth whose spacing first reaches p_spacing.
bool terrain_grid(const LevelMount &p_mount, const std::string &p_asset, float p_spacing, unsigned p_threads, uint8_t &r_depth, res::HeightGrid &r_grid,
		std::string &r_error, std::vector<SoupSource> &r_sources) {
	r_sources.push_back({ SoupSource::Kind::Res, asset_key(p_asset), {} });
	const Result<std::pair<ResInfo, std::vector<uint8_t>>> terrain = p_mount.read_res(p_asset);
	if (!terrain) {
		r_error = "res " + p_asset + ": " + terrain.error().message;
		return false;
	}
	std::string tree_name;
	res::ResError e = res::terrain_streaming_tree_name(terrain->second, tree_name);
	if (!e.ok()) {
		r_error = "res " + p_asset + ": " + e.message();
		return false;
	}
	r_sources.push_back({ SoupSource::Kind::Res, asset_key(tree_name), {} });
	const Result<std::pair<ResInfo, std::vector<uint8_t>>> tree_bytes = p_mount.read_res(tree_name);
	if (!tree_bytes) {
		r_error = "res " + tree_name + ": " + tree_bytes.error().message;
		return false;
	}
	res::TerrainStreamingTree tree;
	e = res::TerrainStreamingTree::parse(tree_bytes->second, tree);
	if (!e.ok()) {
		r_error = "res " + tree_name + ": " + e.message();
		return false;
	}
	if (!tree.heightfield) {
		r_error = "res " + tree_name + ": streaming tree without a heightfield";
		return false;
	}
	const res::HeightfieldTree &heightfield = *tree.heightfield;
	if (heightfield.nodes.empty()) {
		r_error = "res " + tree_name + ": empty heightfield tree";
		return false;
	}
	const res::HeightfieldNode &root = heightfield.nodes.front();
	const size_t cells = heightfield.interior_cells();
	const float span = std::fmax(root.bounds_max[0] - root.bounds_min[0], root.bounds_max[2] - root.bounds_min[2]);
	const uint8_t max_level = heightfield.max_level();
	uint8_t depth = 0;
	while (depth < max_level && span / float(cells << depth) > p_spacing * 1.001f && (cells << (depth + 1)) < kMaxTerrainSide) {
		++depth;
	}
	// The lod0 chunks height_grid may ask for (streaming nodes down to the depth), read on all cores.
	std::vector<res::ChunkId> wanted;
	for (const res::TerrainStreamingNode &node : tree.streaming_nodes) {
		if (node.id.level <= depth) {
			wanted.push_back(node.lod0_chunk);
			r_sources.push_back({ SoupSource::Kind::Chunk, {}, Guid::from_bytes(node.lod0_chunk.bytes.data()) });
		}
	}
	std::vector<std::optional<std::vector<uint8_t>>> chunks(wanted.size());
	parallel_for(
			wanted.size(),
			[&](size_t p_index) {
				Result<std::vector<uint8_t>> chunk = p_mount.read_chunk(Guid::from_bytes(wanted[p_index].bytes.data()));
				if (chunk) {
					chunks[p_index] = std::move(*chunk);
				}
			},
			p_threads);
	const auto fetch = [&](const res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
		for (size_t i = 0; i < wanted.size(); ++i) {
			if (wanted[i] == p_id) {
				return chunks[i];
			}
		}
		r_sources.push_back({ SoupSource::Kind::Chunk, {}, Guid::from_bytes(p_id.bytes.data()) });
		Result<std::vector<uint8_t>> chunk = p_mount.read_chunk(Guid::from_bytes(p_id.bytes.data()));
		if (!chunk) {
			return std::nullopt;
		}
		return std::move(*chunk);
	};
	e = tree.height_grid(depth, fetch, r_grid);
	if (!e.ok()) {
		r_error = "res " + tree_name + ": " + e.message();
		return false;
	}
	r_depth = depth;
	return true;
}

// Tile index of a cell centre; false when it is not a sane coordinate.
bool centre_tile(float p_origin, float p_spacing, size_t p_index, int32_t &r_tile) {
	const double centre = double(p_origin) + (double(p_index) + 0.5) * double(p_spacing);
	const double tile = std::floor(centre / 64.0);
	if (!(std::fabs(tile) < 1e7)) {
		return false;
	}
	r_tile = int32_t(tile);
	return true;
}

// Winds a terrain triangle to face +Y (Havok's counter-clockwise front).
std::array<res::Vec3, 3> face_up(const std::array<res::Vec3, 3> &p_t) {
	const res::Vec3 &a = p_t[0];
	const res::Vec3 &b = p_t[1];
	const res::Vec3 &c = p_t[2];
	const res::Vec3 u = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const res::Vec3 v = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	if (u[2] * v[0] - u[0] * v[2] < 0.0f) {
		return { a, c, b };
	}
	return p_t;
}

// Kept tiles: the Havok tiles and their 8-neighbours, as a dense mask over their bounding range.
struct TileMask {
	int32_t x0 = 0;
	int32_t z0 = 0;
	int32_t width = 0;
	int32_t height = 0;
	std::vector<uint8_t> bits;

	bool has(int32_t p_x, int32_t p_z) const {
		const int64_t x = int64_t(p_x) - x0;
		const int64_t z = int64_t(p_z) - z0;
		return x >= 0 && z >= 0 && x < width && z < height && bits[size_t(x + z * width)] != 0;
	}
};

TileMask near_tiles(const std::map<std::pair<int32_t, int32_t>, size_t> &p_tiles) {
	TileMask mask;
	if (p_tiles.empty()) {
		return mask;
	}
	int64_t x0 = INT64_MAX;
	int64_t z0 = INT64_MAX;
	int64_t x1 = INT64_MIN;
	int64_t z1 = INT64_MIN;
	for (const auto &[key, count] : p_tiles) {
		x0 = std::min<int64_t>(x0, int64_t(key.first) - 1);
		x1 = std::max<int64_t>(x1, int64_t(key.first) + 1);
		z0 = std::min<int64_t>(z0, int64_t(key.second) - 1);
		z1 = std::max<int64_t>(z1, int64_t(key.second) + 1);
	}
	// Tiles come from |c| <= 1e7 m coordinates (~156k tiles from the origin); the mask is only
	// allocated when it is small, a scattered soup looks the tiles up in the map instead.
	if ((x1 - x0 + 1) * (z1 - z0 + 1) > (int64_t(1) << 24)) {
		return mask;
	}
	mask.x0 = int32_t(x0);
	mask.z0 = int32_t(z0);
	mask.width = int32_t(x1 - x0 + 1);
	mask.height = int32_t(z1 - z0 + 1);
	mask.bits.assign(size_t(mask.width) * size_t(mask.height), 0);
	for (const auto &[key, count] : p_tiles) {
		for (int32_t dz = -1; dz <= 1; ++dz) {
			for (int32_t dx = -1; dx <= 1; ++dx) {
				mask.bits[size_t((key.first + dx - mask.x0) + (key.second + dz - mask.z0) * mask.width)] = 1;
			}
		}
	}
	return mask;
}

struct TerrainRow {
	std::vector<SoupTriangle> triangles;
	size_t cells = 0;
	size_t open = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
};

// One terrain object's rows of cells (all cores), then its triangles in emit order into r_built.
void terrain_triangles(const res::HeightGrid &p_grid, const Transform &p_frame, const std::map<std::pair<int32_t, int32_t>, size_t> &p_tiles,
		const std::vector<std::array<int32_t, 2>> &p_cover, uint32_t p_placement, unsigned p_threads, SoupTerrain &r_terrain, Built &r_built) {
	const size_t width = p_grid.width;
	const size_t height = p_grid.height;
	if (width < 2 || height < 2 || (p_tiles.empty() && p_cover.empty())) {
		return;
	}
	const TileMask mask = near_tiles(p_tiles);
	// S1 terrain cover (SoupOptions::terrain_cover, sorted and unique): those tiles keep their cells too. Empty for
	// every M1 soup, so the default rule below is untouched.
	const auto covered = [&](int32_t p_x, int32_t p_z) {
		return !p_cover.empty() && std::binary_search(p_cover.begin(), p_cover.end(), std::array<int32_t, 2>{ p_x, p_z });
	};
	const auto near = [&](int32_t p_x, int32_t p_z) {
		if (covered(p_x, p_z)) {
			return true;
		}
		if (p_tiles.empty()) {
			return false;
		}
		if (!mask.bits.empty()) {
			return mask.has(p_x, p_z);
		}
		for (int32_t dz = -1; dz <= 1; ++dz) {
			for (int32_t dx = -1; dx <= 1; ++dx) {
				if (p_tiles.count({ p_x - dx, p_z - dz }) != 0) {
					return true;
				}
			}
		}
		return false;
	};
	const float ox = p_grid.origin_xz[0];
	const float oz = p_grid.origin_xz[1];
	const float sx = p_grid.sample_spacing[0];
	const float sz = p_grid.sample_spacing[1];
	std::vector<int32_t> column_tile(width - 1);
	std::vector<uint8_t> column_ok(width - 1);
	for (size_t x = 0; x + 1 < width; ++x) {
		column_ok[x] = centre_tile(ox, sx, x, column_tile[x]) ? 1 : 0;
	}
	std::vector<TerrainRow> rows(height - 1);
	parallel_for(
			height - 1,
			[&](size_t p_z) {
				int32_t tz = 0;
				if (!centre_tile(oz, sz, p_z, tz)) {
					return;
				}
				TerrainRow &row = rows[p_z];
				const size_t z = p_z;
				for (size_t x = 0; x + 1 < width; ++x) {
					if (column_ok[x] == 0 || !near(column_tile[x], tz)) {
						continue;
					}
					++row.cells;
					if (p_grid.hole(x, z) || p_grid.hole(x + 1, z) || p_grid.hole(x, z + 1) || p_grid.hole(x + 1, z + 1)) {
						++row.open;
						continue;
					}
					const auto point = [&](size_t p_i, size_t p_j) {
						return p_frame.transform_point({ ox + float(x + p_i) * sx, p_grid.at(x + p_i, z + p_j), oz + float(z + p_j) * sz });
					};
					const res::Vec3 a = point(0, 0);
					const res::Vec3 b = point(1, 0);
					const res::Vec3 c = point(0, 1);
					const res::Vec3 d = point(1, 1);
					const std::array<std::array<res::Vec3, 3>, 2> pair = { { { a, d, b }, { a, c, d } } };
					for (const std::array<res::Vec3, 3> &t : pair) {
						SoupTriangle tri;
						tri.v = face_up(t);
						std::array<int64_t, 9> q{};
						if (!soup_quantise(tri.v, q)) {
							++row.nonfinite;
							continue;
						}
						if (soup_degenerate(q)) {
							++row.degenerate;
							continue;
						}
						tri.material = 0;
						tri.placement = p_placement;
						tri.surface = surface::kDirt;
						tri.body = uint8_t(CollisionBody::Collision);
						tri.flags = kSoupTerrain;
						row.triangles.push_back(tri);
					}
				}
			},
			p_threads);
	size_t count = 0;
	for (const TerrainRow &row : rows) {
		count += row.triangles.size();
	}
	r_built.triangles.reserve(count);
	r_built.tiles.reserve(count);
	r_built.hashes.reserve(count);
	for (TerrainRow &row : rows) {
		r_terrain.cells += row.cells;
		r_terrain.open += row.open;
		r_built.degenerate += row.degenerate;
		r_built.nonfinite += row.nonfinite;
		for (const SoupTriangle &t : row.triangles) {
			std::array<int64_t, 9> q{};
			soup_quantise(t.v, q);
			uint64_t h = kFnvOffset;
			soup_hash_bytes(h, q, t.material, t.surface, t.body, t.flags);
			soup_hash_bytes(r_built.hash, q, t.material, t.surface, t.body, t.flags);
			r_built.triangles.push_back(t);
			r_built.tiles.push_back(soup_tile_of(q));
			r_built.hashes.push_back(h);
		}
		row = TerrainRow();
	}
}

void include_point(CollisionSoup &r_soup, bool &r_any, const res::Vec3 &p_p) {
	if (!r_any) {
		r_soup.min = p_p;
		r_soup.max = p_p;
		r_any = true;
		return;
	}
	for (int k = 0; k < 3; ++k) {
		r_soup.min[k] = std::min(r_soup.min[k], p_p[k]);
		r_soup.max[k] = std::max(r_soup.max[k], p_p[k]);
	}
}

} // namespace

std::string_view soup_stage_name(int p_stage) {
	switch (p_stage) {
		case SoupProgress::Mount:
			return "mounting the level";
		case SoupProgress::Traverse:
			return "traversing";
		case SoupProgress::Placements:
			return "collision placements";
		case SoupProgress::Materials:
			return "material grid";
		case SoupProgress::Decode:
			return "decoding Havok res";
		case SoupProgress::Triangles:
			return "building triangles";
		case SoupProgress::Terrain:
			return "terrain";
		case SoupProgress::Tiles:
			return "tiling";
		case SoupProgress::Done:
			return "done";
		default:
			return "?";
	}
}

bool soup_quantise(const std::array<res::Vec3, 3> &p_v, std::array<int64_t, 9> &r_q) {
	for (size_t k = 0; k < 9; ++k) {
		const double c = double(p_v[k / 3][k % 3]);
		if (!std::isfinite(c) || std::fabs(c) > kMaxCoordinate) {
			return false;
		}
		r_q[k] = int64_t(std::round(c * 1000.0));
	}
	return true;
}

bool soup_degenerate(const std::array<int64_t, 9> &p_q) {
	const auto same = [&](int p_a, int p_b) { return p_q[p_a * 3] == p_q[p_b * 3] && p_q[p_a * 3 + 1] == p_q[p_b * 3 + 1] && p_q[p_a * 3 + 2] == p_q[p_b * 3 + 2]; };
	return same(0, 1) || same(1, 2) || same(0, 2);
}

void soup_hash_bytes(uint64_t &r_hash, const std::array<int64_t, 9> &p_q, uint32_t p_material, uint8_t p_surface, uint8_t p_body, uint8_t p_flags) {
	const auto mix = [&](uint8_t p_byte) {
		r_hash ^= p_byte;
		r_hash *= 0x00000100000001B3ull;
	};
	for (int64_t v : p_q) {
		const uint64_t u = uint64_t(v);
		for (int b = 0; b < 8; ++b) {
			mix(uint8_t(u >> (8 * b)));
		}
	}
	for (int b = 0; b < 4; ++b) {
		mix(uint8_t(p_material >> (8 * b)));
	}
	mix(p_surface);
	mix(p_body);
	mix(p_flags);
}

std::array<int32_t, 2> soup_tile_of(const std::array<int64_t, 9> &p_q) {
	const auto floor_div = [](int64_t p_a, int64_t p_b) {
		int64_t d = p_a / p_b;
		if ((p_a % p_b) != 0 && p_a < 0) {
			--d;
		}
		return int32_t(d);
	};
	return { floor_div(p_q[0] + p_q[3] + p_q[6], kTileMm3), floor_div(p_q[2] + p_q[5] + p_q[8], kTileMm3) };
}

SoupShapeResult soup_shape_triangles(const res::HavokPhysicsData &p_havok, const std::vector<res::Shape> &p_roots, const res::Transform &p_frame,
		const std::vector<uint8_t> &p_bodies, const SurfaceMaterials &p_materials, std::string_view p_owner_name) {
	Job job;
	job.havok = &p_havok;
	job.shapes = &p_roots;
	job.frame = p_frame;
	job.bodies = p_bodies;
	job.owner.name = p_owner_name;
	job.owner.flags = invisible_name(p_owner_name) ? kSoupInvisible : 0;
	std::vector<LeafItem> items;
	collect_items(job, items);
	Piece piece;
	piece.count = items.size();
	build_piece(job, items, p_materials, piece);
	std::vector<Piece *> pieces{ &piece };
	Built built;
	finish_placement(pieces, built);
	SoupShapeResult out;
	out.triangles = std::move(built.triangles);
	out.tiles = std::move(built.tiles);
	out.destroyed = built.destroyed;
	out.degenerate = built.degenerate;
	out.nonfinite = built.nonfinite;
	out.hash = built.hash;
	for (uint64_t h : built.hashes) {
		out.sum += h;
	}
	return out;
}

Result<CollisionSoup> build_collision_soup(const LoadedLevel &p_level, const World &p_world, const SoupOptions &p_options) {
	const EbxResolver &db = p_level.db();
	const LevelMount &mount = p_level.mount();
	WorkBudget &budget = *p_world.budget;
	SoupProgress *progress = p_options.progress;
	CollisionSoup soup;
	auto clock = std::chrono::steady_clock::now();

	// --- Calling thread: placements, materials, owners, bodies (the World is not thread-safe).
	set_stage(progress, SoupProgress::Placements, 0);
	Result<std::vector<CollisionPlacement>> placements = collision_placements(p_level, p_world);
	if (!placements) {
		return placements.error();
	}
	set_stage(progress, SoupProgress::Materials, 0);
	Result<SurfaceMaterials> materials = SurfaceMaterials::read(db, p_level.root(), budget);
	if (!materials) {
		return materials.error();
	}
	soup.material_pairs = materials->pair_count();
	soup.resolved_pairs = materials->resolved_pairs();

	std::map<std::pair<const EbxPartition *, uint32_t>, std::vector<bool>> states;
	const auto owner_of = [&](const PlacedObject &p_object, Owner &r_owner) -> Status {
		BF3_TRY(owner_states(p_object.entity, budget, states, r_owner.intact));
		if (p_object.mesh) {
			const Result<std::string_view> name = p_object.mesh->name(budget);
			if (!name) {
				return name.error();
			}
			r_owner.name = *name;
			r_owner.flags = invisible_name(*name) ? kSoupInvisible : 0;
		}
		return Status::success();
	};

	// Group members by Havok subpart; a later member wins (IW4L's HashMap insert).
	std::map<uint32_t, std::unordered_map<uint32_t, Owner>> members;
	for (const CollisionPlacement &c : *placements) {
		if (c.kind == CollisionPlacement::Kind::Group) {
			members[c.index];
		}
	}
	for (size_t at = 0; at < p_world.objects.size(); ++at) {
		const PlacedObject &object = p_world.objects[at];
		if (!object.group || !object.group->havok_subpart) {
			continue;
		}
		const auto it = members.find(object.group->group);
		if (it == members.end()) {
			continue;
		}
		Owner owner;
		BF3_TRY(owner_of(object, owner));
		it->second[*object.group->havok_subpart] = owner;
	}

	// M1 T1: the sources the build reads (CollisionSoup::sources), sorted and made unique at the end.
	const auto add_partition_source = [&](const EbxInstance &p_instance) {
		if (const std::optional<std::string_view> name = db.name_of(p_instance.partition().guid())) {
			soup.sources.push_back({ SoupSource::Kind::Ebx, std::string(*name), {} });
		}
	};
	add_partition_source(p_level.root());
	for (const CollisionPlacement &c : *placements) {
		if (c.kind == CollisionPlacement::Kind::Group) {
			add_partition_source(p_world.groups[c.index].entity);
		} else {
			const PlacedObject &object = p_world.objects[c.index];
			add_partition_source(object.entity);
			if (object.reference) {
				add_partition_source(*object.reference);
			}
		}
		if (!c.res.empty()) {
			soup.sources.push_back({ SoupSource::Kind::Res, asset_key(c.res), {} });
		}
	}

	soup.placements.resize(placements->size());
	std::vector<Job> jobs(placements->size());
	std::vector<std::string> object_res; // distinct res names of built objects
	std::unordered_map<std::string, size_t> res_slot;
	std::vector<size_t> job_res(placements->size(), SIZE_MAX);
	std::vector<std::shared_ptr<const res::HavokPhysicsData>> group_havok(placements->size());
	for (size_t i = 0; i < placements->size(); ++i) {
		const CollisionPlacement &c = (*placements)[i];
		SoupPlacement &out = soup.placements[i];
		out.kind = c.kind;
		out.index = c.index;
		out.res = c.res;
		Job &job = jobs[i];
		job.frame = c.frame;
		if (c.kind == CollisionPlacement::Kind::Group) {
			const StaticModelGroup &group = p_world.groups[c.index];
			if (!group.havok) {
				out.state = SoupPlacement::State::Missing;
				out.error = "the traversal kept no Havok data for the group";
				continue;
			}
			group_havok[i] = group.havok;
			job.havok = group.havok.get();
			job.members = &members[c.index];
			continue;
		}
		const PlacedObject &object = p_world.objects[c.index];
		if (c.res.empty()) {
			out.state = SoupPlacement::State::NoRes;
			continue;
		}
		const Result<bool> hidden = starts_hidden(p_world, object);
		if (!hidden) {
			return hidden.error();
		}
		if (*hidden) {
			out.state = SoupPlacement::State::Hidden;
			continue;
		}
		BF3_TRY(owner_of(object, job.owner));
		for (std::string_view body : c.bodies) {
			job.bodies.push_back(body_kind(body));
		}
		BF3_TRY(charge_text(budget, c.res)); // keyed below
		const auto [it, fresh] = res_slot.emplace(c.res, object_res.size());
		if (fresh) {
			object_res.push_back(c.res);
		}
		job_res[i] = it->second;
	}
	soup.seconds_placements = seconds_since(clock);

	// --- All cores: object res (once per name) and group shape trees.
	clock = std::chrono::steady_clock::now();
	std::vector<size_t> group_jobs;
	for (size_t i = 0; i < jobs.size(); ++i) {
		if (group_havok[i]) {
			group_jobs.push_back(i);
		}
	}
	set_stage(progress, SoupProgress::Decode, object_res.size() + group_jobs.size());
	std::vector<std::optional<Result<res::LoadedHavok>>> loaded(object_res.size());
	std::vector<std::vector<Shape>> group_shapes(group_jobs.size());
	std::vector<res::ResError> group_errors(group_jobs.size());
	parallel_for(
			object_res.size() + group_jobs.size(),
			[&](size_t p_index) {
				if (p_index < group_jobs.size()) {
					// Groups first: they are the largest trees (XP3_Alborz's is 107 MB decoded).
					group_errors[p_index] = group_havok[group_jobs[p_index]]->shapes(group_shapes[p_index]);
				} else {
					const size_t r = p_index - group_jobs.size();
					loaded[r].emplace(res::load_havok(mount, object_res[r]));
				}
				tick(progress);
			},
			p_options.threads);
	for (size_t g = 0; g < group_jobs.size(); ++g) {
		const size_t i = group_jobs[g];
		if (!group_errors[g].ok()) {
			soup.placements[i].state = SoupPlacement::State::Missing;
			soup.placements[i].error = "res " + soup.placements[i].res + ": " + group_errors[g].message();
			jobs[i].havok = nullptr;
			continue;
		}
		jobs[i].shapes = &group_shapes[g];
		++soup.res_decoded;
	}
	for (const std::optional<Result<res::LoadedHavok>> &r : loaded) {
		soup.res_decoded += (r && *r) ? 1 : 0;
	}
	for (size_t i = 0; i < jobs.size(); ++i) {
		if (job_res[i] == SIZE_MAX) {
			continue;
		}
		const Result<res::LoadedHavok> &r = *loaded[job_res[i]];
		if (!r) {
			soup.placements[i].state = SoupPlacement::State::Missing;
			soup.placements[i].error = r.error().message;
			continue;
		}
		jobs[i].havok = &r->data;
		jobs[i].shapes = &r->shapes;
	}
	soup.seconds_decode = seconds_since(clock);

	// --- All cores: triangles, in pieces of leaves; then per placement, hashes in walk order.
	clock = std::chrono::steady_clock::now();
	std::vector<std::vector<LeafItem>> items(jobs.size());
	std::vector<Piece> pieces;
	for (size_t i = 0; i < jobs.size(); ++i) {
		const Job &job = jobs[i];
		if (job.havok == nullptr || job.shapes == nullptr || soup.placements[i].state != SoupPlacement::State::Built) {
			continue;
		}
		collect_items(job, items[i]);
		for (size_t first = 0; first < items[i].size(); first += kLeavesPerPiece) {
			Piece piece;
			piece.placement = uint32_t(i);
			piece.first = first;
			piece.count = std::min(kLeavesPerPiece, items[i].size() - first);
			pieces.push_back(std::move(piece));
		}
	}
	set_stage(progress, SoupProgress::Triangles, pieces.size());
	std::atomic<size_t> total{ 0 };
	std::atomic<bool> over{ false };
	parallel_for(
			pieces.size(),
			[&](size_t p_index) {
				Piece &piece = pieces[p_index];
				if (!over.load(std::memory_order_relaxed)) {
					build_piece(jobs[piece.placement], items[piece.placement], *materials, piece);
					if (total.fetch_add(piece.triangles.size()) + piece.triangles.size() > kMaxSoupTriangles) {
						over = true;
						piece.triangles = std::vector<SoupTriangle>();
					}
				}
				tick(progress);
			},
			p_options.threads);
	if (over) {
		return make_error("collision soup: more than " + std::to_string(kMaxSoupTriangles) + " triangles");
	}
	items = std::vector<std::vector<LeafItem>>();
	std::vector<std::vector<Piece *>> by_placement(jobs.size());
	for (Piece &piece : pieces) {
		by_placement[piece.placement].push_back(&piece);
	}
	std::vector<Built> built(jobs.size());
	parallel_for(
			jobs.size(), [&](size_t p_index) { finish_placement(by_placement[p_index], built[p_index]); }, p_options.threads);
	pieces = std::vector<Piece>();
	soup.seconds_triangles = seconds_since(clock);

	// --- Totals and tiles (placement order, then walk order; terrain last).
	clock = std::chrono::steady_clock::now();
	std::map<std::pair<int32_t, int32_t>, size_t> tile_counts;
	for (size_t i = 0; i < jobs.size(); ++i) {
		SoupPlacement &p = soup.placements[i];
		const Built &b = built[i];
		switch (p.state) {
			case SoupPlacement::State::Built:
				++soup.built;
				break;
			case SoupPlacement::State::Hidden:
				++soup.hidden;
				break;
			case SoupPlacement::State::Missing:
				++soup.missing;
				break;
			case SoupPlacement::State::NoRes:
				++soup.no_res;
				break;
		}
		if (p.state != SoupPlacement::State::Built) {
			continue;
		}
		p.triangles = b.triangles.size();
		p.destroyed = b.destroyed;
		p.degenerate = b.degenerate;
		p.nonfinite = b.nonfinite;
		p.hash = b.hash;
		soup.triangles += p.triangles;
		soup.destroyed += p.destroyed;
		soup.degenerate += p.degenerate;
		soup.nonfinite += p.nonfinite;
		for (const std::array<int32_t, 2> &tile : b.tiles) {
			++tile_counts[{ tile[0], tile[1] }];
		}
	}

	// --- Terrain: height grids and the cells around the Havok tiles (rows on all cores).
	set_stage(progress, SoupProgress::Terrain, 0);
	if (p_options.terrain) {
		const std::map<std::pair<int32_t, int32_t>, size_t> havok_tiles = tile_counts;
		// S1: the cover, sorted and unique whatever the caller passed (binary search above).
		std::vector<std::array<int32_t, 2>> cover = p_options.terrain_cover;
		std::sort(cover.begin(), cover.end());
		cover.erase(std::unique(cover.begin(), cover.end()), cover.end());
		for (size_t at = 0; at < p_world.objects.size(); ++at) {
			const PlacedObject &object = p_world.objects[at];
			if (object.kind != ObjectKind::Terrain) {
				continue;
			}
			SoupTerrain terrain;
			terrain.object = uint32_t(at);
			const std::optional<EbxInstance> asset = db.follow(object.entity.data(), "TerrainAsset");
			std::optional<std::string_view> name;
			if (asset) {
				const Result<std::optional<std::string_view>> charged = charged_str(asset->data(), "Name", budget);
				if (!charged) {
					return charged.error();
				}
				name = *charged;
			}
			if (!name) {
				terrain.state = SoupTerrain::State::NoRes;
				soup.terrains.push_back(std::move(terrain));
				continue;
			}
			BF3_TRY(charge_text(budget, *name)); // kept below
			terrain.asset = std::string(*name);
			res::HeightGrid grid;
			add_partition_source(object.entity);
			add_partition_source(*asset);
			if (!terrain_grid(mount, terrain.asset, p_options.terrain_spacing, p_options.threads, terrain.depth, grid, terrain.error, soup.sources)) {
				terrain.state = SoupTerrain::State::Missing;
				soup.terrains.push_back(std::move(terrain));
				continue;
			}
			terrain.side = grid.width;
			terrain.holes = size_t(std::count(grid.holes.begin(), grid.holes.end(), uint8_t(1)));
			const LevelTransform &t = object.transform;
			const float factor = t.scale / 1.0f;
			Transform frame;
			for (int k = 0; k < 3; ++k) {
				frame.axis[0][k] = t.right[k] * factor;
				frame.axis[1][k] = t.up[k] * factor;
				frame.axis[2][k] = t.forward[k] * factor;
			}
			frame.origin = t.trans;
			Built b;
			terrain_triangles(grid, frame, havok_tiles, cover, uint32_t(jobs.size() + soup.terrains.size()), p_options.threads, terrain, b);
			if (soup.triangles + soup.terrain_triangles + b.triangles.size() > kMaxSoupTriangles) {
				return make_error("collision soup: more than " + std::to_string(kMaxSoupTriangles) + " triangles");
			}
			terrain.triangles = b.triangles.size();
			terrain.degenerate = b.degenerate;
			terrain.nonfinite = b.nonfinite;
			terrain.hash = b.hash;
			soup.terrain_triangles += terrain.triangles;
			soup.degenerate += b.degenerate;
			soup.nonfinite += b.nonfinite;
			for (const std::array<int32_t, 2> &tile : b.tiles) {
				++tile_counts[{ tile[0], tile[1] }];
			}
			soup.terrains.push_back(std::move(terrain));
			built.push_back(std::move(b));
		}
		soup.triangles += soup.terrain_triangles;
	}
	soup.seconds_terrain = seconds_since(clock);

	clock = std::chrono::steady_clock::now();
	set_stage(progress, SoupProgress::Tiles, built.size());
	std::map<std::pair<int32_t, int32_t>, size_t> tile_index;
	soup.tiles.reserve(tile_counts.size());
	for (const auto &[key, count] : tile_counts) {
		tile_index[key] = soup.tiles.size();
		SoupTile tile;
		tile.x = key.first;
		tile.z = key.second;
		tile.triangles.reserve(count);
		soup.tiles.push_back(std::move(tile));
	}
	bool any = false;
	for (size_t i = 0; i < built.size(); ++i) {
		Built &b = built[i];
		for (size_t t = 0; t < b.triangles.size(); ++t) {
			SoupTile &tile = soup.tiles[tile_index[{ b.tiles[t][0], b.tiles[t][1] }]];
			const SoupTriangle &tri = b.triangles[t];
			for (const res::Vec3 &p : tri.v) {
				if (tile.triangles.empty() && &p == &tri.v[0]) {
					tile.min = p;
					tile.max = p;
				}
				for (int k = 0; k < 3; ++k) {
					tile.min[k] = std::min(tile.min[k], p[k]);
					tile.max[k] = std::max(tile.max[k], p[k]);
				}
				include_point(soup, any, p);
			}
			tile.triangles.push_back(tri);
			tile.sum += b.hashes[t];
			soup.sum += b.hashes[t];
			soup.surfaces[tri.surface & 31]++;
			soup.bodies[std::min<size_t>(tri.body, kCollisionBodyCount - 1)]++;
		}
		b = Built(); // release as we go
		tick(progress);
	}
	soup.seconds_tiles = seconds_since(clock);
	// A streaming node without data names the zero chunk id: nothing is read for it (no bundle or toc holds it).
	std::erase_if(soup.sources, [](const SoupSource &p_s) { return p_s.kind == SoupSource::Kind::Chunk && p_s.id.is_zero(); });
	std::sort(soup.sources.begin(), soup.sources.end());
	soup.sources.erase(std::unique(soup.sources.begin(), soup.sources.end()), soup.sources.end());
	set_stage(progress, SoupProgress::Done, 0);
	return soup;
}

Result<LevelCollision> load_level_collision(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const SoupOptions &p_options) {
	LevelCollision out;
	SoupProgress *progress = p_options.progress;
	set_stage(progress, SoupProgress::Mount, 0);
	auto clock = std::chrono::steady_clock::now();
	LoadOptions load;
	load.threads = p_options.threads;
	Result<std::unique_ptr<LoadedLevel>> level = load_level(std::move(p_install), p_level_id, p_mode, load);
	if (!level) {
		return level.error();
	}
	out.level = std::move(*level);
	out.seconds_load = seconds_since(clock);
	clock = std::chrono::steady_clock::now();
	set_stage(progress, SoupProgress::Traverse, 0);
	Result<std::unique_ptr<World>> world = World::traverse(*out.level);
	if (!world) {
		return world.error();
	}
	out.world = std::move(*world);
	out.seconds_traverse = seconds_since(clock);
	for (const PlacedObject &object : out.world->objects) {
		if (object.kind == ObjectKind::AlternateSpawn) {
			out.spawns.push_back(object.transform);
		}
	}
	clock = std::chrono::steady_clock::now();
	Result<CollisionSoup> soup = build_collision_soup(*out.level, *out.world, p_options);
	if (!soup) {
		return soup.error();
	}
	out.soup = std::move(*soup);
	out.seconds_soup = seconds_since(clock);
	return out;
}

} // namespace bf3::core::level

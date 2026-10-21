#include "bf3/render/collision_prep.h"

#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <cmath>

namespace bf3::render {

namespace lvl = core::level;

namespace {

using C = std::array<float, 3>;

constexpr C kCharacterColour{ 1.00f, 0.62f, 0.10f };
constexpr C kInvisibleColour{ 0.85f, 0.20f, 0.85f };
constexpr C kTerrainColour{ 0.46f, 0.54f, 0.34f };

constexpr float kLight[3] = { 0.3363f, 0.8408f, 0.4243f };

float placement_jitter(uint32_t p_placement) {
	uint32_t h = p_placement * 2654435761u;
	h ^= h >> 15;
	return 0.94f + 0.12f * float(h & 0xFFFF) / 65535.0f;
}

}

std::array<float, 3> collision_surface_colour(uint8_t p_surface) {
	switch (p_surface) {
		case lvl::surface::kBark:
			return { 0.45f, 0.32f, 0.20f };
		case lvl::surface::kBrick:
			return { 0.72f, 0.36f, 0.28f };
		case lvl::surface::kCarpet:
			return { 0.55f, 0.25f, 0.35f };
		case lvl::surface::kCloth:
			return { 0.80f, 0.55f, 0.75f };
		case lvl::surface::kConcrete:
			return { 0.66f, 0.66f, 0.63f };
		case lvl::surface::kDirt:
			return { 0.55f, 0.42f, 0.27f };
		case lvl::surface::kFlesh:
			return { 0.90f, 0.60f, 0.50f };
		case lvl::surface::kFoliage:
			return { 0.25f, 0.70f, 0.25f };
		case lvl::surface::kGlass:
			return { 0.45f, 0.85f, 0.95f };
		case lvl::surface::kGrass:
			return { 0.45f, 0.75f, 0.30f };
		case lvl::surface::kGravel:
			return { 0.58f, 0.55f, 0.48f };
		case lvl::surface::kIce:
			return { 0.80f, 0.92f, 1.00f };
		case lvl::surface::kMetal:
			return { 0.40f, 0.55f, 0.80f };
		case lvl::surface::kMud:
			return { 0.40f, 0.30f, 0.18f };
		case lvl::surface::kPaper:
			return { 0.92f, 0.88f, 0.70f };
		case lvl::surface::kPlaster:
			return { 0.88f, 0.84f, 0.74f };
		case lvl::surface::kRock:
			return { 0.50f, 0.48f, 0.45f };
		case lvl::surface::kSand:
			return { 0.88f, 0.78f, 0.50f };
		case lvl::surface::kSnow:
			return { 0.95f, 0.95f, 0.98f };
		case lvl::surface::kWater:
			return { 0.15f, 0.35f, 0.85f };
		case lvl::surface::kWood:
			return { 0.75f, 0.52f, 0.28f };
		case lvl::surface::kAsphalt:
			return { 0.30f, 0.30f, 0.32f };
		case lvl::surface::kCeramic:
			return { 0.85f, 0.80f, 0.92f };
		case lvl::surface::kPlastic:
			return { 0.95f, 0.85f, 0.20f };
		case lvl::surface::kRubber:
			return { 0.18f, 0.18f, 0.18f };
		case lvl::surface::kCushion:
			return { 0.70f, 0.40f, 0.60f };
		case lvl::surface::kFruit:
			return { 0.95f, 0.45f, 0.20f };
		case lvl::surface::kPaintedMetal:
			return { 0.30f, 0.65f, 0.65f };
		case lvl::surface::kSlush:
			return { 0.70f, 0.75f, 0.80f };
		default:
			return { 0.60f, 0.60f, 0.60f };
	}
}

namespace {

void emit_triangle(PreparedCollisionMesh &r_m, const lvl::SoupTriangle &p_tri, bool &r_first) {
	C base = collision_surface_colour(p_tri.surface);
	if (p_tri.body == uint8_t(lvl::CollisionBody::Character)) {
		base = kCharacterColour;
	} else if ((p_tri.flags & lvl::kSoupInvisible) != 0) {
		base = kInvisibleColour;
	} else if ((p_tri.flags & lvl::kSoupTerrain) != 0) {
		base = kTerrainColour;
	}
	if ((p_tri.flags & lvl::kSoupSeeThrough) != 0) {
		for (float &c : base) {
			c = 0.55f * c + 0.45f;
		}
	}
	const core::res::Vec3 n = core::vec::normalize(
			core::vec::cross(core::vec::sub(p_tri.v[1], p_tri.v[0]), core::vec::sub(p_tri.v[2], p_tri.v[0])));
	const float facing = std::fabs(n[0] * kLight[0] + n[1] * kLight[1] + n[2] * kLight[2]);
	const float shade = (0.45f + 0.55f * (std::isfinite(facing) ? facing : 0.0f)) * placement_jitter(p_tri.placement);
	const float rgba[4] = { std::min(1.0f, base[0] * shade), std::min(1.0f, base[1] * shade), std::min(1.0f, base[2] * shade), 1.0f };
	static constexpr int kOrder[3] = { 0, 2, 1 };
	static constexpr float kBary[3][2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f } };
	for (int k = 0; k < 3; ++k) {
		const core::res::Vec3 &p = p_tri.v[kOrder[k]];
		r_m.positions.insert(r_m.positions.end(), { p[0], p[1], p[2] });
		r_m.colors.insert(r_m.colors.end(), rgba, rgba + 4);
		r_m.uvs.insert(r_m.uvs.end(), { kBary[k][0], kBary[k][1] });
		for (int a = 0; a < 3; ++a) {
			if (r_first) {
				r_m.min[a] = p[a];
				r_m.max[a] = p[a];
			} else {
				r_m.min[a] = std::min(r_m.min[a], p[a]);
				r_m.max[a] = std::max(r_m.max[a], p[a]);
			}
		}
		r_first = false;
	}
}

std::vector<PreparedCollisionMesh> prepare_tile_meshes(const lvl::CollisionSoup &p_soup, unsigned p_threads) {

	struct Slot {
		size_t tile = 0;
		uint8_t layer = 0;
		size_t triangles = 0;
	};
	std::vector<Slot> slots;
	for (size_t t = 0; t < p_soup.tiles.size(); ++t) {
		std::array<size_t, kCollisionLayers> counts{};
		for (const lvl::SoupTriangle &tri : p_soup.tiles[t].triangles) {
			counts[collision_layer(tri)]++;
		}
		for (size_t b = 0; b < counts.size(); ++b) {
			if (counts[b] > 0) {
				slots.push_back(Slot{ t, uint8_t(b), counts[b] });
			}
		}
	}
	std::vector<PreparedCollisionMesh> out(slots.size());
	core::parallel_for(
			slots.size(),
			[&](size_t p_index) {
				const Slot &slot = slots[p_index];
				const lvl::SoupTile &tile = p_soup.tiles[slot.tile];
				PreparedCollisionMesh &m = out[p_index];
				m.tile_x = tile.x;
				m.tile_z = tile.z;
				m.layer = slot.layer;
				m.triangles = slot.triangles;
				m.positions.reserve(slot.triangles * 9);
				m.colors.reserve(slot.triangles * 12);
				m.uvs.reserve(slot.triangles * 6);
				bool first = true;
				for (const lvl::SoupTriangle &tri : tile.triangles) {
					if (collision_layer(tri) == slot.layer) {
						emit_triangle(m, tri, first);
					}
				}
			},
			p_threads);
	return out;
}

float tile_distance(const std::array<int32_t, 2> &p_tile, const std::array<float, 2> &p_centre) {
	float d2 = 0.0f;
	for (int a = 0; a < 2; ++a) {
		const float lo = float(p_tile[size_t(a)]) * 64.0f, hi = lo + 64.0f;
		const float c = p_centre[size_t(a)];
		const float d = c < lo ? lo - c : (c > hi ? c - hi : 0.0f);
		d2 += d * d;
	}
	return std::sqrt(d2);
}

}

std::vector<std::array<int32_t, 2>> terrain_window_candidates(const CollisionPrepOptions &p_options) {
	std::vector<std::array<int32_t, 2>> out;
	const float radius = std::clamp(std::isfinite(p_options.terrain_radius) ? p_options.terrain_radius : 0.0f, 0.0f, kMaxTerrainWindowRadius);
	const std::array<float, 2> &c = p_options.terrain_centre;
	if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || std::fabs(c[0]) > 1e6f || std::fabs(c[1]) > 1e6f) {
		return out;
	}
	const int32_t x0 = int32_t(std::floor((c[0] - radius) / 64.0f)), x1 = int32_t(std::floor((c[0] + radius) / 64.0f));
	const int32_t z0 = int32_t(std::floor((c[1] - radius) / 64.0f)), z1 = int32_t(std::floor((c[1] + radius) / 64.0f));
	for (int32_t z = z0; z <= z1; ++z) {
		for (int32_t x = x0; x <= x1; ++x) {
			if (tile_distance({ x, z }, c) <= radius) {
				out.push_back({ x, z });
			}
		}
	}

	std::stable_sort(out.begin(), out.end(),
			[&](const std::array<int32_t, 2> &a, const std::array<int32_t, 2> &b) { return tile_distance(a, c) < tile_distance(b, c); });
	return out;
}

std::vector<PreparedCollisionMesh> prepare_terrain_window(const lvl::CollisionSoup &p_soup, const CollisionPrepOptions &p_options, TerrainWindowStats *r_stats) {
	TerrainWindowStats stats;
	stats.centre = p_options.terrain_centre;
	stats.radius = std::clamp(std::isfinite(p_options.terrain_radius) ? p_options.terrain_radius : 0.0f, 0.0f, kMaxTerrainWindowRadius);
	std::vector<PreparedCollisionMesh> out;
	bool any = false;
	for (const lvl::SoupTerrain &t : p_soup.terrains) {
		any = any || (t.state == lvl::SoupTerrain::State::Built && t.grid != nullptr);
	}
	if (!p_options.terrain || !any) {
		if (r_stats != nullptr) {
			*r_stats = std::move(stats);
		}
		return out;
	}
	const std::vector<std::array<int32_t, 2>> candidates = terrain_window_candidates(p_options);
	stats.tiles_considered = candidates.size();

	constexpr size_t kChunk = 32;
	struct Gathered {
		std::vector<lvl::SoupTriangle> triangles;
		bool failed = false;
	};
	std::vector<Gathered> chunk;
	for (size_t at = 0; at < candidates.size() && !stats.capped; at += kChunk) {
		const size_t n = std::min(kChunk, candidates.size() - at);
		chunk.assign(n, Gathered{});
		core::parallel_for(
				n,
				[&](size_t p_index) {
					Gathered &g = chunk[p_index];
					lvl::WorkBudget budget(kTerrainWindowTileBudget);
					for (size_t t = 0; t < p_soup.terrains.size(); ++t) {
						const lvl::SoupTerrain &terrain = p_soup.terrains[t];
						if (terrain.state != lvl::SoupTerrain::State::Built || terrain.grid == nullptr) {
							continue;
						}
						core::Result<size_t> added = lvl::append_soup_terrain_tile(
								terrain, uint32_t(p_soup.placements.size() + t), candidates[at + p_index], g.triangles, budget);
						if (!added) {
							g.failed = true;
							g.triangles.clear();
							return;
						}
					}
				},
				p_options.threads);
		for (size_t i = 0; i < n; ++i) {
			Gathered &g = chunk[i];
			if (g.failed) {
				++stats.tiles_failed;
				continue;
			}
			if (g.triangles.empty()) {
				continue;
			}
			if (stats.triangles + g.triangles.size() > p_options.max_terrain_triangles) {
				stats.capped = true;
				break;
			}
			stats.triangles += g.triangles.size();
			PreparedCollisionMesh m;
			m.tile_x = candidates[at + i][0];
			m.tile_z = candidates[at + i][1];
			m.layer = kTerrainLayer;
			m.triangles = g.triangles.size();
			m.positions.reserve(m.triangles * 9);
			m.colors.reserve(m.triangles * 12);
			m.uvs.reserve(m.triangles * 6);
			bool first = true;
			for (const lvl::SoupTriangle &tri : g.triangles) {
				emit_triangle(m, tri, first);
			}
			out.push_back(std::move(m));
			std::vector<lvl::SoupTriangle>().swap(g.triangles);
		}
	}

	std::sort(out.begin(), out.end(), [](const PreparedCollisionMesh &a, const PreparedCollisionMesh &b) {
		return std::make_pair(a.tile_x, a.tile_z) < std::make_pair(b.tile_x, b.tile_z);
	});
	stats.tiles = out.size();
	if (r_stats != nullptr) {
		*r_stats = std::move(stats);
	}
	return out;
}

std::vector<PreparedCollisionMesh> prepare_collision_meshes(const lvl::CollisionSoup &p_soup, unsigned p_threads) {
	CollisionPrepOptions options;
	options.threads = p_threads;
	options.terrain = false;
	return prepare_collision_meshes(p_soup, options);
}

std::vector<PreparedCollisionMesh> prepare_collision_meshes(const lvl::CollisionSoup &p_soup, const CollisionPrepOptions &p_options, TerrainWindowStats *r_stats) {
	std::vector<PreparedCollisionMesh> out = prepare_tile_meshes(p_soup, p_options.threads);
	std::vector<PreparedCollisionMesh> window = prepare_terrain_window(p_soup, p_options, r_stats);
	out.reserve(out.size() + window.size());
	for (PreparedCollisionMesh &m : window) {
		out.push_back(std::move(m));
	}
	return out;
}

}

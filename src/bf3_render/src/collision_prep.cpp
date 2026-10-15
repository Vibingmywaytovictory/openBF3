#include "bf3/render/collision_prep.h"

#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <cmath>

namespace bf3::render {

namespace lvl = core::level;

namespace {

using C = std::array<float, 3>;

constexpr C kCharacterColour{ 1.00f, 0.62f, 0.10f }; // RBTypeCharacter bodies (player clip)
constexpr C kInvisibleColour{ 0.85f, 0.20f, 0.85f }; // invisible blockers
constexpr C kTerrainColour{ 0.46f, 0.54f, 0.34f }; // terrain heightfield cells

// A fixed light from above and to the side, in Frostbite space.
constexpr float kLight[3] = { 0.3363f, 0.8408f, 0.4243f };

float placement_jitter(uint32_t p_placement) {
	uint32_t h = p_placement * 2654435761u;
	h ^= h >> 15;
	return 0.94f + 0.12f * float(h & 0xFFFF) / 65535.0f;
}

} // namespace

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

std::vector<PreparedCollisionMesh> prepare_collision_meshes(const lvl::CollisionSoup &p_soup, unsigned p_threads) {
	// Which (tile, layer) meshes exist and how many triangles each takes.
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
					if (collision_layer(tri) != slot.layer) {
						continue;
					}
					C base = collision_surface_colour(tri.surface);
					if (tri.body == uint8_t(lvl::CollisionBody::Character)) {
						base = kCharacterColour;
					} else if ((tri.flags & lvl::kSoupInvisible) != 0) {
						base = kInvisibleColour;
					} else if ((tri.flags & lvl::kSoupTerrain) != 0) {
						base = kTerrainColour;
					}
					if ((tri.flags & lvl::kSoupSeeThrough) != 0) {
						for (float &c : base) {
							c = 0.55f * c + 0.45f;
						}
					}
					const core::res::Vec3 n = core::res::vec::normalize(
							core::res::vec::cross(core::res::vec::sub(tri.v[1], tri.v[0]), core::res::vec::sub(tri.v[2], tri.v[0])));
					const float facing = std::fabs(n[0] * kLight[0] + n[1] * kLight[1] + n[2] * kLight[2]);
					const float shade = (0.45f + 0.55f * (std::isfinite(facing) ? facing : 0.0f)) * placement_jitter(tri.placement);
					const float rgba[4] = { std::min(1.0f, base[0] * shade), std::min(1.0f, base[1] * shade), std::min(1.0f, base[2] * shade), 1.0f };
					static constexpr int kOrder[3] = { 0, 2, 1 }; // Godot front faces are clockwise
					static constexpr float kBary[3][2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f } };
					for (int k = 0; k < 3; ++k) {
						const core::res::Vec3 &p = tri.v[kOrder[k]];
						m.positions.insert(m.positions.end(), { p[0], p[1], p[2] });
						m.colors.insert(m.colors.end(), rgba, rgba + 4);
						m.uvs.insert(m.uvs.end(), { kBary[k][0], kBary[k][1] });
						for (int a = 0; a < 3; ++a) {
							if (first) {
								m.min[a] = p[a];
								m.max[a] = p[a];
							} else {
								m.min[a] = std::min(m.min[a], p[a]);
								m.max[a] = std::max(m.max[a], p[a]);
							}
						}
						first = false;
					}
				}
			},
			p_threads);
	return out;
}

} // namespace bf3::render

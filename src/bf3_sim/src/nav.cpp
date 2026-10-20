#include "nav_internal.h"

#include "bf3/core/cache/digest.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/thread_pool.h"
#include "bf3/sim/soldier_motor.h"

#include <DetourAlloc.h>
#include <DetourCommon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

std::string bits_hex(float p_value) {
	uint32_t bits;
	std::memcpy(&bits, &p_value, sizeof(bits));
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%08x", bits);
	return buf;
}

uint32_t float_bits(float p_value) {
	uint32_t bits;
	std::memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

bool finite_positive(float p_v) {
	return std::isfinite(p_v) && p_v > 0.0f;
}

const NavTileInfo &empty_info() {
	static const NavTileInfo info;
	return info;
}

}

NavParams nav_params_from_motor(const MotorParams &p_motor) {
	NavParams p;
	if (finite_positive(p_motor.radius)) {
		p.agent_radius = p_motor.radius;
	}
	if (std::isfinite(p_motor.max_ascend_degrees) && p_motor.max_ascend_degrees > 0.0f && p_motor.max_ascend_degrees < 90.0f) {
		p.max_slope_degrees = p_motor.max_ascend_degrees;
	}
	const PoseParams &stand = p_motor.poses[size_t(Pose::Stand)];
	const PoseParams &crouch = p_motor.poses[size_t(Pose::Crouch)];

	if (finite_positive(stand.height)) {
		p.agent_height = stand.height;
	}
	if (finite_positive(crouch.height) && crouch.height <= p.agent_height) {
		p.crouch_height = crouch.height;
	}
	if (finite_positive(stand.step_height)) {
		p.agent_climb = stand.step_height;
	}
	return p;
}

std::string nav_params_text(const NavParams &p_params) {
	const NavParams &p = p_params;
	return "cs=" + bits_hex(p.cell_size) + ";ch=" + bits_hex(p.cell_height) + ";radius=" + bits_hex(p.agent_radius) + ";height=" + bits_hex(p.agent_height) +
			";crouch=" + bits_hex(p.crouch_height) + ";climb=" + bits_hex(p.agent_climb) + ";slope=" + bits_hex(p.max_slope_degrees) +
			";radius_up=" + (p.radius_round_up ? "1" : "0") + ";edge=" + bits_hex(p.max_edge_length) + ";simplify=" + bits_hex(p.max_simplification_error) +
			";min_region=" + std::to_string(p.min_region_cells) + ";merge_region=" + std::to_string(p.merge_region_cells) +
			";detail_dist=" + bits_hex(p.detail_sample_distance) + ";detail_err=" + bits_hex(p.detail_sample_max_error) +
			";partition=" + std::to_string(int(p.partition)) + ";drop=" + (p.drop_links ? "1" : "0") + ";drop_max=" + bits_hex(p.drop_max) +
			";drop_reach=" + bits_hex(p.drop_reach) + ";drop_spacing=" + bits_hex(p.drop_sample_spacing) + ";margin=" + bits_hex(p.region_margin);
}

std::string recast_build_id() {
#if defined(DT_POLYREF64)
	constexpr int kPolyRef64 = 1;
#else
	constexpr int kPolyRef64 = 0;
#endif
	return "recast=1.6.0;polyref64=" + std::to_string(kPolyRef64) + ";verts_per_poly=" + std::to_string(DT_VERTS_PER_POLYGON);
}

NavCells nav_cells(const NavParams &p_params) {
	const NavParams &p = p_params;
	NavCells c;
	c.walkable_height = int(std::ceil(p.crouch_height / p.cell_height - 1e-4f));
	c.stand_height = int(std::ceil(p.agent_height / p.cell_height - 1e-4f));

	c.walkable_climb = int(std::lround(p.agent_climb / p.cell_height));
	const float r = p.agent_radius / p.cell_size;
	c.walkable_radius = p.radius_round_up ? int(std::ceil(r - 1e-4f)) : int(std::floor(r + 0.5f));
	c.border = c.walkable_radius + 3;
	c.tile_cells = int(std::lround(kNavTileSize / p.cell_size));
	c.max_edge = int(p.max_edge_length / p.cell_size);
	return c;
}

core::Status check_nav_params(const NavParams &p_params) {
	const NavParams &p = p_params;
	const auto bad = [](const char *p_field, float p_value) { return core::make_error(std::string("nav params: ") + p_field + " " + std::to_string(p_value)); };
	if (!finite_positive(p.cell_size) || p.cell_size < 0.05f || p.cell_size > 4.0f) {
		return bad("cell_size", p.cell_size);
	}
	const float cells = kNavTileSize / p.cell_size;
	if (std::fabs(cells - std::round(cells)) > 1e-3f || cells > 4096.0f) {
		return bad("cell_size (must divide 64 m)", p.cell_size);
	}
	if (!finite_positive(p.cell_height) || p.cell_height < 0.01f || p.cell_height > 2.0f) {
		return bad("cell_height", p.cell_height);
	}
	if (!finite_positive(p.agent_radius) || p.agent_radius > 8.0f) {
		return bad("agent_radius", p.agent_radius);
	}
	if (!finite_positive(p.agent_height) || p.agent_height > 16.0f) {
		return bad("agent_height", p.agent_height);
	}
	if (!finite_positive(p.crouch_height) || p.crouch_height > p.agent_height) {
		return bad("crouch_height", p.crouch_height);
	}
	if (!std::isfinite(p.agent_climb) || p.agent_climb < 0.0f || p.agent_climb > 4.0f) {
		return bad("agent_climb", p.agent_climb);
	}
	const float climb_cells = p.agent_climb / p.cell_height;
	if (std::fabs(climb_cells - std::round(climb_cells)) > 1e-3f) {
		return bad("agent_climb (climb / cell_height must be a whole number of cells, topics/06 §2.4)", p.agent_climb);
	}
	if (!std::isfinite(p.max_slope_degrees) || p.max_slope_degrees <= 0.0f || p.max_slope_degrees >= 90.0f) {
		return bad("max_slope_degrees", p.max_slope_degrees);
	}
	if (!finite_positive(p.max_edge_length) || !finite_positive(p.max_simplification_error) || !finite_positive(p.detail_sample_distance) ||
			!finite_positive(p.detail_sample_max_error)) {
		return core::make_error("nav params: a Recast simplification value is not positive");
	}
	if (p.min_region_cells > (1u << 20) || p.merge_region_cells > (1u << 20)) {
		return core::make_error("nav params: region cell counts above 2^20");
	}
	if (uint8_t(p.partition) > uint8_t(NavPartition::Layers)) {
		return core::make_error("nav params: unknown partition");
	}
	if (p.drop_links && (!finite_positive(p.drop_max) || p.drop_max > 64.0f || !finite_positive(p.drop_reach) || p.drop_reach > 8.0f ||
							   !finite_positive(p.drop_sample_spacing) || p.drop_sample_spacing < 0.1f)) {
		return core::make_error("nav params: drop link values out of range");
	}
	if (!std::isfinite(p.region_margin) || p.region_margin < 0.0f || p.region_margin > 1024.0f) {
		return bad("region_margin", p.region_margin);
	}
	const NavCells c = nav_cells(p);
	if (c.walkable_height < 3 || c.walkable_height > 255 || c.stand_height > 255 || c.walkable_radius < 0 || c.walkable_radius > 255) {
		return core::make_error("nav params: cell counts outside Recast's ranges");
	}
	return core::Status::success();
}

int nav_recast_border_cells(const NavParams &p_params) {
	return nav_cells(p_params).border;
}

float nav_input_border(const NavParams &p_params) {
	const float recast = float(nav_recast_border_cells(p_params)) * p_params.cell_size;
	if (!p_params.drop_links) {
		return recast;
	}
	return std::max(recast, p_params.drop_reach + p_params.agent_radius + p_params.cell_size);
}

namespace {

class TileIndex {
public:
	explicit TileIndex(std::span<const NavTileKey> p_tiles) {
		if (p_tiles.empty()) {
			return;
		}
		x0_ = x1_ = p_tiles[0][0];
		z0_ = z1_ = p_tiles[0][1];
		for (const NavTileKey &t : p_tiles) {
			x0_ = std::min(x0_, t[0]);
			x1_ = std::max(x1_, t[0]);
			z0_ = std::min(z0_, t[1]);
			z1_ = std::max(z1_, t[1]);
		}
		const int64_t cells = (int64_t(x1_) - x0_ + 1) * (int64_t(z1_) - z0_ + 1);
		if (cells <= (int64_t(1) << 22)) {
			dense_.assign(size_t(cells), -1);
			for (size_t i = 0; i < p_tiles.size(); ++i) {
				dense_[size_t((int64_t(p_tiles[i][1]) - z0_) * (int64_t(x1_) - x0_ + 1) + (int64_t(p_tiles[i][0]) - x0_))] = int32_t(i);
			}
		} else {
			for (size_t i = 0; i < p_tiles.size(); ++i) {
				sparse_[key(p_tiles[i][0], p_tiles[i][1])] = int32_t(i);
			}
		}
		any_ = true;
	}
	bool any() const { return any_; }
	int32_t x0() const { return x0_; }
	int32_t x1() const { return x1_; }
	int32_t z0() const { return z0_; }
	int32_t z1() const { return z1_; }
	int32_t find(int32_t p_x, int32_t p_z) const {
		if (!dense_.empty()) {
			return dense_[size_t((int64_t(p_z) - z0_) * (int64_t(x1_) - x0_ + 1) + (int64_t(p_x) - x0_))];
		}
		const auto it = sparse_.find(key(p_x, p_z));
		return it == sparse_.end() ? -1 : it->second;
	}

private:
	static uint64_t key(int32_t p_x, int32_t p_z) { return (uint64_t(uint32_t(p_x)) << 32) | uint32_t(p_z); }
	bool any_ = false;
	int32_t x0_ = 0, x1_ = 0, z0_ = 0, z1_ = 0;
	std::vector<int32_t> dense_;
	std::unordered_map<uint64_t, int32_t> sparse_;
};

core::Status check_tiles(std::span<const NavTileKey> p_tiles) {
	if (p_tiles.size() > kMaxNavTiles) {
		return core::make_error("nav: " + std::to_string(p_tiles.size()) + " tiles exceed " + std::to_string(kMaxNavTiles));
	}
	for (size_t i = 0; i < p_tiles.size(); ++i) {
		const NavTileKey &t = p_tiles[i];
		if (std::abs(int64_t(t[0])) > (int64_t(1) << 20) || std::abs(int64_t(t[1])) > (int64_t(1) << 20)) {
			return core::make_error("nav: tile (" + std::to_string(t[0]) + ", " + std::to_string(t[1]) + ") is out of range");
		}
		if (i > 0 && !(p_tiles[i - 1] < t)) {
			return core::make_error("nav: the tiles are not sorted and unique");
		}
	}
	return core::Status::success();
}

struct GatherTerrain {
	const core::level::SoupTerrain *terrain = nullptr;
	uint32_t placement = 0;
	int32_t radius = 1;
};

constexpr size_t kNavTerrainBudget = size_t(1) << 28;

constexpr int32_t kMaxNavTerrainRadius = 4;

struct GatherTriangle {
	const std::array<Vec3, 3> *v = nullptr;
	uint8_t layer = 0;
	int32_t tx = 0, tz = 0;
};

template <class Next>
core::Result<std::vector<NavTileInput>> gather(std::span<const NavTileKey> p_tiles, const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads,
		Next &&p_next, std::span<const GatherTerrain> p_terrains = {}, std::span<const core::level::WaterPlane> p_water = {}) {
	BF3_TRY(check_nav_params(p_params));
	BF3_TRY(check_tiles(p_tiles));
	std::vector<NavTileInput> inputs(p_tiles.size());
	for (size_t i = 0; i < p_tiles.size(); ++i) {
		inputs[i].tile = p_tiles[i];
	}
	const TileIndex index(p_tiles);
	if (!index.any()) {
		return inputs;
	}
	const float b = nav_input_border(p_params);
	const float size = kNavTileSize;
	uint64_t charged = 0;
	GatherTriangle t;
	while (true) {
		BF3_TRY(p_next(t));
		if (t.v == nullptr) {
			break;
		}
		if (((p_layer_mask >> t.layer) & 1u) == 0) {
			continue;
		}
		const std::array<Vec3, 3> &v = *t.v;
		const float minx = std::min({ v[0][0], v[1][0], v[2][0] });
		const float maxx = std::max({ v[0][0], v[1][0], v[2][0] });
		const float minz = std::min({ v[0][2], v[1][2], v[2][2] });
		const float maxz = std::max({ v[0][2], v[1][2], v[2][2] });
		if (!std::isfinite(minx) || !std::isfinite(maxx) || !std::isfinite(minz) || !std::isfinite(maxz) || !std::isfinite(v[0][1]) ||
				!std::isfinite(v[1][1]) || !std::isfinite(v[2][1])) {
			return core::make_error("nav input: a non-finite triangle corner");
		}

		const double lx = std::floor((double(minx) - b) / size), hx = std::floor((double(maxx) + b) / size);
		const double lz = std::floor((double(minz) - b) / size), hz = std::floor((double(maxz) + b) / size);
		const int32_t x0 = int32_t(std::max<double>(lx, index.x0())), x1 = int32_t(std::min<double>(hx, index.x1()));
		const int32_t z0 = int32_t(std::max<double>(lz, index.z0())), z1 = int32_t(std::min<double>(hz, index.z1()));
		if (x0 > x1 || z0 > z1) {
			continue;
		}
		const uint64_t visits = uint64_t(int64_t(x1) - x0 + 1) * uint64_t(int64_t(z1) - z0 + 1);
		charged += visits;
		if (charged > kMaxNavInputRefs) {
			return core::make_error("nav input: more than " + std::to_string(kMaxNavInputRefs) + " (triangle, tile) pairs");
		}
		for (int32_t tz = z0; tz <= z1; ++tz) {
			for (int32_t tx = x0; tx <= x1; ++tx) {
				const int32_t at = index.find(tx, tz);
				if (at < 0) {
					continue;
				}
				NavTileInput &in = inputs[size_t(at)];
				if (in.layers.size() >= kMaxNavTileInputTriangles) {
					return core::make_error("nav input: tile (" + std::to_string(tx) + ", " + std::to_string(tz) + ") has more than " +
							std::to_string(kMaxNavTileInputTriangles) + " triangles");
				}
				for (const Vec3 &c : v) {
					in.vertices.insert(in.vertices.end(), c.begin(), c.end());
				}
				in.layers.push_back(t.layer);
				if (t.tx == tx && t.tz == tz) {
					++in.own_triangles;
				}
				if (std::abs(int64_t(t.tx) - tx) > 1 || std::abs(int64_t(t.tz) - tz) > 1) {
					++in.far_triangles;
				}
			}
		}
	}

	core::level::WorkBudget terrain_budget(kNavTerrainBudget);
	std::vector<std::optional<core::Error>> errors(inputs.size());
	const bool terrain_layer = ((p_layer_mask >> uint32_t(StaticLayer::Collision)) & 1u) != 0;
	std::vector<uint8_t> failed(inputs.size(), 0);
	core::parallel_for(
			inputs.size(),
			[&](size_t p_i) {
				NavTileInput &in = inputs[p_i];
				std::vector<core::level::SoupTriangle> scratch;
				for (const GatherTerrain &g : terrain_layer ? p_terrains : std::span<const GatherTerrain>()) {
					for (int32_t dx = -g.radius; dx <= g.radius; ++dx) {
						for (int32_t dz = -g.radius; dz <= g.radius; ++dz) {
							const std::array<int32_t, 2> soup_tile = { in.tile[0] + dx, in.tile[1] + dz };
							scratch.clear();
							core::Result<size_t> added = core::level::append_soup_terrain_tile(*g.terrain, g.placement, soup_tile, scratch, terrain_budget);
							if (!added) {
								errors[p_i] = added.error();
								return;
							}
							for (const core::level::SoupTriangle &t : scratch) {
								const std::array<Vec3, 3> &v = t.v;
								const float minx = std::min({ v[0][0], v[1][0], v[2][0] });
								const float maxx = std::max({ v[0][0], v[1][0], v[2][0] });
								const float minz = std::min({ v[0][2], v[1][2], v[2][2] });
								const float maxz = std::max({ v[0][2], v[1][2], v[2][2] });
								if (std::floor((double(minx) - b) / size) > in.tile[0] || std::floor((double(maxx) + b) / size) < in.tile[0] ||
										std::floor((double(minz) - b) / size) > in.tile[1] || std::floor((double(maxz) + b) / size) < in.tile[1]) {
									continue;
								}
								if (in.layers.size() >= kMaxNavTileInputTriangles) {
									errors[p_i] = core::make_error("nav input: tile (" + std::to_string(in.tile[0]) + ", " + std::to_string(in.tile[1]) +
											") has more than " + std::to_string(kMaxNavTileInputTriangles) + " triangles");
									return;
								}
								for (const Vec3 &c : v) {
									in.vertices.insert(in.vertices.end(), c.begin(), c.end());
								}
								in.layers.push_back(uint8_t(StaticLayer::Collision));
								++in.terrain_triangles;
								in.own_triangles += dx == 0 && dz == 0 ? 1 : 0;
								in.far_triangles += std::abs(dx) > 1 || std::abs(dz) > 1 ? 1 : 0;
							}
						}
					}
				}
				if (!p_water.empty() && in.terrain_triangles > 0) {
					const size_t first = in.layers.size() - in.terrain_triangles;
					in.unwalkable.assign(in.layers.size(), 0);
					bool any = false;
					for (size_t tri = first; tri < in.layers.size(); ++tri) {
						std::array<Vec3, 3> v;
						for (size_t k = 0; k < 3; ++k) {
							v[k] = { in.vertices[tri * 9 + k * 3], in.vertices[tri * 9 + k * 3 + 1], in.vertices[tri * 9 + k * 3 + 2] };
						}

						size_t units = 1;
						const float top = std::max({ v[0][1], v[1][1], v[2][1] });
						for (const core::level::WaterPlane &w : p_water) {
							units += w.y - core::level::kCoverWaterDepth > top ? w.polygon.size() : 0;
						}
						if (core::Status st = terrain_budget.charge(units); !st.ok()) {
							errors[p_i] = st.error();
							return;
						}
						const bool wet = nav_terrain_wet(v, p_water);
						in.unwalkable[tri] = wet ? 1 : 0;
						any = any || wet;
					}
					if (!any) {
						in.unwalkable.clear();
					}
				}
				core::Hasher h(core::HashAlgorithm::Sha256);
				std::vector<uint8_t> chunk;
				chunk.reserve(64 * 1024);
				const auto put = [&](uint32_t p_v) {
					for (int k = 0; k < 4; ++k) {
						chunk.push_back(uint8_t(p_v >> (k * 8)));
					}
				};
				put(uint32_t(in.tile[0]));
				put(uint32_t(in.tile[1]));
				for (size_t tri = 0; tri < in.layers.size(); ++tri) {
					if (chunk.size() + 40 > chunk.capacity()) {
						h.update(chunk);
						chunk.clear();
					}
					for (size_t k = 0; k < 9; ++k) {
						put(float_bits(in.vertices[tri * 9 + k]));
					}
					chunk.push_back(in.layers[tri]);
				}
				if (!in.unwalkable.empty()) {
					chunk.push_back('W');
					chunk.insert(chunk.end(), in.unwalkable.begin(), in.unwalkable.end());
				}
				h.update(chunk);
				failed[p_i] = h.finish(in.digest.bytes) ? 0 : 1;
			},
			p_threads);
	for (const std::optional<core::Error> &e : errors) {
		if (e) {
			return *e;
		}
	}
	for (uint8_t f : failed) {
		if (f != 0) {
			return core::make_error("nav input: SHA-256 failed");
		}
	}
	return inputs;
}

core::Result<std::vector<GatherTerrain>> gather_terrains(const core::level::CollisionSoup &p_soup, const NavParams &p_params) {
	std::vector<GatherTerrain> out;
	const double border = nav_input_border(p_params);
	for (size_t i = 0; i < p_soup.terrains.size(); ++i) {
		const core::level::SoupTerrain &t = p_soup.terrains[i];
		if (t.state != core::level::SoupTerrain::State::Built) {
			continue;
		}
		if (t.grid == nullptr) {
			return core::make_error("nav input: terrain object " + std::to_string(t.object) + " has no terrain record");
		}
		const core::res::HeightGrid &g = *t.grid;
		const auto length = [](const Vec3 &p_v) { return std::sqrt(double(p_v[0]) * p_v[0] + double(p_v[1]) * p_v[1] + double(p_v[2]) * p_v[2]); };
		const std::array<Vec3, 3> &a = t.frame.axis;

		double reach = length(a[0]) * std::fabs(double(g.sample_spacing[0])) + length(a[2]) * std::fabs(double(g.sample_spacing[1]));
		if (a[1][0] != 0.0f || a[1][2] != 0.0f || a[0][1] != 0.0f || a[2][1] != 0.0f) {
			double lo = std::numeric_limits<double>::infinity(), hi = -lo;
			for (float y : g.heights) {
				if (std::isfinite(y)) {
					lo = std::min(lo, double(y));
					hi = std::max(hi, double(y));
				}
			}
			reach += lo <= hi ? length(a[1]) * (hi - lo) : 0.0;
		}
		const double r = 1.0 + std::floor((reach + border) / double(kNavTileSize));
		if (!std::isfinite(r) || r > double(kMaxNavTerrainRadius)) {
			return core::make_error("nav input: terrain object " + std::to_string(t.object) + "'s cells reach past " + std::to_string(kMaxNavTerrainRadius) +
					" tiles");
		}
		out.push_back({ &t, uint32_t(p_soup.placements.size() + i), int32_t(r) });
	}
	return out;
}

}

bool nav_terrain_wet(const std::array<Vec3, 3> &p_triangle, std::span<const core::level::WaterPlane> p_water) {

	const float top = std::max({ p_triangle[0][1], p_triangle[1][1], p_triangle[2][1] });
	if (!std::isfinite(top)) {
		return false;
	}
	const float cx = (p_triangle[0][0] + p_triangle[1][0] + p_triangle[2][0]) / 3.0f;
	const float cz = (p_triangle[0][2] + p_triangle[1][2] + p_triangle[2][2]) / 3.0f;
	for (const core::level::WaterPlane &w : p_water) {
		if (w.y - core::level::kCoverWaterDepth > top && core::level::water_plane_covers(w, cx, cz)) {
			return true;
		}
	}
	return false;
}

core::Result<std::vector<NavTileInput>> nav_tile_inputs_threads(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads, std::span<const core::level::WaterPlane> p_water) {
	BF3_TRY(check_nav_params(p_params));
	core::Result<std::vector<GatherTerrain>> terrains = gather_terrains(p_soup, p_params);
	if (!terrains) {
		return terrains.error();
	}
	size_t tile = 0;
	size_t tri = 0;
	return gather(p_tiles, p_params, p_layer_mask, p_threads, [&](GatherTriangle &r_t) -> core::Status {
		while (tile < p_soup.tiles.size() && tri >= p_soup.tiles[tile].triangles.size()) {
			++tile;
			tri = 0;
		}
		if (tile >= p_soup.tiles.size()) {
			r_t.v = nullptr;
			return core::Status::success();
		}
		const core::level::SoupTile &st = p_soup.tiles[tile];
		const core::level::SoupTriangle &s = st.triangles[tri++];
		const std::optional<StaticLayer> layer = static_layer_of(s.body);
		if (!layer) {
			return core::make_error("nav input: soup body " + std::to_string(int(s.body)) + " has no layer");
		}
		r_t.v = &s.v;
		r_t.layer = uint8_t(*layer);
		r_t.tx = st.x;
		r_t.tz = st.z;
		return core::Status::success();
	}, *terrains, p_water);
}

core::Result<std::vector<NavTileInput>> nav_tile_inputs_triangles(std::span<const StaticTriangle> p_triangles, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads) {

	struct Keyed {
		std::array<int32_t, 2> tile;
		uint32_t index;
	};
	if (p_triangles.size() > core::level::kMaxSoupTriangles) {
		return core::make_error("nav input: too many triangles");
	}
	std::vector<Keyed> keyed;
	keyed.reserve(p_triangles.size());
	for (size_t i = 0; i < p_triangles.size(); ++i) {
		std::array<int64_t, 9> q;
		if (!core::level::soup_quantise(p_triangles[i].v, q)) {
			return core::make_error("nav input: triangle " + std::to_string(i) + " has a non-finite or out-of-range corner");
		}
		if (uint8_t(p_triangles[i].layer) >= kStaticLayerCount) {
			return core::make_error("nav input: triangle " + std::to_string(i) + " has a bad layer");
		}
		keyed.push_back({ core::level::soup_tile_of(q), uint32_t(i) });
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed &p_a, const Keyed &p_b) { return p_a.tile < p_b.tile; });
	size_t at = 0;
	return gather(p_tiles, p_params, p_layer_mask, p_threads, [&](GatherTriangle &r_t) -> core::Status {
		if (at >= keyed.size()) {
			r_t.v = nullptr;
			return core::Status::success();
		}
		const Keyed &k = keyed[at++];
		const StaticTriangle &s = p_triangles[k.index];
		r_t.v = &s.v;
		r_t.layer = uint8_t(s.layer);
		r_t.tx = k.tile[0];
		r_t.tz = k.tile[1];
		return core::Status::success();
	});
}

core::Result<std::vector<NavTileInput>> nav_tile_inputs(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles, const NavParams &p_params,
		uint32_t p_layer_mask) {
	return nav_tile_inputs_threads(p_soup, p_tiles, p_params, p_layer_mask, 0, {});
}

core::Result<std::vector<NavTileInput>> nav_tile_inputs(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles, const NavParams &p_params,
		uint32_t p_layer_mask, std::span<const core::level::WaterPlane> p_water) {
	return nav_tile_inputs_threads(p_soup, p_tiles, p_params, p_layer_mask, 0, p_water);
}

uint32_t NavWorld::Impl::global(NavPolyRef p_ref) const {
	if (mesh == nullptr || p_ref == 0) {
		return UINT32_MAX;
	}
	unsigned int salt = 0, it = 0, ip = 0;
	mesh->decodePolyId(dtPolyRef(p_ref), salt, it, ip);
	if (it >= poly_base.size() || ip >= tile_polys[it]) {
		return UINT32_MAX;
	}
	return poly_base[it] + ip;
}

NavPolyRef NavWorld::Impl::ref_of(uint32_t p_global) const {
	if (mesh == nullptr || p_global >= polys) {
		return 0;
	}

	const auto it = std::upper_bound(poly_base.begin(), poly_base.end(), p_global);
	size_t tile = size_t(it - poly_base.begin());
	while (tile > 0) {
		--tile;
		if (tile_polys[tile] > 0 && p_global >= poly_base[tile] && p_global < poly_base[tile] + tile_polys[tile]) {
			const dtMeshTile *t = static_cast<const dtNavMesh *>(mesh.get())->getTile(int(tile));
			return NavPolyRef(mesh->getPolyRefBase(t) | dtPolyRef(p_global - poly_base[tile]));
		}
		if (tile_polys[tile] > 0) {
			break;
		}
	}
	return 0;
}

NavWorld::NavWorld(std::unique_ptr<Impl> p_impl) :
		impl_(std::move(p_impl)) {}

NavWorld::~NavWorld() = default;

const NavParams &NavWorld::params() const {
	return impl_->params;
}

size_t NavWorld::tile_count() const {
	return impl_->tiles.size();
}

const NavTileInfo &NavWorld::tile(size_t p_index) const {
	return p_index < impl_->tiles.size() ? impl_->tiles[p_index] : empty_info();
}

const NavStats &NavWorld::stats() const {
	return impl_->stats;
}

size_t NavWorld::poly_count() const {
	return impl_->polys;
}

Vec3 NavWorld::min() const {
	return impl_->min;
}

Vec3 NavWorld::max() const {
	return impl_->max;
}

core::Result<std::shared_ptr<const NavWorld>> NavWorldBuilder::finish(const NavParams &p_params, std::vector<NavTileInfo> p_tiles,
		std::vector<std::vector<uint8_t>> p_data, NavStats p_stats) {
	const Clock::time_point start = Clock::now();
	if (p_tiles.size() != p_data.size()) {
		return core::make_error("nav world: tile infos and data differ in count");
	}
	if (p_tiles.size() > kMaxNavTiles) {
		return core::make_error("nav world: too many tiles");
	}
	for (size_t i = 1; i < p_tiles.size(); ++i) {
		if (!(p_tiles[i - 1].tile < p_tiles[i].tile)) {
			return core::make_error("nav world: tiles not in (x, z) order");
		}
	}
	auto impl = std::make_unique<NavWorld::Impl>();
	NavWorld::Impl &w = *impl;
	w.params = p_params;
	size_t present = 0;
	for (const std::vector<uint8_t> &d : p_data) {
		present += d.empty() ? 0 : 1;
	}
	w.mesh.reset(dtAllocNavMesh());
	if (w.mesh == nullptr) {
		return core::make_error("nav world: dtAllocNavMesh failed");
	}
	dtNavMeshParams np{};
	np.orig[0] = np.orig[1] = np.orig[2] = 0.0f;
	np.tileWidth = kNavTileSize;
	np.tileHeight = kNavTileSize;
	np.maxTiles = int(dtNextPow2(unsigned(std::max<size_t>(present, 1))));
	np.maxPolys = 1 << DT_POLY_BITS;
	if (dtStatusFailed(w.mesh->init(&np))) {
		return core::make_error("nav world: dtNavMesh::init failed for " + std::to_string(present) + " tiles");
	}
	w.poly_base.assign(size_t(np.maxTiles), 0);
	w.tile_polys.assign(size_t(np.maxTiles), 0);
	bool any = false;
	uint32_t running = 0;
	p_stats.tiles = 0;
	p_stats.tiles_empty = 0;
	p_stats.polys = 0;
	p_stats.drop_links = 0;
	p_stats.data_bytes = 0;
	for (size_t i = 0; i < p_tiles.size(); ++i) {
		const std::vector<uint8_t> &d = p_data[i];
		if (d.empty()) {
			++p_stats.tiles_empty;
			continue;
		}
		unsigned char *copy = static_cast<unsigned char *>(dtAlloc(d.size(), DT_ALLOC_PERM));
		if (copy == nullptr) {
			return core::make_error("nav world: out of memory");
		}
		std::memcpy(copy, d.data(), d.size());
		dtTileRef ref = 0;
		const dtStatus st = w.mesh->addTile(copy, int(d.size()), DT_TILE_FREE_DATA, 0, &ref);
		if (dtStatusFailed(st)) {
			dtFree(copy);
			return core::make_error("nav world: Detour refused tile (" + std::to_string(p_tiles[i].tile[0]) + ", " + std::to_string(p_tiles[i].tile[1]) +
					"), status " + std::to_string(st));
		}
		const unsigned int index = w.mesh->decodePolyIdTile(dtPolyRef(ref));
		const dtMeshTile *tile = static_cast<const dtNavMesh *>(w.mesh.get())->getTile(int(index));
		w.poly_base[index] = running;
		w.tile_polys[index] = uint32_t(tile->header->polyCount);
		running += uint32_t(tile->header->polyCount);
		++p_stats.tiles;
		p_stats.polys += size_t(tile->header->polyCount - tile->header->offMeshConCount);
		p_stats.drop_links += size_t(tile->header->offMeshConCount);
		p_stats.data_bytes += d.size();
		for (int k = 0; k < 3; ++k) {
			w.min[k] = any ? std::min(w.min[k], tile->header->bmin[k]) : tile->header->bmin[k];
			w.max[k] = any ? std::max(w.max[k], tile->header->bmax[k]) : tile->header->bmax[k];
		}
		any = true;
	}

	for (size_t i = 0; i < w.poly_base.size(); ++i) {
		if (w.tile_polys[i] == 0) {
			w.poly_base[i] = i == 0 ? 0 : w.poly_base[i - 1] + w.tile_polys[i - 1];
		}
	}
	w.polys = running;
	w.tiles = std::move(p_tiles);
	w.data = std::move(p_data);
	p_stats.seconds_assemble = seconds_since(start);
	if (p_stats.seconds_total == 0.0) {
		p_stats.seconds_total = p_stats.seconds_input + p_stats.seconds_bake + p_stats.seconds_restore + p_stats.seconds_assemble;
	}
	w.stats = p_stats;
	return std::shared_ptr<const NavWorld>(new NavWorld(std::move(impl)));
}

uint32_t nav_poly_component(const NavWorld &p_world, const NavComponents &p_components, NavPolyRef p_poly) {
	if (p_poly == 0) {
		return UINT32_MAX;
	}
	const uint32_t g = p_world.impl().global(p_poly);
	return g != UINT32_MAX && g < p_components.component.size() ? p_components.component[g] : UINT32_MAX;
}

NavPointCheck nav_check_point(const NavWorld &p_world, const NavComponents &p_components, const Vec3 &p_point, const Vec3 &p_half_extents) {
	NavPointCheck out;
	const NavWorld::Impl &w = p_world.impl();
	if (w.mesh == nullptr || w.polys == 0) {
		return out;
	}
	std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter> q(dtAllocNavMeshQuery());
	if (q == nullptr || dtStatusFailed(q->init(w.mesh.get(), 64))) {
		return out;
	}
	dtQueryFilter filter;
	filter.setIncludeFlags(nav_flags::kAll);
	dtPolyRef ref = 0;
	float nearest[3] = { 0, 0, 0 };
	if (dtStatusFailed(q->findNearestPoly(p_point.data(), p_half_extents.data(), &filter, &ref, nearest)) || ref == 0) {
		return out;
	}
	out.poly = NavPolyRef(ref);
	out.nearest = { nearest[0], nearest[1], nearest[2] };
	const float dx = nearest[0] - p_point[0], dz = nearest[2] - p_point[2];
	out.horizontal = std::sqrt(dx * dx + dz * dz);
	out.vertical = nearest[1] - p_point[1];
	const uint32_t g = w.global(out.poly);
	if (g != UINT32_MAX && g < p_components.component.size()) {
		out.component = p_components.component[g];
	}
	if (g != UINT32_MAX && g < p_components.weak_component.size()) {
		out.weak_component = p_components.weak_component[g];
	}
	return out;
}

NavDebugMesh nav_debug_mesh(const NavWorld &p_world, const NavComponents *p_components) {
	NavDebugMesh out;
	const NavWorld::Impl &w = p_world.impl();
	if (w.mesh == nullptr) {
		return out;
	}
	const dtNavMesh &mesh = *w.mesh;
	constexpr float kLift = 0.05f;
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const dtPoly &poly = tile->polys[pi];
			const uint32_t g = w.poly_base[size_t(ti)] + uint32_t(pi);
			const uint32_t comp = p_components != nullptr && g < p_components->component.size() ? p_components->component[g] : 0;
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
				const int con = pi - tile->header->offMeshBase;
				if (con >= 0 && con < tile->header->offMeshConCount) {
					const float *p = tile->offMeshCons[con].pos;
					out.links.push_back({ p[0], p[1] + kLift, p[2] });
					out.links.push_back({ p[3], p[4] + kLift, p[5] });
				}
				continue;
			}
			const dtPolyDetail &pd = tile->detailMeshes[pi];
			for (int j = 0; j < pd.triCount; ++j) {
				const unsigned char *t = &tile->detailTris[(pd.triBase + unsigned(j)) * 4];
				for (int k = 0; k < 3; ++k) {
					const float *v = t[k] < poly.vertCount ? &tile->verts[poly.verts[t[k]] * 3] : &tile->detailVerts[(pd.vertBase + (t[k] - poly.vertCount)) * 3];
					out.triangles.push_back({ v[0], v[1] + kLift, v[2] });
				}
				out.triangle_component.push_back(comp);
				out.triangle_area.push_back(poly.getArea());
			}
		}
	}
	return out;
}

void nav_poly_area_flags(const NavWorld &p_world, NavPolyRef p_poly, uint8_t *r_area, uint16_t *r_flags) {
	uint8_t area = 0;
	uint16_t flags = 0;
	const NavWorld::Impl &w = p_world.impl();
	const dtMeshTile *tile = nullptr;
	const dtPoly *poly = nullptr;
	if (w.mesh != nullptr && dtStatusSucceed(w.mesh->getTileAndPolyByRef(dtPolyRef(p_poly), &tile, &poly))) {
		area = poly->getArea();
		flags = poly->flags;
	}
	if (r_area != nullptr) {
		*r_area = area;
	}
	if (r_flags != nullptr) {
		*r_flags = flags;
	}
}

bool nav_offmesh_end(const NavWorld &p_world, NavPolyRef p_poly, Vec3 *r_start, Vec3 *r_end) {
	const NavWorld::Impl &w = p_world.impl();
	if (w.mesh == nullptr) {
		return false;
	}
	const dtOffMeshConnection *con = w.mesh->getOffMeshConnectionByRef(dtPolyRef(p_poly));
	if (con == nullptr) {
		return false;
	}
	if (r_start != nullptr) {
		*r_start = { con->pos[0], con->pos[1], con->pos[2] };
	}
	if (r_end != nullptr) {
		*r_end = { con->pos[3], con->pos[4], con->pos[5] };
	}
	return true;
}

}

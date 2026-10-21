#include "static_world_internal.h"

#include "jolt_runtime.h"

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/terrain.h"

#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kMaxHeightSpan = 1e6;
constexpr double kMaxCoordinate = 1e6;

constexpr double kFastStep = 0.01;

std::string where_of(const StaticTerrainView &p_view) {
	return "static terrain (object " + std::to_string(p_view.object) + ")";
}

void put_u32(std::vector<uint8_t> &r_out, uint32_t p_value) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_value >> (i * 8)));
	}
}

void put_u64(std::vector<uint8_t> &r_out, uint64_t p_value) {
	for (int i = 0; i < 8; ++i) {
		r_out.push_back(uint8_t(p_value >> (i * 8)));
	}
}

void put_f32(std::vector<uint8_t> &r_out, float p_value) {
	uint32_t bits;
	std::memcpy(&bits, &p_value, sizeof(bits));
	put_u32(r_out, bits);
}

double dot3(const Vec3 &p_a, const Vec3 &p_b) {
	return double(p_a[0]) * p_b[0] + double(p_a[1]) * p_b[1] + double(p_a[2]) * p_b[2];
}

std::array<Vec3, 3> face_up(const std::array<Vec3, 3> &p_t) {
	const Vec3 &a = p_t[0];
	const Vec3 &b = p_t[1];
	const Vec3 &c = p_t[2];
	const Vec3 u = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const Vec3 v = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	if (u[2] * v[0] - u[0] * v[2] < 0.0f) {
		return { a, c, b };
	}
	return p_t;
}

struct Frame {
	core::res::Transform transform;
	double scale = 1.0;
	JPH::Quat rotation = JPH::Quat::sIdentity();
};

core::Result<Frame> check_view(const StaticTerrainView &p_view) {
	const std::string where = where_of(p_view);
	if (p_view.width < 2 || p_view.height < 2 || p_view.width > core::res::kMaxGridSide || p_view.height > core::res::kMaxGridSide) {
		return core::make_error(where + ": grid " + std::to_string(p_view.width) + " x " + std::to_string(p_view.height) + " is outside 2.." +
				std::to_string(core::res::kMaxGridSide));
	}
	const size_t samples = p_view.width * p_view.height;
	if (p_view.heights.size() != samples || p_view.holes.size() != samples) {
		return core::make_error(where + ": " + std::to_string(p_view.heights.size()) + " heights and " + std::to_string(p_view.holes.size()) +
				" hole bytes for " + std::to_string(samples) + " samples");
	}
	for (float v : { p_view.origin_xz[0], p_view.origin_xz[1], p_view.spacing[0], p_view.spacing[1] }) {
		if (!std::isfinite(v)) {
			return core::make_error(where + ": non-finite origin or spacing");
		}
	}
	if (!(p_view.spacing[0] > 0.0f) || !(p_view.spacing[1] > 0.0f)) {
		return core::make_error(where + ": spacing must be positive");
	}
	Frame f;
	f.transform = core::Transform::from_axes(p_view.axes, p_view.translation);
	for (const Vec3 &a : p_view.axes) {
		for (float c : a) {
			if (!std::isfinite(c)) {
				return core::make_error(where + ": non-finite frame");
			}
		}
	}
	for (float c : p_view.translation) {
		if (!std::isfinite(c) || std::fabs(c) > kMaxCoordinate) {
			return core::make_error(where + ": frame translation is non-finite or past 1e6 m");
		}
	}
	const double s = std::sqrt(dot3(p_view.axes[0], p_view.axes[0]));
	if (!(s > 1e-6) || !(s < 1e6)) {
		return core::make_error(where + ": frame scale " + std::to_string(s) + " is outside (1e-6, 1e6)");
	}
	for (size_t i = 0; i < 3; ++i) {
		const double len = std::sqrt(dot3(p_view.axes[i], p_view.axes[i]));
		if (std::fabs(len - s) > 1e-4 * s) {
			return core::make_error(where + ": the frame's scale is not uniform (a rotation times a positive uniform scale is required)");
		}
		for (size_t j = i + 1; j < 3; ++j) {
			if (std::fabs(dot3(p_view.axes[i], p_view.axes[j])) > 1e-4 * s * s) {
				return core::make_error(where + ": the frame's axes are not orthogonal (a shear)");
			}
		}
	}
	const Vec3 &a = p_view.axes[0];
	const Vec3 &b = p_view.axes[1];
	const Vec3 &c = p_view.axes[2];
	const double det = double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) - double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
			double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0]);
	if (!(det > 0.0)) {
		return core::make_error(where + ": the frame mirrors (determinant " + std::to_string(det) + ")");
	}
	f.scale = s;

	const float inv = float(1.0 / s);
	const JPH::Mat44 m(JPH::Vec4(a[0] * inv, a[1] * inv, a[2] * inv, 0.0f), JPH::Vec4(b[0] * inv, b[1] * inv, b[2] * inv, 0.0f),
			JPH::Vec4(c[0] * inv, c[1] * inv, c[2] * inv, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
	const bool identity = a == Vec3{ 1.0f, 0.0f, 0.0f } && b == Vec3{ 0.0f, 1.0f, 0.0f } && c == Vec3{ 0.0f, 0.0f, 1.0f };
	f.rotation = identity ? JPH::Quat::sIdentity() : m.GetQuaternion().Normalized();
	return f;
}

struct CellPair {
	std::array<std::array<Vec3, 3>, 2> v{};
	std::array<bool, 2> kept{};
	bool open = false;
};

void soup_cell(const StaticTerrainView &p_view, const core::res::Transform &p_frame, size_t p_x, size_t p_z, CellPair &r_out) {
	r_out = CellPair();
	const size_t w = p_view.width;
	const auto hole = [&](size_t p_i, size_t p_j) { return p_view.holes[(p_x + p_i) + (p_z + p_j) * w] != 0; };
	if (hole(0, 0) || hole(1, 0) || hole(0, 1) || hole(1, 1)) {
		r_out.open = true;
		return;
	}
	const float ox = p_view.origin_xz[0];
	const float oz = p_view.origin_xz[1];
	const float sx = p_view.spacing[0];
	const float sz = p_view.spacing[1];
	const auto point = [&](size_t p_i, size_t p_j) {
		return p_frame.transform_point({ ox + float(p_x + p_i) * sx, p_view.heights[(p_x + p_i) + (p_z + p_j) * w], oz + float(p_z + p_j) * sz });
	};
	const Vec3 a = point(0, 0);
	const Vec3 b = point(1, 0);
	const Vec3 c = point(0, 1);
	const Vec3 d = point(1, 1);
	const std::array<std::array<Vec3, 3>, 2> pair = { { { a, d, b }, { a, c, d } } };
	for (size_t k = 0; k < 2; ++k) {
		r_out.v[k] = face_up(pair[k]);
		std::array<int64_t, 9> q;
		r_out.kept[k] = core::level::soup_quantise(r_out.v[k], q) && !core::level::soup_degenerate(q);
	}
}

bool jolt_keeps(const std::vector<uint8_t> &p_mask, size_t p_w, size_t p_x, size_t p_z, int p_t) {
	const size_t a = p_x + p_z * p_w;
	const size_t b = a + 1;
	const size_t c = a + p_w;
	const size_t d = c + 1;
	if (p_mask[a] != 0 || p_mask[d] != 0) {
		return false;
	}
	return p_t == 0 ? p_mask[c] == 0 : p_mask[b] == 0;
}

void grow(Vec3 &r_min, Vec3 &r_max, const Vec3 &p_v, bool &r_any) {
	for (size_t k = 0; k < 3; ++k) {
		r_min[k] = r_any ? std::min(r_min[k], p_v[k]) : p_v[k];
		r_max[k] = r_any ? std::max(r_max[k], p_v[k]) : p_v[k];
	}
	r_any = true;
}

}

core::Result<StaticTerrainView> static_terrain_view(const core::level::SoupTerrain &p_terrain) {
	if (p_terrain.state != core::level::SoupTerrain::State::Built) {
		return core::make_error("static terrain (object " + std::to_string(p_terrain.object) + "): not built");
	}
	if (p_terrain.grid == nullptr) {
		return core::make_error("static terrain (object " + std::to_string(p_terrain.object) + "): no terrain record");
	}
	const core::res::HeightGrid &g = *p_terrain.grid;
	StaticTerrainView view;
	view.object = p_terrain.object;
	view.width = g.width;
	view.height = g.height;
	view.origin_xz = g.origin_xz;
	view.spacing = g.sample_spacing;
	view.heights = g.heights;
	view.holes = g.holes;
	view.axes = p_terrain.frame.axes();
	view.translation = p_terrain.frame.trans;
	if (view.heights.size() != g.width * g.height || view.holes.size() != g.width * g.height) {
		return core::make_error("static terrain (object " + std::to_string(p_terrain.object) + "): the record's arrays do not match its size");
	}
	return view;
}

core::Sha256Digest static_terrain_content(const StaticTerrainView &p_view) {
	core::Hasher hasher(core::HashAlgorithm::Sha256);
	std::vector<uint8_t> head;
	put_u64(head, p_view.width);
	put_u64(head, p_view.height);
	for (float v : { p_view.origin_xz[0], p_view.origin_xz[1], p_view.spacing[0], p_view.spacing[1] }) {
		put_f32(head, v);
	}
	for (const Vec3 &a : p_view.axes) {
		for (float c : a) {
			put_f32(head, c);
		}
	}
	for (float c : p_view.translation) {
		put_f32(head, c);
	}
	hasher.update(head);

	hasher.update(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(p_view.heights.data()), p_view.heights.size() * sizeof(float)));
	std::vector<uint8_t> chunk;
	chunk.reserve(1 << 16);
	for (size_t i = 0; i < p_view.holes.size(); ++i) {
		chunk.push_back(p_view.holes[i] != 0 ? 1 : 0);
		if (chunk.size() == chunk.capacity()) {
			hasher.update(chunk);
			chunk.clear();
		}
	}
	hasher.update(chunk);
	core::Sha256Digest out{};
	hasher.finish(out.bytes);
	return out;
}

core::Result<StaticWorldShapes::Impl::Terrain> cook_static_terrain(const StaticTerrainView &p_view, const StaticWorldBuildOptions &p_options,
		const core::Sha256Digest *p_content) {
	const std::string where = where_of(p_view);
	core::Result<Frame> frame_result = check_view(p_view);
	if (!frame_result) {
		return frame_result.error();
	}
	const Frame &frame = *frame_result;
	const size_t w = p_view.width;
	const size_t h = p_view.height;
	const size_t cells_x = w - 1;
	const size_t cells_z = h - 1;

	core::level::WorkBudget budget(core::res::kMaxGridSide * core::res::kMaxGridSide * 3);
	BF3_TRY(budget.charge(w * h));

	StaticWorldShapes::Impl::Terrain out;
	StaticTerrainInfo &info = out.info;
	info.object = p_view.object;
	info.width = uint32_t(w);
	info.height = uint32_t(h);
	info.content = p_content != nullptr ? *p_content : static_terrain_content(p_view);

	std::vector<uint8_t> mask(w * h, 0);
	double lo = std::numeric_limits<double>::infinity();
	double hi = -lo;
	size_t holes = 0;
	for (size_t i = 0; i < w * h; ++i) {
		const float y = p_view.heights[i];
		const bool hole = p_view.holes[i] != 0;
		holes += hole ? 1 : 0;
		if (hole || !std::isfinite(y)) {
			mask[i] = 1;
			continue;
		}
		lo = std::min(lo, double(y));
		hi = std::max(hi, double(y));
	}
	info.holes = uint32_t(holes);
	const bool any_sample = lo <= hi;
	if (any_sample && (!(hi - lo <= kMaxHeightSpan) || std::fabs(lo) * frame.scale > kMaxCoordinate || std::fabs(hi) * frame.scale > kMaxCoordinate)) {
		return core::make_error(where + ": height range " + std::to_string(lo) + ".." + std::to_string(hi) + " is past 1e6 m");
	}
	const double ox = p_view.origin_xz[0], oz = p_view.origin_xz[1];
	const double sx = p_view.spacing[0], sz = p_view.spacing[1];
	const double extent = std::max({ std::fabs(ox), std::fabs(oz), std::fabs(ox + double(cells_x) * sx), std::fabs(oz + double(cells_z) * sz) });
	if (!(extent * frame.scale <= kMaxCoordinate)) {
		return core::make_error(where + ": the grid reaches past 1e6 m");
	}

	BF3_TRY(budget.charge(cells_x * cells_z));
	const bool fast = std::min(sx, sz) * frame.scale >= kFastStep;
	std::vector<std::array<uint32_t, 2>> marked;
	CellPair pair;
	for (size_t z = 0; z < cells_z; ++z) {
		for (size_t x = 0; x < cells_x; ++x) {
			const size_t a = x + z * w;
			if (fast) {

				const bool open = p_view.holes[a] != 0 || p_view.holes[a + 1] != 0 || p_view.holes[a + w] != 0 || p_view.holes[a + w + 1] != 0;
				if (!open) {
					continue;
				}
				if (jolt_keeps(mask, w, x, z, 0) || jolt_keeps(mask, w, x, z, 1)) {
					mask[a] = 1;
					marked.push_back({ uint32_t(x), uint32_t(z) });
				}
				continue;
			}
			soup_cell(p_view, frame.transform, x, z, pair);

			if ((jolt_keeps(mask, w, x, z, 1) && !pair.kept[0]) || (jolt_keeps(mask, w, x, z, 0) && !pair.kept[1])) {
				mask[a] = 1;
				marked.push_back({ uint32_t(x), uint32_t(z) });
			}
		}
	}
	info.holed = uint32_t(marked.size());

	std::vector<std::array<uint32_t, 2>> patch_cells;
	for (const std::array<uint32_t, 2> &m : marked) {
		for (uint32_t dz = 0; dz < 2; ++dz) {
			for (uint32_t dx = 0; dx < 2; ++dx) {
				if (m[0] < dx || m[1] < dz) {
					continue;
				}
				const uint32_t cx = m[0] - dx;
				const uint32_t cz = m[1] - dz;
				if (cx < cells_x && cz < cells_z) {
					patch_cells.push_back({ cz, cx });
				}
			}
		}
	}
	std::sort(patch_cells.begin(), patch_cells.end());
	patch_cells.erase(std::unique(patch_cells.begin(), patch_cells.end()), patch_cells.end());
	JPH::VertexList vertices;
	JPH::IndexedTriangleList triangles;
	Vec3 bmin{}, bmax{};
	bool any_bounds = false;
	for (const std::array<uint32_t, 2> &c : patch_cells) {
		soup_cell(p_view, frame.transform, c[1], c[0], pair);
		for (int k = 0; k < 2; ++k) {
			if (!pair.kept[k] || jolt_keeps(mask, w, c[1], c[0], k == 0 ? 1 : 0)) {
				continue;
			}
			const uint32_t first = uint32_t(vertices.size());
			for (const Vec3 &v : pair.v[k]) {
				vertices.push_back(JPH::Float3(v[0], v[1], v[2]));
				grow(bmin, bmax, v, any_bounds);
			}
			triangles.push_back(JPH::IndexedTriangle(first, first + 1, first + 2, 0, 0));
		}
	}
	if (triangles.size() > size_t(8) * marked.size()) {
		return core::make_error(where + ": " + std::to_string(triangles.size()) + " patch triangles for " + std::to_string(marked.size()) + " marked samples");
	}
	info.patch_triangles = uint32_t(triangles.size());
	const float cos_threshold = std::cos(p_options.active_edge_degrees * (3.14159265358979323846f / 180.0f));
	if (!triangles.empty()) {
		JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
		settings.SetEmbedded();
		settings.mPerTriangleUserData = true;
		settings.mActiveEdgeCosThresholdAngle = cos_threshold;
		if (!settings.mIndexedTriangles.empty()) {
			const JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError()) {
				return core::make_error(where + " patch: Jolt: " + std::string(result.GetError().c_str()));
			}
			out.patch = result.Get();
		}
	}

	bool field_has_triangle = false;
	for (size_t z = 0; z < cells_z && !field_has_triangle; ++z) {
		for (size_t x = 0; x < cells_x; ++x) {
			if (jolt_keeps(mask, w, x, z, 0) || jolt_keeps(mask, w, x, z, 1)) {
				field_has_triangle = true;
				break;
			}
		}
	}

	const size_t block = kStaticTerrainBlock;
	size_t n = std::max({ w, h, 2 * block });
	n = (n + block - 1) / block * block;
	out.position = p_view.translation;
	const JPH::Quat q = frame.rotation;
	out.rotation = { q.GetX(), q.GetY(), q.GetZ(), q.GetW() };
	const float s = float(frame.scale);
	if (field_has_triangle) {
		JPH::HeightFieldShapeSettings settings;
		settings.SetEmbedded();
		settings.mOffset = JPH::Vec3(s * p_view.origin_xz[0], 0.0f, s * p_view.origin_xz[1]);
		settings.mScale = JPH::Vec3(s * p_view.spacing[0], s, s * p_view.spacing[1]);
		settings.mSampleCount = uint32_t(n);
		settings.mBitsPerSample = kStaticTerrainBits;
		settings.mBlockSize = uint32_t(block);
		settings.mActiveEdgeCosThresholdAngle = cos_threshold;
		settings.mHeightSamples.resize(n * n, JPH::HeightFieldShapeConstants::cNoCollisionValue);
		for (size_t z = 0; z < h; ++z) {
			for (size_t x = 0; x < w; ++x) {
				const size_t i = x + z * w;
				if (mask[i] == 0) {
					settings.mHeightSamples[x + z * n] = p_view.heights[i];
				}
			}
		}
		const JPH::ShapeSettings::ShapeResult result = settings.Create();
		if (result.HasError()) {
			return core::make_error(where + " field: Jolt: " + std::string(result.GetError().c_str()));
		}
		out.field = result.Get();

		const JPH::HeightFieldShape &field = static_cast<const JPH::HeightFieldShape &>(*out.field);
		if (field.GetSampleCount() != n) {
			return core::make_error(where + " field: Jolt made " + std::to_string(field.GetSampleCount()) + " samples a side, expected " + std::to_string(n));
		}
		BF3_TRY(budget.charge(w * h));
		std::vector<float> band(n * block);
		double worst = 0.0;
		for (size_t y0 = 0; y0 < h; y0 += block) {
			field.GetHeights(0, uint32_t(y0), uint32_t(n), uint32_t(block), band.data(), intptr_t(n));
			for (size_t dy = 0; dy < block && y0 + dy < h; ++dy) {
				for (size_t x = 0; x < w; ++x) {
					const size_t i = x + (y0 + dy) * w;
					if (mask[i] != 0) {
						continue;
					}
					worst = std::max(worst, std::fabs(double(band[x + dy * n]) - double(s) * double(p_view.heights[i])));
				}
			}
		}
		info.max_error = float(worst);

		for (int k = 0; k < 8; ++k) {
			const float gx = p_view.origin_xz[0] + ((k & 1) != 0 ? float(cells_x) * p_view.spacing[0] : 0.0f);
			const float gy = float((k & 2) != 0 ? hi : lo);
			const float gz = p_view.origin_xz[1] + ((k & 4) != 0 ? float(cells_z) * p_view.spacing[1] : 0.0f);
			grow(bmin, bmax, frame.transform.transform_point({ gx, gy, gz }), any_bounds);
		}
	}
	info.min = bmin;
	info.max = bmax;
	return out;
}

core::Result<std::vector<StaticWorldShapes::Impl::Terrain>> cook_static_terrains(core::Cache *p_cache, std::span<const StaticTerrainView> p_views,
		const StaticWorldBuildOptions &p_options, bool *r_stored, uint64_t *r_bytes, size_t *r_hit) {
	if (p_views.size() > kMaxStaticTerrains) {
		return core::make_error("static world: " + std::to_string(p_views.size()) + " terrains exceed " + std::to_string(kMaxStaticTerrains));
	}
	core::Cache *cache = (p_cache != nullptr && p_cache->is_open()) ? p_cache : nullptr;
	std::vector<StaticWorldShapes::Impl::Terrain> out;
	out.reserve(p_views.size());
	bool stored = cache != nullptr;
	uint64_t bytes = 0;
	size_t hits = 0;
	for (const StaticTerrainView &view : p_views) {
		const core::Sha256Digest content = static_terrain_content(view);
		if (cache == nullptr) {
			core::Result<StaticWorldShapes::Impl::Terrain> cooked = cook_static_terrain(view, p_options, &content);
			if (!cooked) {
				return cooked.error();
			}
			out.push_back(std::move(*cooked));
			continue;
		}
		const std::optional<core::CacheKey> key = core::make_cache_key(static_terrain_key_inputs(content, p_options));
		if (!key) {
			return core::make_error(where_of(view) + ": no cache key");
		}
		std::optional<StaticWorldShapes::Impl::Terrain> terrain;
		const auto accept = [&](std::span<const uint8_t> p_blob) -> core::Status {
			core::Result<StaticWorldShapes::Impl::Terrain> decoded = decode_static_terrain(p_blob);
			if (!decoded) {
				return decoded.error();
			}
			if (decoded->info.content != content || decoded->info.width != view.width || decoded->info.height != view.height) {
				return core::make_error("jolt-terrain entry describes another terrain");
			}
			terrain = std::move(*decoded);
			return core::Status::success();
		};
		const auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
			core::Result<StaticWorldShapes::Impl::Terrain> cooked = cook_static_terrain(view, p_options, &content);
			if (!cooked) {
				return cooked.error();
			}
			terrain = std::move(*cooked);
			return encode_static_terrain(*terrain);
		};
		core::Result<core::CachedItem> item = core::cached_convert(cache, *key, accept, produce);
		if (!item) {
			return item.error();
		}
		if (!terrain) {
			return core::make_error(where_of(view) + ": the cache gave no terrain");
		}
		terrain->info.object = view.object;
		bytes += item->payload_bytes;
		hits += item->outcome == core::CacheOutcome::Hit ? 1 : 0;
		stored = stored && item->outcome != core::CacheOutcome::Uncached;
		out.push_back(std::move(*terrain));
	}
	if (r_stored != nullptr) {
		*r_stored = stored;
	}
	if (r_bytes != nullptr) {
		*r_bytes = bytes;
	}
	if (r_hit != nullptr) {
		*r_hit = hits;
	}
	return out;
}

std::vector<StaticBodySpec> static_body_specs(const StaticWorldShapes::Impl &p_world) {
	std::vector<StaticBodySpec> out;
	for (size_t t = 0; t < p_world.tiles.size(); ++t) {
		const StaticWorldShapes::Impl::Tile &tile = p_world.tiles[t];
		const JPH::Vec3 origin = static_tile_origin(tile.info.x, tile.info.z);
		for (size_t layer = 0; layer < kStaticLayerCount; ++layer) {
			if (tile.shapes[layer] == nullptr) {
				continue;
			}
			out.push_back({ tile.shapes[layer].GetPtr(), origin, JPH::Quat::sIdentity(), uint32_t(layer), uint64_t(t) * 4u + layer });
		}
	}
	for (size_t t = 0; t < p_world.terrains.size(); ++t) {
		const StaticWorldShapes::Impl::Terrain &terrain = p_world.terrains[t];
		const uint64_t user = uint64_t(t) * 4u + kStaticTerrainBodyLayer;
		if (terrain.field != nullptr) {
			const JPH::Quat q(terrain.rotation[0], terrain.rotation[1], terrain.rotation[2], terrain.rotation[3]);
			out.push_back({ terrain.field.GetPtr(), JPH::Vec3(terrain.position[0], terrain.position[1], terrain.position[2]), q,
					uint32_t(StaticLayer::Collision), user });
		}
		if (terrain.patch != nullptr) {
			out.push_back({ terrain.patch.GetPtr(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), uint32_t(StaticLayer::Collision), user });
		}
	}
	return out;
}

JPH::Vec3 static_face_normal(const StaticWorldShapes::Impl &p_world, uint64_t p_user, const JPH::Shape *p_shape, const JPH::SubShapeID &p_sub) {
	const uint64_t index = p_user >> 2;
	const uint32_t layer = uint32_t(p_user & 3u);
	if (layer < kStaticLayerCount) {
		if (index >= p_world.tiles.size() || p_world.tiles[index].shapes[layer] == nullptr) {
			return JPH::Vec3::sAxisY();
		}
		return p_world.tiles[index].shapes[layer]->GetSurfaceNormal(p_sub, JPH::Vec3::sZero());
	}
	if (index >= p_world.terrains.size() || p_shape == nullptr) {
		return JPH::Vec3::sAxisY();
	}
	const StaticWorldShapes::Impl::Terrain &terrain = p_world.terrains[index];
	if (p_shape == terrain.field.GetPtr()) {
		const JPH::Quat q(terrain.rotation[0], terrain.rotation[1], terrain.rotation[2], terrain.rotation[3]);
		return q * terrain.field->GetSurfaceNormal(p_sub, JPH::Vec3::sZero());
	}
	if (p_shape == terrain.patch.GetPtr()) {
		return terrain.patch->GetSurfaceNormal(p_sub, JPH::Vec3::sZero());
	}
	return JPH::Vec3::sAxisY();
}

}

#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::sim {

inline constexpr float kWaterCell = 16.0f;
inline constexpr size_t kMaxWaterCells = size_t(1) << 22;

struct WaterBodyMesh {
	std::vector<std::array<Vec3, 3>> triangles;
};

class WaterSurface {
public:
	static core::Result<std::shared_ptr<const WaterSurface>> build(std::span<const WaterBodyMesh> p_bodies);

	std::optional<float> height(float p_x, float p_z) const;

	size_t body_count() const { return bodies_.size(); }
	size_t triangle_count() const;
	Vec3 body_min(size_t p_body) const { return bodies_[p_body].min; }
	Vec3 body_max(size_t p_body) const { return bodies_[p_body].max; }
	uint64_t digest() const { return digest_; }

private:
	struct Body {
		Vec3 min{};
		Vec3 max{};
		size_t columns = 0;
		size_t rows = 0;
		std::vector<uint32_t> cell_start;
		std::vector<uint32_t> cell_items;
		std::vector<std::array<Vec3, 3>> triangles;
	};

	std::vector<Body> bodies_;
	uint64_t digest_ = 0;
};

}

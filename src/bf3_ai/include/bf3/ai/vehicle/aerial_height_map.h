#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::ai {

inline constexpr float kMissingGroundHeight = 9999.0f;

class AerialHeightSource {
public:
	virtual ~AerialHeightSource() = default;
	virtual float height(float p_x, float p_z) const = 0;
};

struct AerialBounds {
	float min_x = 0.0f;
	float min_z = 0.0f;
	float size_x = 0.0f;
	float size_z = 0.0f;
};

class AerialHeightMap {
public:
	float altitude(const Vec3 &p_position) const;
	float ground_height(const Vec3 &p_position) const;
	float ground_height(float p_x, float p_z) const;

	void create_height_map(const AerialBounds &p_bounds, const AerialHeightSource &p_source, int p_samples_per_axis);
	uint16_t compute_height_at(
			const AerialHeightSource &p_source, int p_cell_x, int p_cell_z, int p_samples_per_axis) const;

	std::vector<uint8_t> serialize() const;
	core::Status deserialize(std::span<const uint8_t> p_bytes);

	float cell_size() const { return cell_size_; }
	int width() const { return width_; }
	int height() const { return height_; }
	Vec2 origin() const { return origin_; }
	bool empty() const { return heights_.empty(); }

private:
	float cell_size_ = 16.0f;
	Vec2 origin_{};
	std::vector<uint16_t> heights_;
	int width_ = 0;
	int height_ = 0;
};

}

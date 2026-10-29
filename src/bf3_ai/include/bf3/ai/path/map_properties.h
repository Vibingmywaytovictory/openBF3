#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/ai/path/material.h"

namespace bf3::ai {

class MapProperties {
public:
	bool is_valid_ground_mesh(const Vec3 &p_normal, MaterialId p_material) const;
	void set_max_slope(float p_radians);
	float max_slope() const;
	void set_head_clearance(float p_clearance) { head_clearance_ = p_clearance; }
	float head_clearance() const { return head_clearance_; }
	void set_radius(float p_radius) { radius_ = p_radius; }
	float radius() const { return radius_; }
	void set_allowed_height_diff(float p_diff) { allowed_height_diff_ = p_diff; }
	float allowed_height_diff() const { return allowed_height_diff_; }

private:
	float max_slope_cos_ = 0.0f;
	float radius_ = 0.0f;
	float head_clearance_ = 2.0f;
	float allowed_height_diff_ = 2.5f;
};

}

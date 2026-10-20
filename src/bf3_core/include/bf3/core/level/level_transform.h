#pragma once

#include "bf3/core/res/res_math.h"

#include <array>

namespace bf3::core::level {

using Vec3 = std::array<float, 3>;
using Rows = std::array<Vec3, 4>;

struct LevelTransform {
	Vec3 right{ 1.0f, 0.0f, 0.0f };
	Vec3 up{ 0.0f, 1.0f, 0.0f };
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
	Vec3 trans{ 0.0f, 0.0f, 0.0f };
	float scale = 1.0f;

	static LevelTransform identity() { return LevelTransform(); }
	static LevelTransform from_rows(const Rows &p_rows);

	static LevelTransform from_res(const res::Transform &p_transform);

	Rows rows() const { return { right, up, forward, trans }; }

	Vec3 rotate(const Vec3 &p_v) const;

	Vec3 transform_vector(const Vec3 &p_v) const;
	Vec3 transform_point(const Vec3 &p_p) const;

	LevelTransform in_parent(const LevelTransform &p_parent) const;
	float determinant() const;

	Vec3 axis_scales() const;

	bool is_uniform(float p_tolerance) const;

	LevelTransform normalized() const;

	bool operator==(const LevelTransform &) const = default;
};

}

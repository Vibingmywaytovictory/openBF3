#pragma once
// A Frostbite LinearTransform with a uniform scale, in Frostbite space (metres, Y up): a local point
// p lands at `trans + scale * (p.x*right + p.y*up + p.z*forward)`. Rows right, up, forward, trans,
// as the EBX reader gives a LinearTransform. Child-in-parent is the row-vector product
// `child (x) parent` (handoff sections/03 §2.2). Ported from IW4L level_bf3 (transform.rs),
// Apache-2.0; the operation order is kept so results are bit-identical to it.

#include "bf3/core/res/res_math.h"

#include <array>

namespace bf3::core::level {

using Vec3 = std::array<float, 3>;
using Rows = std::array<Vec3, 4>; // right, up, forward, trans

struct LevelTransform {
	Vec3 right{ 1.0f, 0.0f, 0.0f };
	Vec3 up{ 0.0f, 1.0f, 0.0f };
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
	Vec3 trans{ 0.0f, 0.0f, 0.0f };
	float scale = 1.0f;

	static LevelTransform identity() { return LevelTransform(); }
	static LevelTransform from_rows(const Rows &p_rows);
	// A Havok transform (rotation axes + origin), scale 1.
	static LevelTransform from_res(const res::Transform &p_transform);

	Rows rows() const { return { right, up, forward, trans }; }
	// The image of a local direction through the axes alone, without `scale`.
	Vec3 rotate(const Vec3 &p_v) const;
	// The image of a local direction, scale included.
	Vec3 transform_vector(const Vec3 &p_v) const;
	Vec3 transform_point(const Vec3 &p_p) const;
	// This transform read as local to p_parent, placed in p_parent's space (`self (x) parent`).
	LevelTransform in_parent(const LevelTransform &p_parent) const;
	float determinant() const;
	// How much the transform stretches local X, Y and Z, `scale` included (level data scales
	// through the axes as well as through `scale`).
	Vec3 axis_scales() const;
	// Whether every local axis stretches alike, within p_tolerance relative.
	bool is_uniform(float p_tolerance) const;
	// Unit axes with their stretch moved into `scale` (exact when uniform, else the mean stretch).
	LevelTransform normalized() const;

	bool operator==(const LevelTransform &) const = default;
};

} // namespace bf3::core::level

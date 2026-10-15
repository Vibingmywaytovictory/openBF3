#pragma once
// A water surface of a level (spike S1, docs/design/s1.md §5.2 "Deep water"; read by mode_areas.h
// read_water_planes, used by the soup's cover water rule, SoupOptions::cover_water). Godot-free, no EBX: a small
// header the static world and nav options can include.

#include <array>
#include <vector>

namespace bf3::core::level {

// A closed XZ polygon at height y (a LakeData shape).
struct WaterPlane {
	float y = 0.0f; // the highest point of the shape (retail: every point at the same height)
	std::vector<std::array<float, 2>> polygon; // world XZ
	bool operator==(const WaterPlane &) const = default;
};

// Whether (x, z) lies inside the plane's polygon (even-odd rule).
bool water_plane_covers(const WaterPlane &p_plane, float p_x, float p_z);

// The water rule's depth: a cover tile under water keeps its terrain only when one of its samples lies above the
// plane minus one soldier height (the stand height, 1.8 m).
inline constexpr float kCoverWaterDepth = 1.8f;

} // namespace bf3::core::level

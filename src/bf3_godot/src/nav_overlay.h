#pragma once

#include "bf3/core/math.h"
#include "bf3/sim/nav.h"

#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace bf3::godot_side {

struct NavOverlayArrays {
	std::vector<core::Vec3> triangle_positions;
	std::vector<std::array<float, 4>> triangle_colors;
	std::vector<core::Vec3> line_positions;
	size_t triangles_main = 0;
	size_t triangles_other = 0;
	size_t links = 0;
};

NavOverlayArrays prepare_nav_overlay(const sim::NavDebugMesh &p_mesh, uint32_t p_main);

class NavOverlay {
public:
	NavOverlay() = default;
	NavOverlay(const NavOverlay &) = delete;
	NavOverlay &operator=(const NavOverlay &) = delete;
	~NavOverlay() = default;

	bool upload(const NavOverlayArrays &p_arrays, const godot::RID &p_scenario, bool p_visible);
	void set_visible(bool p_visible);
	bool visible() const { return visible_; }
	bool uploaded() const { return instance_.is_valid(); }
	void free_all();

private:
	godot::RID shader_;
	godot::RID material_;
	godot::RID mesh_;
	godot::RID instance_;
	bool visible_ = false;
};

}

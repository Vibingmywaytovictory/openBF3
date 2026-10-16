#pragma once
// S1 (docs/design/s1.md §12): the navmesh overlay of `--bots=N --nav-overlay` (F7 toggles), one RenderingServer mesh
// and instance from the sim::NavDebugMesh the loader prepared.
//
// prepare_nav_overlay runs on the loader thread (no Godot API): vertex positions and colours for the triangles and the
// drop links. Colours: the bot level's main component green (crouch area blue), every other component red with a hue
// per id, drop links yellow lines. NavOverlay::upload (main thread) makes one mesh with two surfaces (triangles,
// lines), an unshaded, vertex-coloured, double-sided, depth-tested material, and one instance without shadows; the
// debug mesh is already lifted 5 cm above the surface.

#include "bf3/sim/nav.h"

#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace bf3::godot_side {

struct NavOverlayArrays {
	std::vector<std::array<float, 3>> triangle_positions; // 3 per triangle
	std::vector<std::array<float, 4>> triangle_colors;
	std::vector<std::array<float, 3>> line_positions; // 2 per drop link
	size_t triangles_main = 0;
	size_t triangles_other = 0;
	size_t links = 0;
};

// Loader thread. p_main: BotLevel::main_component.
NavOverlayArrays prepare_nav_overlay(const sim::NavDebugMesh &p_mesh, uint32_t p_main);

class NavOverlay {
public:
	NavOverlay() = default;
	NavOverlay(const NavOverlay &) = delete;
	NavOverlay &operator=(const NavOverlay &) = delete;
	~NavOverlay() = default; // free_all() must run on the main thread before the RenderingServer goes

	// Main thread. False when there is nothing to draw.
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

} // namespace bf3::godot_side

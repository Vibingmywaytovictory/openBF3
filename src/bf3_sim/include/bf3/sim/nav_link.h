#pragma once
// Recast/Detour link check for S0 (navigation proper arrives with the bots milestone, plan §3.6).

namespace bf3::sim {

struct NavLinkResult {
	bool ok = false;
	int grid_w = 0;
	int grid_h = 0;
	int spans = 0; // solid spans rasterised from a 10 x 10 m floor
	bool detour_alloc = false; // dtAllocNavMesh / dtFreeNavMesh round trip
};

NavLinkResult nav_link_check();

} // namespace bf3::sim

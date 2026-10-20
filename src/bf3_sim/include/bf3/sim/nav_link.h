#pragma once

namespace bf3::sim {

struct NavLinkResult {
	bool ok = false;
	int grid_w = 0;
	int grid_h = 0;
	int spans = 0;
	bool detour_alloc = false;
};

NavLinkResult nav_link_check();

}

#include "bf3/sim/nav_link.h"

#include <DetourNavMesh.h>
#include <Recast.h>

namespace bf3::sim {

NavLinkResult nav_link_check() {
	NavLinkResult r;
	rcContext ctx(false);
	const float bmin[3] = { -5.0f, -1.0f, -5.0f };
	const float bmax[3] = { 5.0f, 3.0f, 5.0f };
	const float cs = 0.25f;
	const float ch = 0.25f;
	rcCalcGridSize(bmin, bmax, cs, &r.grid_w, &r.grid_h);
	rcHeightfield *hf = rcAllocHeightfield();
	if (hf != nullptr && rcCreateHeightfield(&ctx, *hf, r.grid_w, r.grid_h, bmin, bmax, cs, ch)) {
		// Two triangles of a flat floor at y = 0, wound so the normal points up for Recast.
		const float verts[] = { -5, 0, -5, -5, 0, 5, 5, 0, 5, 5, 0, -5 };
		const int tris[] = { 0, 1, 2, 0, 2, 3 };
		unsigned char areas[2] = { 0, 0 };
		rcMarkWalkableTriangles(&ctx, 45.0f, verts, 4, tris, 2, areas);
		if (rcRasterizeTriangles(&ctx, verts, 4, tris, areas, 2, *hf, 1)) {
			for (int i = 0; i < r.grid_w * r.grid_h; ++i) {
				for (const rcSpan *s = hf->spans[i]; s != nullptr; s = s->next) {
					r.spans++;
				}
			}
		}
	}
	rcFreeHeightField(hf);
	dtNavMesh *nav = dtAllocNavMesh();
	r.detour_alloc = nav != nullptr;
	dtFreeNavMesh(nav);
	r.ok = r.spans >= (r.grid_w * r.grid_h * 9) / 10 && r.detour_alloc;
	return r;
}

} // namespace bf3::sim

#pragma once

#include "../server_ghosts.h"
#include "bf3/sim/sim_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf3::host {

inline constexpr uint8_t kRestingVehicleSnapshots = 15;

struct VehicleScopeCandidate {
	uint8_t team = 0;
	sim::Vec3 position{};
	bool occupied_by_viewer = false;
	bool resting = false;
	uint8_t generation = 0;
};

struct VehicleScopeStats {
	uint64_t sent = 0;
	uint64_t held = 0;
	uint64_t out_of_scope = 0;
};

class VehicleScope {
public:
	void configure(size_t p_viewers, size_t p_vehicles, bool p_scoped);
	void forget(size_t p_viewer);
	bool due(size_t p_viewer, const GhostViewer &p_who, size_t p_vehicle, const VehicleScopeCandidate &p_vehicle_now);
	const VehicleScopeStats &stats() const { return stats_; }

private:
	struct Seen {
		bool known = false;
		uint8_t generation = 0;
		float accumulator = 0;
	};

	size_t vehicles_ = 0;
	bool scoped_ = true;
	std::vector<Seen> seen_;
	VehicleScopeStats stats_;
};

}

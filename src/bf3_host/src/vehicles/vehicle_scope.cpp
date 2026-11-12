#include "vehicle_scope.h"

#include <algorithm>
#include <optional>

namespace bf3::host {

void VehicleScope::configure(size_t p_viewers, size_t p_vehicles, bool p_scoped) {
	vehicles_ = p_vehicles;
	scoped_ = p_scoped;
	seen_.assign(p_viewers * p_vehicles, Seen());
}

void VehicleScope::forget(size_t p_viewer) {
	if (vehicles_ == 0 || (p_viewer + 1u) * vehicles_ > seen_.size()) {
		return;
	}
	std::fill_n(seen_.begin() + ptrdiff_t(p_viewer * vehicles_), vehicles_, Seen());
}

bool VehicleScope::due(
		size_t p_viewer, const GhostViewer &p_who, size_t p_vehicle, const VehicleScopeCandidate &p_vehicle_now) {
	if (p_vehicle >= vehicles_ || (p_viewer + 1u) * vehicles_ > seen_.size()) {
		return false;
	}
	Seen &s = seen_[p_viewer * vehicles_ + p_vehicle];
	if (!scoped_ || p_vehicle_now.occupied_by_viewer) {
		s = Seen{ true, p_vehicle_now.generation, 0.0f };
		++stats_.sent;
		return true;
	}
	std::optional<GhostTier> tier = kUnscopedTier;
	if (p_who.spawned) {
		GhostCandidate c;
		c.team = p_vehicle_now.team;
		c.position = p_vehicle_now.position;
		tier = ghost_tier(p_who, c, false);
	} else {
		tier = GhostTier{ 0.0f, kRestingVehicleSnapshots, kRestingVehicleSnapshots };
	}
	if (!tier) {
		s = Seen();
		++stats_.out_of_scope;
		return false;
	}
	const uint8_t period = p_vehicle_now.resting ? std::max(tier->target, kRestingVehicleSnapshots) : tier->target;
	if (!s.known || s.generation != p_vehicle_now.generation) {
		s = Seen{ true, p_vehicle_now.generation, 0.0f };
		++stats_.sent;
		return true;
	}
	s.accumulator += 1.0f / float(std::max<uint8_t>(period, 1));
	if (s.accumulator >= 1.0f - 1e-4f) {
		s.accumulator = 0.0f;
		++stats_.sent;
		return true;
	}
	++stats_.held;
	return false;
}

}

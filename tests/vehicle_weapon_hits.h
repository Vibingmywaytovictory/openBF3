#pragma once

#include "bf3/host/gunplay.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/weapons/damage.h"
#include "vehicles/vehicle_gunnery.h"

#include <atomic>
#include <cstdint>

namespace bf3::test {

// WHY: VehicleGunnery's own damage counter runs before the server drops team, dead-victim and spawn-protected damage,
// so a vehicle weapon's hits on soldiers are counted here, where the server reports what it applied.
class VehicleWeaponHits final : public host::GunplayObserver {
public:
	void damage(
			uint32_t p_tick, const sim::weapons::DamageEvent &p_event, bool p_applied, float p_health_after) override {
		(void)p_tick, (void)p_health_after;
		if (p_applied && p_event.target < net::kMaxSlots && p_event.attacker_slot != p_event.target &&
				p_event.shot_index >= host::kVehicleShotIndexBase && p_event.shot_index < host::kWreckShotIndexBase) {
			hits_.fetch_add(1, std::memory_order_relaxed);
		}
	}

	uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }

private:
	std::atomic<uint64_t> hits_{ 0 };
};

}

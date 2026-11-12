#pragma once

#include "bf3/host/client_session.h"
#include "bf3/host/presented_vehicle.h"
#include "bf3/net/protocol.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

inline constexpr sim::Tick kVehicleStaleTicks = 120;

class ClientVehicles {
public:
	void receive(std::span<const uint8_t> p_bytes);
	void publish(PresentationFrame &r_frame) const;
	void stats(ClientStats &r_stats) const;

private:
	struct Tracked {
		PresentedVehicle newest;
		std::vector<PresentedVehicleSample> history;
	};

	std::vector<Tracked> vehicles_;
	sim::Tick newest_ = 0;
	uint64_t messages_ = 0;
	uint64_t bad_ = 0;
};

}

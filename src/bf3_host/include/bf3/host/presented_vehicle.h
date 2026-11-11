#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicle_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bf3::host {

inline constexpr size_t kMaxPresentedVehicles = 32;

struct PresentedVehicle {
	uint16_t spawner = 0;
	uint8_t generation = 0;
	uint8_t team = 0;
	uint8_t seat_count = 0;
	std::array<uint8_t, net::kMaxVehicleSeats> occupants{};
	sim::Tick tick = 0;
	sim::Vec3 com_position{};
	std::array<float, 4> rotation{ 0, 0, 0, 1 };
	sim::Vec3 linear_velocity{};
	uint8_t wheel_count = 0;
	std::array<float, sim::kMaxVehicleWheels> wheel_angle{};
	std::array<float, sim::kMaxVehicleWheels> wheel_steer{};
	std::array<float, sim::kMaxVehicleWheels> suspension_length{};
	float engine_rpm = 0;
	int8_t gear = 0;
};
static_assert(std::is_trivially_copyable_v<PresentedVehicle>);

PresentedVehicle present_vehicle(const net::VehicleWire &p_wire, sim::Tick p_tick);

}

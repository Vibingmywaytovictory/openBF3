#pragma once

#include "bf3/sim/command.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/weapons/weapon_input.h"

#include <cstdint>
#include <optional>

namespace bf3::sim::vehicles {

namespace seat_buttons {
inline constexpr uint32_t kSeatShift = 12;
inline constexpr uint32_t kSeatMask = 7u << kSeatShift;
inline constexpr uint32_t kHandbrake = 1u << 15;
inline constexpr uint32_t kAll = kSeatMask | kHandbrake;
}

inline constexpr uint8_t kSeatKeys = 6;

static_assert((seat_buttons::kAll & ~buttons::kWireMask) == 0, "vehicle buttons must fit the 24 wire bits");
static_assert(
		(seat_buttons::kAll & (weapons::weapon_buttons::kAll | buttons::kJump | buttons::kSprint | buttons::kUse)) == 0,
		"vehicle buttons must not overlap the soldier's buttons");
static_assert((seat_buttons::kAll & buttons::kEdgeMask) == 0, "vehicle fields are levels, never edges");
static_assert(kSeatKeys < (seat_buttons::kSeatMask >> seat_buttons::kSeatShift), "seat field too small");

constexpr uint32_t with_seat_request(uint32_t p_buttons, std::optional<uint8_t> p_seat) {
	const uint32_t b = p_buttons & ~seat_buttons::kSeatMask;
	if (!p_seat || *p_seat >= kSeatKeys) {
		return b;
	}
	return b | (uint32_t(*p_seat + 1u) << seat_buttons::kSeatShift);
}

constexpr std::optional<uint8_t> seat_request(uint32_t p_buttons) {
	const uint32_t v = (p_buttons & seat_buttons::kSeatMask) >> seat_buttons::kSeatShift;
	if (v == 0 || v > kSeatKeys) {
		return std::nullopt;
	}
	return uint8_t(v - 1u);
}

constexpr bool handbrake_held(uint32_t p_buttons) {
	return (p_buttons & seat_buttons::kHandbrake) != 0;
}

constexpr VehicleCommand driver_command(const Command &p_command) {
	VehicleCommand c;
	c.tick = p_command.tick;
	c.sequence = p_command.sequence;
	c.throttle = p_command.move_forward;
	c.brake = 0;
	c.steer = p_command.move_right;
	c.buttons = handbrake_held(p_command.buttons) ? vehicle_buttons::kHandbrake : uint8_t(0);
	c.flags = vehicle_flags::kDriven;
	return c;
}
}

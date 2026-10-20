#pragma once

#include "bf3/sim/command.h"

#include <cstdint>

namespace bf3::sim::weapons {

inline constexpr uint8_t kMaxWeaponSlots = 8;

namespace weapon_buttons {
inline constexpr uint32_t kFire = buttons::kFire;
inline constexpr uint32_t kZoom = buttons::kZoom;
inline constexpr uint32_t kReload = buttons::kReload;
inline constexpr uint32_t kFireModeShift = 6;
inline constexpr uint32_t kFireModeMask = 3u << kFireModeShift;
inline constexpr uint32_t kSlotShift = 8;
inline constexpr uint32_t kSlotMask = 0xFu << kSlotShift;
inline constexpr uint32_t kAll = kFire | kZoom | kReload | kFireModeMask | kSlotMask;
}

static_assert((weapon_buttons::kAll & ~buttons::kWireMask) == 0, "weapon buttons must fit the 24 wire bits");
static_assert((weapon_buttons::kAll & (buttons::kJump | buttons::kSprint | buttons::kUse)) == 0,
		"weapon buttons must not overlap the motor's buttons");
static_assert((weapon_buttons::kAll & buttons::kEdgeMask) == 0, "weapon fields are levels, never edges");
static_assert(kMaxWeaponSlots < (weapon_buttons::kSlotMask >> weapon_buttons::kSlotShift), "slot field too small");

struct WeaponInput {
	bool fire = false;
	bool zoom = false;
	bool reload = false;
	uint8_t fire_mode_counter = 0;
	uint8_t select_slot = 0xFF;

	constexpr bool operator==(const WeaponInput &) const = default;
};

constexpr WeaponInput weapon_input(const Command &p_command) {
	WeaponInput in;
	in.fire = (p_command.buttons & weapon_buttons::kFire) != 0;
	in.zoom = (p_command.buttons & weapon_buttons::kZoom) != 0;
	in.reload = (p_command.buttons & weapon_buttons::kReload) != 0;
	in.fire_mode_counter = uint8_t((p_command.buttons & weapon_buttons::kFireModeMask) >> weapon_buttons::kFireModeShift);
	const uint32_t slot = (p_command.buttons & weapon_buttons::kSlotMask) >> weapon_buttons::kSlotShift;
	in.select_slot = (slot == 0 || slot > kMaxWeaponSlots) ? uint8_t(0xFF) : uint8_t(slot - 1);
	return in;
}

constexpr uint8_t fire_mode_steps(uint8_t p_old, uint8_t p_new) {
	return uint8_t((p_new - p_old) & 3u);
}

constexpr uint32_t with_weapon_input(uint32_t p_buttons, const WeaponInput &p_input) {
	uint32_t b = p_buttons & ~weapon_buttons::kAll;
	b |= p_input.fire ? weapon_buttons::kFire : 0u;
	b |= p_input.zoom ? weapon_buttons::kZoom : 0u;
	b |= p_input.reload ? weapon_buttons::kReload : 0u;
	b |= (uint32_t(p_input.fire_mode_counter) & 3u) << weapon_buttons::kFireModeShift;
	if (p_input.select_slot < kMaxWeaponSlots) {
		b |= (uint32_t(p_input.select_slot) + 1u) << weapon_buttons::kSlotShift;
	}
	return b;
}

}

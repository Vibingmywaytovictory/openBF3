#pragma once
// The weapon part of a Command (docs/design/m2a-weapons.md §5.2). command.h reserved kFire, kZoom and kReload
// for M2a; this header adds the fire-mode and weapon-slot fields in the button bits command.h leaves free, within
// the 24 bits bf3_net sends (buttons::kWireMask).
//
// Every weapon field is a LEVEL, never an edge: fire / zoom / reload are "held", the fire mode is a 2-bit toggle
// counter (the weapon sim cycles once per change of the counter) and the slot field names the slot the player
// wants (0 = keep the current one). A level survives the server's repeated_command (which clears only
// buttons::kEdgeMask) and a lost command packet without doing anything twice: a repeated "switch to slot 2" is
// idempotent and a repeated counter value is no change. Single fire, bursts and the bolt's "until fire release"
// read the fire level's transitions against the previous command, which the weapon state keeps
// (WeaponSlotState::flags kTriggerDown), so they need no edge bit either.

#include "bf3/sim/command.h"

#include <cstdint>

namespace bf3::sim::weapons {

inline constexpr uint8_t kMaxWeaponSlots = 8; // a kit's slots (bf3_core kit_def.h: 8 per kit)

namespace weapon_buttons {
inline constexpr uint32_t kFire = buttons::kFire; // held
inline constexpr uint32_t kZoom = buttons::kZoom; // held (aim down sights)
inline constexpr uint32_t kReload = buttons::kReload; // held = asking for a reload
inline constexpr uint32_t kFireModeShift = 6;
inline constexpr uint32_t kFireModeMask = 3u << kFireModeShift; // 2-bit toggle counter
inline constexpr uint32_t kSlotShift = 8;
inline constexpr uint32_t kSlotMask = 0xFu << kSlotShift; // 0 = keep, 1..kMaxWeaponSlots = select slot - 1
inline constexpr uint32_t kAll = kFire | kZoom | kReload | kFireModeMask | kSlotMask;
} // namespace weapon_buttons

static_assert((weapon_buttons::kAll & ~buttons::kWireMask) == 0, "weapon buttons must fit the 24 wire bits");
static_assert((weapon_buttons::kAll & (buttons::kJump | buttons::kSprint | buttons::kUse)) == 0,
		"weapon buttons must not overlap the motor's buttons");
static_assert((weapon_buttons::kAll & buttons::kEdgeMask) == 0, "weapon fields are levels, never edges");
static_assert(kMaxWeaponSlots < (weapon_buttons::kSlotMask >> weapon_buttons::kSlotShift), "slot field too small");

struct WeaponInput {
	bool fire = false;
	bool zoom = false;
	bool reload = false;
	uint8_t fire_mode_counter = 0; // 0..3
	uint8_t select_slot = 0xFF; // 0..kMaxWeaponSlots-1, or 0xFF = keep the current slot

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

// Writes the weapon fields of p_input into p_buttons (the input mapper and the bots build commands with this).
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

} // namespace bf3::sim::weapons

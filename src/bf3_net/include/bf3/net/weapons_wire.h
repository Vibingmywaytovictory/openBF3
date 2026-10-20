#pragma once

#include "bf3/core/result.h"
#include "bf3/net/bit_stream.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <array>
#include <climits>
#include <cstdint>
#include <type_traits>

namespace bf3::net {

inline constexpr int32_t kWeaponsWireMaxOffset = int32_t(35 * sim::weapons::kTimeUnitsPerSecond);

inline constexpr int32_t kWeaponsWireTimeZero = INT32_MIN;

struct WeaponSlotWire {
	uint16_t def = sim::weapons::kNoWeapon;
	uint8_t mode = 0;
	uint8_t mode_counter = 0;
	int32_t magazine = 0;
	int32_t reserve = 0;

	bool operator==(const WeaponSlotWire &) const = default;
};

struct WeaponTimersWire {
	uint8_t slot = 0xFF;
	sim::weapons::WeaponPhase phase = sim::weapons::WeaponPhase::None;
	uint8_t burst_left = 0;
	uint8_t flags = 0;
	int32_t phase_start = 0;
	int32_t phase_end = 0;
	int32_t next_shot_time = 0;
	int32_t spread_time = 0;
	int32_t hold_start = 0;
	float spread = 0;
	sim::weapons::RecoilOffset recoil{};

	bool operator==(const WeaponTimersWire &) const = default;
};

struct WeaponsWire {
	uint32_t next_shot_index = 0;
	uint8_t active = 0;
	uint8_t pending = 0xFF;
	uint32_t events = 0;
	int32_t sprint_recover_until = 0;
	std::array<WeaponSlotWire, sim::weapons::kMaxWeaponSlots> slots{};
	std::array<WeaponTimersWire, 2> timers{};

	std::array<WeaponTimersWire, sim::weapons::kMaxWeaponSlots> idle{};

	bool operator==(const WeaponsWire &) const = default;
};
static_assert(std::is_trivially_copyable_v<WeaponsWire>);

WeaponsWire weapons_wire_view(const sim::weapons::SoldierWeaponsState &p_state, sim::Tick p_server_tick);

core::Result<sim::weapons::SoldierWeaponsState> weapons_from_wire(const WeaponsWire &p_wire, sim::Tick p_server_tick, uint32_t p_player_slot,
		size_t p_def_count);

void write_weapons_wire(BitWriter &r_writer, const WeaponsWire &p_wire);
core::Status read_weapons_wire(BitReader &r_reader, WeaponsWire &r_wire);

}

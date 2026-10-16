#pragma once
// The own soldier's weapons on the wire (docs/design/m2a.md §7.3, §8.2; owner nt): a compact view of
// sim::weapons::SoldierWeaponsState (~600 B in memory) at full precision, ~110 B in the Snapshot's own block. It is
// what both ends compare: the client checks weapons_wire_view(predicted at server_tick) against the decoded block
// and, on a mismatch, rebases its weapons from the wire view and replays (M1's rule; 0 expected in loopback, E15).
//
// Content: next_shot_index, active, pending, events, sprint_recover_until; per non-empty slot (def, magazine,
// reserve, mode, mode_counter); for the active and the pending slot every timer as an i32 offset from the snapshot
// tick's start (sim::weapons::tick_start(server_tick)), saturated at +-kWeaponsWireMaxOffset (35 s), plus spread,
// spread time, recoil, hold start, phase, burst and flags. weapons_from_wire rebuilds a state that steps exactly as
// the original (m2a.contract / net.proto round-trip every state of wp's W7 600-tick run and step both; nt's rows).
// A slot that is neither active nor pending keeps only what the view carries: its timers are idle by construction
// (a switch cancels them), and its spread and recoil have recovered by the time it deploys again [to be checked by
// nt's W7 round trip; nt widens the view additively if that check needs more].

#include "bf3/core/result.h"
#include "bf3/net/bit_stream.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace bf3::net {

inline constexpr int32_t kWeaponsWireMaxOffset = int32_t(35 * sim::weapons::kTimeUnitsPerSecond); // 2.1e9 units

// One slot: ammunition and mode for every non-empty slot.
struct WeaponSlotWire {
	uint16_t def = sim::weapons::kNoWeapon; // kNoWeapon = empty (nothing else is sent for it)
	uint8_t mode = 0;
	uint8_t mode_counter = 0;
	int32_t magazine = 0;
	int32_t reserve = 0;

	bool operator==(const WeaponSlotWire &) const = default;
};

// The timed state of the active or the pending slot; times are offsets from the snapshot tick's start.
struct WeaponTimersWire {
	uint8_t slot = 0xFF; // which slot this is (0xFF = none)
	sim::weapons::WeaponPhase phase = sim::weapons::WeaponPhase::None;
	uint8_t burst_left = 0;
	uint8_t flags = 0; // sim::weapons::slot_flags::
	int32_t phase_start = 0;
	int32_t phase_end = 0;
	int32_t next_shot_time = 0;
	int32_t spread_time = 0;
	int32_t hold_start = 0;
	float spread = 0; // f32 bit pattern on the wire
	sim::weapons::RecoilOffset recoil{};

	bool operator==(const WeaponTimersWire &) const = default;
};

struct WeaponsWire {
	uint32_t next_shot_index = 0;
	uint8_t active = 0;
	uint8_t pending = 0xFF;
	uint32_t events = 0; // weapon_events:: of the snapshot's tick
	int32_t sprint_recover_until = 0; // offset, as the timers
	std::array<WeaponSlotWire, sim::weapons::kMaxWeaponSlots> slots{};
	std::array<WeaponTimersWire, 2> timers{}; // [0] the active slot, [1] the pending slot (slot 0xFF when none)

	bool operator==(const WeaponsWire &) const = default;
};
static_assert(std::is_trivially_copyable_v<WeaponsWire>);

// The view of a state after server tick p_server_tick (offsets from tick_start(p_server_tick)).
WeaponsWire weapons_wire_view(const sim::weapons::SoldierWeaponsState &p_state, sim::Tick p_server_tick);
// The state a view describes (tick = p_server_tick, player_slot = p_player_slot). Fails on a slot index, def or
// phase out of range (p_def_count = the table's def count).
core::Result<sim::weapons::SoldierWeaponsState> weapons_from_wire(const WeaponsWire &p_wire, sim::Tick p_server_tick, uint32_t p_player_slot,
		size_t p_def_count);
// Bit-packed body inside the Snapshot's own block (protocol.cpp calls these). The reader bounds every field.
void write_weapons_wire(BitWriter &r_writer, const WeaponsWire &p_wire);
core::Status read_weapons_wire(BitReader &r_reader, WeaponsWire &r_wire);

} // namespace bf3::net

// The own soldier's weapons on the wire (weapons_wire.h; docs/design/m2a.md §7.3, §8.2; owner nt).
//
// Wire body (inside the Snapshot's own block, after the own state):
//   u32 next_shot_index | u8 active | u8 pending | u16 events | time sprint_recover_until
//   per slot 0..7: 1 bit non-empty;
//     empty:     u2 mode_counter
//     non-empty: varint def | u2 mode | u2 mode_counter | zigzag varint magazine | zigzag varint reserve
//                | u3 phase | u6 burst_left | u7 flags | time phase_start | time phase_end | time next_shot_time
//                | f32 spread | time spread_time | recoil | time hold_start
//   time   = 1 bit "absolute 0", else i32 offset from tick_start(server tick), |offset| <= kWeaponsWireMaxOffset
//   recoil = 1 bit "both +0.0f", else f32 pitch, f32 yaw
// A typical M2a kit (4 non-empty slots, the active one firing) is about 95 bytes. Every float travels as its bit
// pattern; the reader refuses a non-finite one, an offset out of range, a varint beyond int32 and an index out of
// range, so a hostile datagram fails instead of building a state the sim was never in.
#include "bf3/net/weapons_wire.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace bf3::net {

namespace sw = sim::weapons;

namespace {

// Field widths on the wire; the view masks to them so read(write(view)) == view.
constexpr unsigned kModeBits = 2; // WeaponSimDef::modes has 4 entries
constexpr unsigned kCounterBits = 2; // the fire-mode toggle counter (weapon_input.h)
constexpr unsigned kBurstBits = 6; // burst_left <= kMaxBulletsPerBurst (32)
constexpr unsigned kFlagBits = 7; // slot_flags:: bits 0..6
constexpr unsigned kEventBits = 16; // weapon_events:: bits 0..15
constexpr unsigned kPhaseBits = 3; // WeaponPhase 0..7
static_assert(sw::kMaxBulletsPerBurst < (1 << kBurstBits));
static_assert(sw::slot_flags::kReleasePending == (1u << (kFlagBits - 1)), "a new slot flag needs a wider wire field");
static_assert(sw::weapon_events::kFireBlocked == (1u << (kEventBits - 1)), "a new weapon event needs a wider wire field");
static_assert(uint8_t(sw::WeaponPhase::Releasing) == (1u << kPhaseBits) - 1u, "a new weapon phase needs a wider wire field");
static_assert(sw::kMaxWeaponSlots == 8);

constexpr uint32_t mask(unsigned p_bits) {
	return (1u << p_bits) - 1u;
}

int32_t time_offset(sw::SimTime p_time, sw::SimTime p_base) {
	if (p_time == 0) {
		return kWeaponsWireTimeZero;
	}
	const sw::SimTime d = p_time - p_base;
	return int32_t(std::clamp<sw::SimTime>(d, -sw::SimTime(kWeaponsWireMaxOffset), sw::SimTime(kWeaponsWireMaxOffset)));
}

sw::SimTime time_from(int32_t p_offset, sw::SimTime p_base) {
	return p_offset == kWeaponsWireTimeZero ? 0 : p_base + p_offset;
}

WeaponTimersWire timers_of(const sw::WeaponSlotState &p_s, uint8_t p_slot, sw::SimTime p_base) {
	WeaponTimersWire t;
	t.slot = p_slot;
	t.phase = p_s.phase;
	t.burst_left = uint8_t(p_s.burst_left & mask(kBurstBits));
	t.flags = uint8_t(p_s.flags & mask(kFlagBits));
	t.phase_start = time_offset(p_s.phase_start, p_base);
	t.phase_end = time_offset(p_s.phase_end, p_base);
	t.next_shot_time = time_offset(p_s.next_shot_time, p_base);
	t.spread_time = time_offset(p_s.spread_time, p_base);
	t.hold_start = time_offset(p_s.hold_start, p_base);
	t.spread = p_s.spread;
	t.recoil = p_s.recoil;
	return t;
}

void write_time(BitWriter &r_w, int32_t p_offset) {
	r_w.write_bool(p_offset == kWeaponsWireTimeZero);
	if (p_offset != kWeaponsWireTimeZero) {
		r_w.write_signed(std::clamp(p_offset, -kWeaponsWireMaxOffset, kWeaponsWireMaxOffset), 32);
	}
}

bool read_time(BitReader &r_r, int32_t &r_offset) {
	if (r_r.read_bool()) {
		r_offset = kWeaponsWireTimeZero;
		return true;
	}
	const int64_t v = r_r.read_signed(32);
	r_offset = int32_t(v);
	return v >= -kWeaponsWireMaxOffset && v <= kWeaponsWireMaxOffset;
}

uint64_t zigzag(int32_t p_v) {
	return uint64_t((uint32_t(p_v) << 1) ^ uint32_t(p_v >> 31));
}

bool unzigzag(uint64_t p_v, int32_t &r_out) {
	if (p_v > 0xFFFFFFFFull) {
		return false;
	}
	const uint32_t u = uint32_t(p_v);
	r_out = int32_t((u >> 1) ^ (0u - (u & 1u)));
	return true;
}

bool positive_zero(float p_v) {
	return std::bit_cast<uint32_t>(p_v) == 0;
}

// The timed state the view holds for slot p_slot (timers[0], timers[1] or idle), or nullptr.
const WeaponTimersWire *timed_of(const WeaponsWire &p_wire, uint8_t p_slot) {
	for (const WeaponTimersWire &t : p_wire.timers) {
		if (t.slot == p_slot) {
			return &t;
		}
	}
	return p_wire.idle[p_slot].slot == p_slot ? &p_wire.idle[p_slot] : nullptr;
}

// Where the view puts slot p_slot's timed state: timers[0] for the active slot, timers[1] for the pending one,
// idle[p_slot] otherwise (the same rule in weapons_wire_view and read_weapons_wire).
WeaponTimersWire &timed_place(WeaponsWire &r_wire, uint8_t p_slot) {
	if (p_slot == r_wire.active) {
		return r_wire.timers[0];
	}
	if (p_slot == r_wire.pending) {
		return r_wire.timers[1];
	}
	return r_wire.idle[p_slot];
}

} // namespace

WeaponsWire weapons_wire_view(const sw::SoldierWeaponsState &p_state, sim::Tick p_server_tick) {
	const sw::SimTime base = sw::tick_start(p_server_tick);
	WeaponsWire w;
	w.next_shot_index = p_state.next_shot_index;
	w.active = p_state.active;
	w.pending = p_state.pending;
	w.events = p_state.events & mask(kEventBits);
	w.sprint_recover_until = time_offset(p_state.sprint_recover_until, base);
	for (uint8_t i = 0; i < sw::kMaxWeaponSlots; ++i) {
		const sw::WeaponSlotState &s = p_state.slots[i];
		WeaponSlotWire &o = w.slots[i];
		o.mode_counter = uint8_t(s.mode_counter & mask(kCounterBits));
		if (s.def == sw::kNoWeapon) {
			continue;
		}
		o.def = s.def;
		o.mode = uint8_t(s.mode & mask(kModeBits));
		o.magazine = s.magazine;
		o.reserve = s.reserve;
		timed_place(w, i) = timers_of(s, i, base);
	}
	return w;
}

core::Result<sw::SoldierWeaponsState> weapons_from_wire(const WeaponsWire &p_wire, sim::Tick p_server_tick, uint32_t p_player_slot,
		size_t p_def_count) {
	if (p_wire.active >= sw::kMaxWeaponSlots) {
		return core::make_error("weapons wire: active slot " + std::to_string(p_wire.active) + " out of range");
	}
	if (p_wire.pending != 0xFF && p_wire.pending >= sw::kMaxWeaponSlots) {
		return core::make_error("weapons wire: pending slot " + std::to_string(p_wire.pending) + " out of range");
	}
	const sw::SimTime base = sw::tick_start(p_server_tick);
	sw::SoldierWeaponsState st;
	st.tick = p_server_tick;
	st.player_slot = p_player_slot;
	st.next_shot_index = p_wire.next_shot_index;
	st.active = p_wire.active;
	st.pending = p_wire.pending;
	st.events = p_wire.events;
	st.sprint_recover_until = time_from(p_wire.sprint_recover_until, base);
	for (uint8_t i = 0; i < sw::kMaxWeaponSlots; ++i) {
		const WeaponSlotWire &o = p_wire.slots[i];
		sw::WeaponSlotState &s = st.slots[i];
		s.mode_counter = o.mode_counter;
		if (o.def == sw::kNoWeapon) {
			continue;
		}
		if (o.def >= p_def_count) {
			return core::make_error("weapons wire: slot " + std::to_string(i) + " def " + std::to_string(o.def) + " is not in the table (" +
					std::to_string(p_def_count) + ")");
		}
		const WeaponTimersWire *t = timed_of(p_wire, i);
		if (t == nullptr) {
			return core::make_error("weapons wire: slot " + std::to_string(i) + " has no timed state");
		}
		if (uint8_t(t->phase) > uint8_t(sw::WeaponPhase::Releasing)) {
			return core::make_error("weapons wire: slot " + std::to_string(i) + " phase out of range");
		}
		if (!std::isfinite(t->spread) || !std::isfinite(t->recoil.pitch) || !std::isfinite(t->recoil.yaw)) {
			return core::make_error("weapons wire: slot " + std::to_string(i) + " non-finite spread or recoil");
		}
		s.def = o.def;
		s.mode = o.mode;
		s.magazine = o.magazine;
		s.reserve = o.reserve;
		s.phase = t->phase;
		s.burst_left = t->burst_left;
		s.flags = t->flags;
		s.phase_start = time_from(t->phase_start, base);
		s.phase_end = time_from(t->phase_end, base);
		s.next_shot_time = time_from(t->next_shot_time, base);
		s.spread = t->spread;
		s.spread_time = time_from(t->spread_time, base);
		s.recoil = t->recoil;
		s.hold_start = time_from(t->hold_start, base);
	}
	return st;
}

void write_weapons_wire(BitWriter &r_writer, const WeaponsWire &p_wire) {
	r_writer.write_bits(p_wire.next_shot_index, 32);
	r_writer.write_bits(p_wire.active, 8);
	r_writer.write_bits(p_wire.pending, 8);
	r_writer.write_bits(p_wire.events & mask(kEventBits), kEventBits);
	write_time(r_writer, p_wire.sprint_recover_until);
	static const WeaponTimersWire kIdle{};
	for (uint8_t i = 0; i < sw::kMaxWeaponSlots; ++i) {
		const WeaponSlotWire &o = p_wire.slots[i];
		const bool present = o.def != sw::kNoWeapon;
		r_writer.write_bool(present);
		if (!present) {
			r_writer.write_bits(o.mode_counter, kCounterBits);
			continue;
		}
		const WeaponTimersWire *found = timed_of(p_wire, i);
		const WeaponTimersWire &t = found != nullptr ? *found : kIdle;
		r_writer.write_varint(o.def);
		r_writer.write_bits(o.mode, kModeBits);
		r_writer.write_bits(o.mode_counter, kCounterBits);
		r_writer.write_varint(zigzag(o.magazine));
		r_writer.write_varint(zigzag(o.reserve));
		r_writer.write_bits(uint8_t(t.phase), kPhaseBits);
		r_writer.write_bits(t.burst_left, kBurstBits);
		r_writer.write_bits(t.flags, kFlagBits);
		write_time(r_writer, t.phase_start);
		write_time(r_writer, t.phase_end);
		write_time(r_writer, t.next_shot_time);
		r_writer.write_f32(t.spread);
		write_time(r_writer, t.spread_time);
		const bool rest = positive_zero(t.recoil.pitch) && positive_zero(t.recoil.yaw);
		r_writer.write_bool(rest);
		if (!rest) {
			r_writer.write_f32(t.recoil.pitch);
			r_writer.write_f32(t.recoil.yaw);
		}
		write_time(r_writer, t.hold_start);
	}
}

core::Status read_weapons_wire(BitReader &r_reader, WeaponsWire &r_wire) {
	r_wire = WeaponsWire{};
	r_wire.next_shot_index = uint32_t(r_reader.read_bits(32));
	const uint64_t active = r_reader.read_bits(8);
	const uint64_t pending = r_reader.read_bits(8);
	r_wire.events = uint32_t(r_reader.read_bits(kEventBits));
	bool ok = read_time(r_reader, r_wire.sprint_recover_until);
	if (r_reader.failed()) {
		return core::make_error("weapons wire: truncated");
	}
	if (active >= sw::kMaxWeaponSlots || (pending != 0xFF && pending >= sw::kMaxWeaponSlots)) {
		return core::make_error("weapons wire: active or pending slot out of range");
	}
	r_wire.active = uint8_t(active);
	r_wire.pending = uint8_t(pending);
	for (uint8_t i = 0; i < sw::kMaxWeaponSlots && ok && !r_reader.failed(); ++i) {
		WeaponSlotWire &o = r_wire.slots[i];
		if (!r_reader.read_bool()) {
			o.mode_counter = uint8_t(r_reader.read_bits(kCounterBits));
			continue;
		}
		const uint64_t def = r_reader.read_varint();
		o.mode = uint8_t(r_reader.read_bits(kModeBits));
		o.mode_counter = uint8_t(r_reader.read_bits(kCounterBits));
		ok = unzigzag(r_reader.read_varint(), o.magazine) && ok;
		ok = unzigzag(r_reader.read_varint(), o.reserve) && ok;
		if (def >= sw::kNoWeapon) {
			ok = false; // kNoWeapon is the empty bit's job; a larger id does not fit the state
			break;
		}
		o.def = uint16_t(def);
		WeaponTimersWire &t = timed_place(r_wire, i);
		t.slot = i;
		t.phase = sw::WeaponPhase(r_reader.read_bits(kPhaseBits));
		t.burst_left = uint8_t(r_reader.read_bits(kBurstBits));
		t.flags = uint8_t(r_reader.read_bits(kFlagBits));
		ok = read_time(r_reader, t.phase_start) && ok;
		ok = read_time(r_reader, t.phase_end) && ok;
		ok = read_time(r_reader, t.next_shot_time) && ok;
		t.spread = r_reader.read_f32();
		ok = read_time(r_reader, t.spread_time) && ok;
		if (!r_reader.read_bool()) {
			t.recoil.pitch = r_reader.read_f32();
			t.recoil.yaw = r_reader.read_f32();
		}
		ok = read_time(r_reader, t.hold_start) && ok;
		ok = ok && std::isfinite(t.spread) && std::isfinite(t.recoil.pitch) && std::isfinite(t.recoil.yaw);
	}
	if (r_reader.failed()) {
		return core::make_error("weapons wire: truncated");
	}
	if (!ok) {
		return core::make_error("weapons wire: field out of range");
	}
	return core::Status::success();
}

} // namespace bf3::net

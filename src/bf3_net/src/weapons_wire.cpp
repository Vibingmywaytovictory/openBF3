#include "bf3/net/weapons_wire.h"

#include "weapons_wire_layout.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace bf3::net {

namespace sw = sim::weapons;

namespace {

constexpr unsigned kModeBits = 2;
constexpr unsigned kCounterBits = 2;
constexpr unsigned kBurstBits = 6;
constexpr unsigned kFlagBits = 7;
constexpr unsigned kEventBits = 16;
constexpr unsigned kPhaseBits = 3;
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

const WeaponTimersWire *timed_of(const WeaponsWire &p_wire, uint8_t p_slot) {
	for (const WeaponTimersWire &t : p_wire.timers) {
		if (t.slot == p_slot) {
			return &t;
		}
	}
	return p_wire.idle[p_slot].slot == p_slot ? &p_wire.idle[p_slot] : nullptr;
}

WeaponTimersWire &timed_place(WeaponsWire &r_wire, uint8_t p_slot) {
	if (p_slot == r_wire.active) {
		return r_wire.timers[0];
	}
	if (p_slot == r_wire.pending) {
		return r_wire.timers[1];
	}
	return r_wire.idle[p_slot];
}

const WeaponTimersWire &timers_for(const WeaponsWire &p_wire, uint8_t p_slot) {
	static const WeaponTimersWire kIdle{};
	const WeaponTimersWire *found = timed_of(p_wire, p_slot);
	return found != nullptr ? *found : kIdle;
}

WeaponTimersWire &timers_for(WeaponsWire &r_wire, uint8_t p_slot) {
	WeaponTimersWire &t = timed_place(r_wire, p_slot);
	t.slot = p_slot;
	return t;
}

template <class A, MaybeConst<int32_t> T>
bool wire_offset(A &r_a, T &r_offset) {
	bool zero = r_offset == kWeaponsWireTimeZero;
	r_a.bits(zero, 1);
	if (zero) {
		r_a.store(r_offset, [] { return kWeaponsWireTimeZero; });
		return true;
	}
	int32_t offset = std::clamp(r_offset, -kWeaponsWireMaxOffset, kWeaponsWireMaxOffset);
	r_a.bits(offset, 32);
	r_a.store(r_offset, [&] { return offset; });
	return offset >= -kWeaponsWireMaxOffset && offset <= kWeaponsWireMaxOffset;
}

template <class A, MaybeConst<int32_t> T>
bool zigzag_varint(A &r_a, T &r_value) {
	uint64_t code = zigzag(r_value);
	r_a.varint(code);
	bool fits = true;
	r_a.store(r_value, [&] {
		int32_t value = 0;
		fits = unzigzag(code, value);
		return value;
	});
	return fits;
}

template <class A, MaybeConst<WeaponsWire> W>
void serialize_weapons(A &r_a, W &r_wire, const WeaponsWireFaults &p_faults) {
	r_a.bits(r_wire.next_shot_index, 32);
	r_a.bits(r_wire.active, 8);
	r_a.bits(r_wire.pending, 8);
	r_a.bits(r_wire.events, kEventBits);
	const bool sprint_ok = wire_offset(r_a, r_wire.sprint_recover_until);
	r_a.check(p_faults.slots, [&] {
		return r_wire.active < sw::kMaxWeaponSlots && (r_wire.pending == 0xFF || r_wire.pending < sw::kMaxWeaponSlots);
	});
	r_a.check(p_faults.field, [&] { return sprint_ok; });
	for (uint8_t i = 0; i < sw::kMaxWeaponSlots && r_a.ok(); ++i) {
		auto &o = r_wire.slots[i];
		bool present = o.def != sw::kNoWeapon;
		r_a.bits(present, 1);
		if (!present) {
			r_a.bits(o.mode_counter, kCounterBits);
			continue;
		}
		auto &t = timers_for(r_wire, i);
		uint64_t def = o.def;
		r_a.varint(def);
		r_a.bits(o.mode, kModeBits);
		r_a.bits(o.mode_counter, kCounterBits);
		bool ok = zigzag_varint(r_a, o.magazine);
		ok = zigzag_varint(r_a, o.reserve) && ok;
		r_a.check(p_faults.field, [&] { return def < sw::kNoWeapon; });
		r_a.store(o.def, [&] { return uint16_t(def); });
		r_a.bits(t.phase, kPhaseBits);
		r_a.bits(t.burst_left, kBurstBits);
		r_a.bits(t.flags, kFlagBits);
		ok = wire_offset(r_a, t.phase_start) && ok;
		ok = wire_offset(r_a, t.phase_end) && ok;
		ok = wire_offset(r_a, t.next_shot_time) && ok;
		r_a.f32(t.spread);
		ok = wire_offset(r_a, t.spread_time) && ok;
		bool rest = positive_zero(t.recoil.pitch) && positive_zero(t.recoil.yaw);
		r_a.bits(rest, 1);
		if (!rest) {
			r_a.f32(t.recoil.pitch);
			r_a.f32(t.recoil.yaw);
		}
		ok = wire_offset(r_a, t.hold_start) && ok;
		r_a.check(p_faults.field, [&] { return ok && std::isfinite(t.spread) && std::isfinite(t.recoil.pitch) && std::isfinite(t.recoil.yaw); });
	}
}

}

void serialize(BitWriteArchive &r_archive, const WeaponsWire &p_wire, const WeaponsWireFaults &p_faults) {
	serialize_weapons(r_archive, p_wire, p_faults);
}

void serialize(BitReadArchive &r_archive, WeaponsWire &r_wire, const WeaponsWireFaults &p_faults) {
	serialize_weapons(r_archive, r_wire, p_faults);
}

sw::SimTime weapons_wire_time(int32_t p_offset, sim::Tick p_server_tick) {
	return time_from(p_offset, sw::tick_start(p_server_tick));
}

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
	BitWriteArchive archive(r_writer);
	serialize(archive, p_wire, {});
}

core::Status read_weapons_wire(BitReader &r_reader, WeaponsWire &r_wire) {
	r_wire = WeaponsWire{};
	BitReadArchive archive(r_reader);
	serialize(archive, r_wire, { "active or pending slot out of range", "field out of range" });
	if (const char *why = archive.error()) {
		return core::make_error(std::string("weapons wire: ") + why);
	}
	return core::Status::success();
}

}
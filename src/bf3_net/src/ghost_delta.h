#pragma once

#include "bf3/net/bit_stream.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/soldier_state.h"
#include "bit_archive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace bf3::net {

inline constexpr int32_t kOtherPitchMaxCode = (1 << (quant::kSoldierPitchBits - 1)) - 1;
inline constexpr unsigned kGhostBackBits = 5;
inline constexpr uint8_t kGhostSpecialSlot = 0x80;
inline constexpr unsigned kOtherPoseBits = 24;
inline constexpr unsigned kOtherGearBits = 21;
inline constexpr uint32_t kMaxDeltaTicks = 4096;
static_assert(kMaxBaselineBack == (1u << kGhostBackBits) - 1u);

constexpr uint32_t other_yaw_code(uint16_t p_yaw) {
	return ((uint32_t(p_yaw) + 2u) >> 2) & ((1u << quant::kSoldierYawBits) - 1u);
}

constexpr int32_t other_pitch_code(int16_t p_pitch) {
	const int32_t code = (int32_t(p_pitch) + 2) >> 2;
	return std::clamp(code, -kOtherPitchMaxCode, kOtherPitchMaxCode);
}

struct OtherCodes {
	uint8_t generation = 0;
	std::array<uint32_t, 3> position{};
	std::array<uint32_t, 3> velocity{};
	uint32_t yaw = 0;
	int32_t pitch = 0;
	uint32_t pose = 0;
	uint32_t gear = 0;

	bool operator==(const OtherCodes &) const = default;
};

constexpr OtherCodes other_codes(const OtherSoldier &p_o) {
	OtherCodes c;
	c.generation = p_o.generation;
	c.position[0] = quant::kPositionXZ.encode(p_o.position[0]);
	c.position[1] = quant::kPositionY.encode(p_o.position[1]);
	c.position[2] = quant::kPositionXZ.encode(p_o.position[2]);
	for (size_t k = 0; k < 3; ++k) {
		c.velocity[k] = quant::kSoldierVelocity.encode(p_o.velocity[k]);
	}
	c.yaw = other_yaw_code(p_o.yaw);
	c.pitch = other_pitch_code(p_o.pitch);
	c.pose = (uint32_t(p_o.pose) & 3u) | ((uint32_t(p_o.target_pose) & 3u) << 2) | (uint32_t(p_o.transition) << 4) |
			((uint32_t(p_o.mode) & 15u) << 12) | (uint32_t(p_o.flags) << 16);
	c.gear = uint32_t(p_o.weapon) | ((uint32_t(p_o.weapon_phase) & 7u) << 8) | (uint32_t(p_o.firing) << 11) |
			(uint32_t(p_o.ads) << 12) | ((uint32_t(p_o.reload_seq) & 3u) << 13) |
			((uint32_t(p_o.burst_seq) & 15u) << 15) | ((uint32_t(p_o.team) & 3u) << 19);
	return c;
}

constexpr OtherSoldier other_of_codes(uint8_t p_slot, const OtherCodes &p_c) {
	OtherSoldier o;
	o.slot = p_slot;
	o.generation = p_c.generation;
	o.position[0] = quant::kPositionXZ.decode(p_c.position[0]);
	o.position[1] = quant::kPositionY.decode(p_c.position[1]);
	o.position[2] = quant::kPositionXZ.decode(p_c.position[2]);
	for (size_t k = 0; k < 3; ++k) {
		o.velocity[k] = quant::kSoldierVelocity.decode(p_c.velocity[k]);
	}
	o.yaw = uint16_t(p_c.yaw << 2);
	o.pitch = int16_t(p_c.pitch * 4);
	o.pose = sim::Pose(p_c.pose & 3u);
	o.target_pose = sim::Pose((p_c.pose >> 2) & 3u);
	o.transition = uint8_t(p_c.pose >> 4);
	o.mode = sim::MoveMode((p_c.pose >> 12) & 15u);
	o.flags = uint8_t(p_c.pose >> 16);
	o.weapon = uint8_t(p_c.gear);
	o.weapon_phase = uint8_t((p_c.gear >> 8) & 7u);
	o.firing = ((p_c.gear >> 11) & 1u) != 0;
	o.ads = ((p_c.gear >> 12) & 1u) != 0;
	o.reload_seq = uint8_t((p_c.gear >> 13) & 3u);
	o.burst_seq = uint8_t((p_c.gear >> 15) & 15u);
	o.team = uint8_t((p_c.gear >> 19) & 3u);
	return o;
}

std::array<uint32_t, 3> predicted_position(
		const OtherCodes &p_base, const std::array<uint32_t, 3> &p_velocity, uint32_t p_dt);
bool canonical_weapons(const WeaponsWire &p_wire, WeaponsWire &r_canonical);
int32_t shifted_offset(int32_t p_offset, int64_t p_shift);

template <class A, MaybeConst<OtherCodes> C> constexpr void serialize(A &r_a, C &r_c) {
	r_a.bits(r_c.generation, 8);
	r_a.bits(r_c.position[0], quant::kPositionXZ.bits);
	r_a.bits(r_c.position[1], quant::kPositionY.bits);
	r_a.bits(r_c.position[2], quant::kPositionXZ.bits);
	for (auto &v : r_c.velocity) {
		r_a.bits(v, quant::kSoldierVelocity.bits);
	}
	r_a.bits(r_c.yaw, quant::kSoldierYawBits);
	r_a.bits(r_c.pitch, quant::kSoldierPitchBits);
	r_a.bits(r_c.pose, kOtherPoseBits);
	r_a.bits(r_c.gear, kOtherGearBits);
}

constexpr bool fits_signed(int64_t p_value, unsigned p_bits) {
	return p_value >= -(int64_t(1) << (p_bits - 1)) && p_value < (int64_t(1) << (p_bits - 1));
}

constexpr int64_t wrapped(int64_t p_diff, unsigned p_bits) {
	const uint64_t m = (uint64_t(1) << p_bits) - 1u;
	const uint64_t u = uint64_t(p_diff) & m;
	return (u & (uint64_t(1) << (p_bits - 1))) != 0 ? int64_t(u) - int64_t(m + 1u) : int64_t(u);
}

template <class A, class V>
constexpr void residual(A &r_a, V &r_value, int64_t p_predicted, unsigned p_bits, unsigned p_narrow, unsigned p_wide,
		bool p_wrap, int64_t p_lo, int64_t p_hi, bool &r_ok) {
	int64_t diff = int64_t(r_value) - p_predicted;
	if (p_wrap) {
		diff = wrapped(diff, p_bits);
	}
	uint8_t select = diff == 0 ? 0 : fits_signed(diff, p_narrow) ? 1 : fits_signed(diff, p_wide) ? 2 : 3;
	r_a.bits(select, 2);
	if (select == 3) {
		r_a.bits(r_value, p_bits);
		return;
	}
	int32_t d = int32_t(diff);
	if (select != 0) {
		r_a.bits(d, select == 1 ? p_narrow : p_wide);
	} else {
		r_a.store(d, [] { return 0; });
	}
	r_a.store(r_value, [&] {
		int64_t v = p_predicted + d;
		if (p_wrap) {
			v = int64_t(uint64_t(v) & ((uint64_t(1) << p_bits) - 1u));
		}
		if (v < p_lo || v > p_hi) {
			r_ok = false;
			v = p_lo;
		}
		return std::remove_cvref_t<V>(v);
	});
}

template <class A, MaybeConst<OtherCodes> C>
constexpr void delta(A &r_a, C &r_c, const OtherCodes &p_base, uint32_t p_dt, bool &r_ok) {
	constexpr int64_t kVelocityMax = (int64_t(1) << quant::kSoldierVelocity.bits) - 1;
	bool velocity = r_c.velocity != p_base.velocity;
	r_a.bits(velocity, 1);
	for (size_t k = 0; k < 3; ++k) {
		if (velocity) {
			residual(r_a, r_c.velocity[k], p_base.velocity[k], quant::kSoldierVelocity.bits, 4, 7, false, 0,
					kVelocityMax, r_ok);
		} else {
			r_a.store(r_c.velocity[k], [&] { return p_base.velocity[k]; });
		}
	}
	const std::array<uint32_t, 3> predicted = predicted_position(p_base, r_c.velocity, p_dt);
	bool position = r_c.position != predicted;
	r_a.bits(position, 1);
	for (size_t k = 0; k < 3; ++k) {
		const unsigned bits = k == 1 ? quant::kPositionY.bits : quant::kPositionXZ.bits;
		if (position) {
			residual(r_a, r_c.position[k], predicted[k], bits, 5, 10, false, 0, (int64_t(1) << bits) - 1, r_ok);
		} else {
			r_a.store(r_c.position[k], [&] { return predicted[k]; });
		}
	}
	bool view = r_c.yaw != p_base.yaw || r_c.pitch != p_base.pitch;
	r_a.bits(view, 1);
	if (view) {
		residual(r_a, r_c.yaw, p_base.yaw, quant::kSoldierYawBits, 7, 11, true, 0,
				(int64_t(1) << quant::kSoldierYawBits) - 1, r_ok);
		residual(r_a, r_c.pitch, p_base.pitch, quant::kSoldierPitchBits, 6, 9, false, -kOtherPitchMaxCode,
				kOtherPitchMaxCode, r_ok);
	} else {
		r_a.store(r_c.yaw, [&] { return p_base.yaw; });
		r_a.store(r_c.pitch, [&] { return p_base.pitch; });
	}
	bool pose = r_c.pose != p_base.pose;
	r_a.bits(pose, 1);
	if (pose) {
		r_a.bits(r_c.pose, kOtherPoseBits);
	} else {
		r_a.store(r_c.pose, [&] { return p_base.pose; });
	}
	bool gear = r_c.gear != p_base.gear;
	r_a.bits(gear, 1);
	if (gear) {
		r_a.bits(r_c.gear, kOtherGearBits);
	} else {
		r_a.store(r_c.gear, [&] { return p_base.gear; });
	}
	r_a.store(r_c.generation, [&] { return p_base.generation; });
}

template <class A, class T>
constexpr void lane(A &r_a, T &r_value, const std::remove_const_t<T> &p_predicted, unsigned p_bits) {
	bool changed = !(r_value == p_predicted);
	r_a.bits(changed, 1);
	if (changed) {
		r_a.bits(r_value, p_bits);
	} else {
		r_a.store(r_value, [&] { return p_predicted; });
	}
}

template <class A, MaybeConst<float> F> constexpr void float_lane(A &r_a, F &r_value, float p_predicted) {
	const uint32_t base = std::bit_cast<uint32_t>(p_predicted);
	uint32_t code = std::bit_cast<uint32_t>(r_value);
	bool changed = code != base;
	r_a.bits(changed, 1);
	if (!changed) {
		r_a.store(r_value, [&] { return p_predicted; });
		return;
	}
	const int64_t diff = wrapped(int64_t(code) - int64_t(base), 32);
	uint8_t select = fits_signed(diff, 10) ? 0 : fits_signed(diff, 18) ? 1 : fits_signed(diff, 26) ? 2 : 3;
	r_a.bits(select, 2);
	if (select == 3) {
		r_a.bits(code, 32);
	} else {
		int32_t d = int32_t(diff);
		r_a.bits(d, select == 0 ? 10u : select == 1 ? 18u : 26u);
		r_a.store(code, [&] { return uint32_t(base + uint32_t(d)); });
	}
	r_a.store(r_value, [&] { return std::bit_cast<float>(code); });
}

constexpr bool float_differs(float p_value, float p_base) {
	return std::bit_cast<uint32_t>(p_value) != std::bit_cast<uint32_t>(p_base);
}

template <class A, MaybeConst<sim::SoldierState> S>
constexpr void delta(A &r_a, S &r_s, const sim::SoldierState &p_b, sim::Tick p_tick) {
	const auto vec_differs = [](const sim::Vec3 &p_x, const sim::Vec3 &p_y) {
		return std::bit_cast<std::array<uint32_t, 3>>(p_x) != std::bit_cast<std::array<uint32_t, 3>>(p_y);
	};
	bool motion = vec_differs(r_s.position, p_b.position) || vec_differs(r_s.velocity, p_b.velocity);
	r_a.bits(motion, 1);
	for (size_t k = 0; k < 3; ++k) {
		if (motion) {
			float_lane(r_a, r_s.position[k], p_b.position[k]);
		} else {
			r_a.store(r_s.position[k], [&] { return p_b.position[k]; });
		}
	}
	for (size_t k = 0; k < 3; ++k) {
		if (motion) {
			float_lane(r_a, r_s.velocity[k], p_b.velocity[k]);
		} else {
			r_a.store(r_s.velocity[k], [&] { return p_b.velocity[k]; });
		}
	}
	bool view = r_s.yaw != p_b.yaw || r_s.pitch != p_b.pitch;
	r_a.bits(view, 1);
	if (view) {
		lane(r_a, r_s.yaw, p_b.yaw, 16);
		lane(r_a, r_s.pitch, p_b.pitch, 16);
	} else {
		r_a.store(r_s.yaw, [&] { return p_b.yaw; });
		r_a.store(r_s.pitch, [&] { return p_b.pitch; });
	}
	bool normal = vec_differs(r_s.ground_normal, p_b.ground_normal);
	r_a.bits(normal, 1);
	for (size_t k = 0; k < 3; ++k) {
		if (normal) {
			float_lane(r_a, r_s.ground_normal[k], p_b.ground_normal[k]);
		} else {
			r_a.store(r_s.ground_normal[k], [&] { return p_b.ground_normal[k]; });
		}
	}
	bool body = r_s.life != p_b.life || r_s.pose != p_b.pose || r_s.target_pose != p_b.target_pose ||
			r_s.transition_ticks != p_b.transition_ticks || r_s.transition_total != p_b.transition_total ||
			r_s.mode != p_b.mode || r_s.flags != p_b.flags || r_s.events != p_b.events ||
			r_s.jump_penalty_ticks != p_b.jump_penalty_ticks || r_s.air_ticks != p_b.air_ticks ||
			r_s.protect_ticks != p_b.protect_ticks;
	r_a.bits(body, 1);
	if (body) {
		lane(r_a, r_s.life, p_b.life, 32);
		lane(r_a, r_s.pose, p_b.pose, 8);
		lane(r_a, r_s.target_pose, p_b.target_pose, 8);
		lane(r_a, r_s.transition_ticks, p_b.transition_ticks, 16);
		lane(r_a, r_s.transition_total, p_b.transition_total, 16);
		lane(r_a, r_s.mode, p_b.mode, 8);
		lane(r_a, r_s.flags, p_b.flags, 16);
		lane(r_a, r_s.events, p_b.events, 16);
		lane(r_a, r_s.jump_penalty_ticks, p_b.jump_penalty_ticks, 16);
		lane(r_a, r_s.air_ticks, p_b.air_ticks, 16);
		lane(r_a, r_s.protect_ticks, p_b.protect_ticks, 16);
	} else {
		r_a.store(r_s.life, [&] { return p_b.life; });
		r_a.store(r_s.pose, [&] { return p_b.pose; });
		r_a.store(r_s.target_pose, [&] { return p_b.target_pose; });
		r_a.store(r_s.transition_ticks, [&] { return p_b.transition_ticks; });
		r_a.store(r_s.transition_total, [&] { return p_b.transition_total; });
		r_a.store(r_s.mode, [&] { return p_b.mode; });
		r_a.store(r_s.flags, [&] { return p_b.flags; });
		r_a.store(r_s.events, [&] { return p_b.events; });
		r_a.store(r_s.jump_penalty_ticks, [&] { return p_b.jump_penalty_ticks; });
		r_a.store(r_s.air_ticks, [&] { return p_b.air_ticks; });
		r_a.store(r_s.protect_ticks, [&] { return p_b.protect_ticks; });
	}
	bool health = float_differs(r_s.health, p_b.health) || float_differs(r_s.impact_speed, p_b.impact_speed) ||
			float_differs(r_s.damage, p_b.damage);
	r_a.bits(health, 1);
	if (health) {
		float_lane(r_a, r_s.health, p_b.health);
		float_lane(r_a, r_s.impact_speed, p_b.impact_speed);
		float_lane(r_a, r_s.damage, p_b.damage);
	} else {
		r_a.store(r_s.health, [&] { return p_b.health; });
		r_a.store(r_s.impact_speed, [&] { return p_b.impact_speed; });
		r_a.store(r_s.damage, [&] { return p_b.damage; });
	}
	r_a.store(r_s.tick, [&] { return p_tick; });
}

inline WeaponTimersWire shifted(const WeaponTimersWire &p_t, int64_t p_shift) {
	if (p_t.slot == WeaponTimersWire().slot) {
		return p_t;
	}
	WeaponTimersWire t = p_t;
	t.phase_start = shifted_offset(p_t.phase_start, p_shift);
	t.phase_end = shifted_offset(p_t.phase_end, p_shift);
	t.next_shot_time = shifted_offset(p_t.next_shot_time, p_shift);
	t.spread_time = shifted_offset(p_t.spread_time, p_shift);
	t.hold_start = shifted_offset(p_t.hold_start, p_shift);
	return t;
}

template <class A, MaybeConst<WeaponTimersWire> T>
constexpr void timers_delta(A &r_a, T &r_t, const WeaponTimersWire &p_p) {
	bool changed = !(r_t == p_p);
	r_a.bits(changed, 1);
	if (!changed) {
		r_a.store(r_t, [&] { return p_p; });
		return;
	}
	lane(r_a, r_t.slot, p_p.slot, 8);
	lane(r_a, r_t.phase, p_p.phase, 8);
	lane(r_a, r_t.burst_left, p_p.burst_left, 8);
	lane(r_a, r_t.flags, p_p.flags, 8);
	lane(r_a, r_t.phase_start, p_p.phase_start, 32);
	lane(r_a, r_t.phase_end, p_p.phase_end, 32);
	lane(r_a, r_t.next_shot_time, p_p.next_shot_time, 32);
	lane(r_a, r_t.spread_time, p_p.spread_time, 32);
	lane(r_a, r_t.hold_start, p_p.hold_start, 32);
	float_lane(r_a, r_t.spread, p_p.spread);
	float_lane(r_a, r_t.recoil.pitch, p_p.recoil.pitch);
	float_lane(r_a, r_t.recoil.yaw, p_p.recoil.yaw);
}

template <class A, MaybeConst<WeaponsWire> W>
constexpr void delta(A &r_a, W &r_w, const WeaponsWire &p_b, int64_t p_shift) {
	const int32_t sprint = shifted_offset(p_b.sprint_recover_until, p_shift);
	bool head = r_w.next_shot_index != p_b.next_shot_index || r_w.active != p_b.active || r_w.pending != p_b.pending ||
			r_w.events != p_b.events || r_w.sprint_recover_until != sprint;
	r_a.bits(head, 1);
	if (head) {
		lane(r_a, r_w.next_shot_index, p_b.next_shot_index, 32);
		lane(r_a, r_w.active, p_b.active, 8);
		lane(r_a, r_w.pending, p_b.pending, 8);
		lane(r_a, r_w.events, p_b.events, 32);
		lane(r_a, r_w.sprint_recover_until, sprint, 32);
	} else {
		r_a.store(r_w.next_shot_index, [&] { return p_b.next_shot_index; });
		r_a.store(r_w.active, [&] { return p_b.active; });
		r_a.store(r_w.pending, [&] { return p_b.pending; });
		r_a.store(r_w.events, [&] { return p_b.events; });
		r_a.store(r_w.sprint_recover_until, [&] { return sprint; });
	}
	for (size_t i = 0; i < r_w.slots.size(); ++i) {
		auto &s = r_w.slots[i];
		const WeaponSlotWire &b = p_b.slots[i];
		bool changed = !(s == b);
		r_a.bits(changed, 1);
		if (!changed) {
			r_a.store(s, [&] { return b; });
			continue;
		}
		lane(r_a, s.def, b.def, 16);
		lane(r_a, s.mode, b.mode, 8);
		lane(r_a, s.mode_counter, b.mode_counter, 8);
		lane(r_a, s.magazine, b.magazine, 32);
		lane(r_a, s.reserve, b.reserve, 32);
	}
	for (size_t i = 0; i < r_w.timers.size(); ++i) {
		timers_delta(r_a, r_w.timers[i], shifted(p_b.timers[i], p_shift));
	}
	for (size_t i = 0; i < r_w.idle.size(); ++i) {
		timers_delta(r_a, r_w.idle[i], shifted(p_b.idle[i], p_shift));
	}
}

}

#include "ghost_delta.h"

#include "bf3/sim/weapons/sim_time.h"

#include <cmath>

namespace bf3::net {

namespace {

constexpr int64_t kVelocityZero = int64_t(quant::kSoldierVelocity.encode(0.0f));

constexpr int64_t inverse_step(const Quantizer &p_q) {
	return int64_t(1.0 / double(p_q.step) + 0.5);
}

constexpr int64_t rounded_div(int64_t p_num, int64_t p_den) {
	return p_num >= 0 ? (p_num + p_den / 2) / p_den : -((-p_num + p_den / 2) / p_den);
}

size_t ring_index(uint16_t p_seq) {
	return size_t(p_seq) % kBaselineRing;
}

}

std::array<uint32_t, 3> predicted_position(
		const OtherCodes &p_base, const std::array<uint32_t, 3> &p_velocity, uint32_t p_dt) {
	constexpr int64_t kVelocityInverse = inverse_step(quant::kSoldierVelocity);
	const int64_t dt = int64_t(std::min(p_dt, kMaxDeltaTicks));
	std::array<uint32_t, 3> out{};
	for (size_t k = 0; k < 3; ++k) {
		const Quantizer &q = k == 1 ? quant::kPositionY : quant::kPositionXZ;
		const int64_t velocity_sum = int64_t(p_base.velocity[k]) + int64_t(p_velocity[k]) - 2 * kVelocityZero;
		const int64_t moved =
				rounded_div(velocity_sum * dt * inverse_step(q), 2 * kVelocityInverse * int64_t(sim::kSimHz));
		out[k] = uint32_t(std::clamp<int64_t>(int64_t(p_base.position[k]) + moved, 0, int64_t(q.max_code())));
	}
	return out;
}

bool canonical_weapons(const WeaponsWire &p_wire, WeaponsWire &r_canonical) {
	BitWriter w(kMaxDatagramBytes * 4);
	write_weapons_wire(w, p_wire);
	if (w.overflowed()) {
		return false;
	}
	const std::vector<uint8_t> bytes = w.take();
	BitReader r(bytes);
	return read_weapons_wire(r, r_canonical).ok();
}

int32_t shifted_offset(int32_t p_offset, int64_t p_shift) {
	if (p_offset == kWeaponsWireTimeZero) {
		return p_offset;
	}
	return int32_t(std::clamp<int64_t>(
			int64_t(p_offset) - p_shift, -int64_t(kWeaponsWireMaxOffset), int64_t(kWeaponsWireMaxOffset)));
}

SnapshotBaselines::SnapshotBaselines() : others_(kMaxSlots * kBaselineRing), own_(kBaselineRing) {}

void SnapshotBaselines::clear() {
	std::fill(others_.begin(), others_.end(), OtherEntry());
	std::fill(own_.begin(), own_.end(), OwnEntry());
}

void SnapshotBaselines::keep(uint16_t p_seq, sim::Tick p_tick, const std::optional<sim::SoldierState> &p_own,
		const std::optional<WeaponsWire> &p_weapons, std::span<const OtherSoldier> p_others) {
	if (p_own) {
		OwnEntry &e = own_[ring_index(p_seq)];
		e.valid = true;
		e.seq = p_seq;
		e.tick = p_tick;
		e.state = *p_own;
		e.has_weapons = p_weapons.has_value();
		e.weapons = p_weapons.value_or(WeaponsWire());
	}
	for (const OtherSoldier &o : p_others) {
		if (o.slot >= kMaxSlots) {
			continue;
		}
		OtherEntry &e = others_[size_t(o.slot) * kBaselineRing + ring_index(p_seq)];
		e.valid = true;
		e.seq = p_seq;
		e.tick = p_tick;
		e.record = o;
	}
}

const OtherSoldier *SnapshotBaselines::other(uint8_t p_slot, uint16_t p_seq, sim::Tick &r_tick) const {
	if (p_slot >= kMaxSlots) {
		return nullptr;
	}
	const OtherEntry &e = others_[size_t(p_slot) * kBaselineRing + ring_index(p_seq)];
	if (!e.valid || e.seq != p_seq) {
		return nullptr;
	}
	r_tick = e.tick;
	return &e.record;
}

const sim::SoldierState *SnapshotBaselines::own(
		uint16_t p_seq, sim::Tick &r_tick, const WeaponsWire *&r_weapons) const {
	const OwnEntry &e = own_[ring_index(p_seq)];
	if (!e.valid || e.seq != p_seq) {
		return nullptr;
	}
	r_tick = e.tick;
	r_weapons = e.has_weapons ? &e.weapons : nullptr;
	return &e.state;
}

}

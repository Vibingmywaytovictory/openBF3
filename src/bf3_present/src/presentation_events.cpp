#include "bf3/present/presentation_events.h"

#include <algorithm>
#include <cmath>

namespace bf3::present {

const char *present_kind_name(PresentKind p_kind) {
	switch (p_kind) {
		case PresentKind::OwnShot:
			return "OwnShot";
		case PresentKind::OwnTimed:
			return "OwnTimed";
		case PresentKind::RemoteShot:
			return "RemoteShot";
		case PresentKind::Impact:
			return "Impact";
		case PresentKind::SoldierContact:
			return "SoldierContact";
		case PresentKind::Hit:
			return "Hit";
		case PresentKind::Death:
			return "Death";
		case PresentKind::Detonation:
			return "Detonation";
		case PresentKind::Spawn:
			return "Spawn";
		case PresentKind::Revive:
			return "Revive";
		case PresentKind::GadgetDeploy:
			return "GadgetDeploy";
		case PresentKind::GadgetRemove:
			return "GadgetRemove";
	}
	return "?";
}

void set_death_record(PresentEvent &r_event, const core::gameplay::DeathRecord &p_record) {
	r_event.pellet = uint8_t((p_record.valid ? 1u : 0u) | (p_record.explosion ? 2u : 0u) |
			(p_record.sprinting ? 4u : 0u) | (uint32_t(p_record.direction) << 3));
	r_event.pos[0] =
			float(uint32_t(p_record.bone) * core::gameplay::kDeathAnimationVariants + p_record.animation_index);
	r_event.dir[0] = float(p_record.forward_speed);
	r_event.dir[1] = float(p_record.right_speed);
	r_event.dir[2] = p_record.random_value();
}

core::gameplay::DeathRecord death_record_of(const PresentEvent &p_event) {
	core::gameplay::DeathRecord r;
	r.valid = (p_event.pellet & 1u) != 0;
	if (!r.valid) {
		return r;
	}
	r.explosion = (p_event.pellet & 2u) != 0;
	r.sprinting = (p_event.pellet & 4u) != 0;
	r.direction = core::gameplay::DeathHitDirection(
			std::min<uint32_t>(uint32_t(p_event.pellet) >> 3, uint32_t(core::gameplay::DeathHitDirection::None)));
	const uint32_t packed = std::isfinite(p_event.pos[0]) && p_event.pos[0] >= 0.0f ? uint32_t(p_event.pos[0]) : 0u;
	r.bone = core::gameplay::death_bone_of_type(packed / core::gameplay::kDeathAnimationVariants);
	r.animation_index = uint8_t(packed % core::gameplay::kDeathAnimationVariants);
	r.forward_speed = int8_t(std::clamp(std::lround(p_event.dir[0]), -1L, 1L));
	r.right_speed = int8_t(std::clamp(std::lround(p_event.dir[1]), -1L, 1L));
	const double k = std::isfinite(p_event.dir[2])
			? std::round(double(p_event.dir[2]) * double(uint32_t(1) << core::gameplay::kDeathRandomBits))
			: 1.0;
	r.random = uint32_t(std::clamp(k, 1.0, double(uint32_t(1) << core::gameplay::kDeathRandomBits)) - 1.0) &
			core::gameplay::kDeathRandomMask;
	return r;
}

bool PresentationEvents::push(const PresentEvent &p_event) {
	const uint64_t head = head_.load(std::memory_order_relaxed);
	const uint64_t tail = tail_.load(std::memory_order_acquire);
	if (head - tail >= kPresentationEventCapacity) {
		overflows_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	ring_[size_t(head % kPresentationEventCapacity)] = p_event;
	head_.store(head + 1, std::memory_order_release);
	return true;
}

size_t PresentationEvents::drain(std::span<PresentEvent> r_out) {
	const uint64_t tail = tail_.load(std::memory_order_relaxed);
	const uint64_t head = head_.load(std::memory_order_acquire);
	const size_t n = size_t(std::min<uint64_t>(head - tail, r_out.size()));
	for (size_t i = 0; i < n; ++i) {
		r_out[i] = ring_[size_t((tail + i) % kPresentationEventCapacity)];
	}
	tail_.store(tail + n, std::memory_order_release);
	return n;
}

uint64_t PresentationEvents::overflows() const {
	return overflows_.load(std::memory_order_relaxed);
}

}

#pragma once

#include "bf3/present/presentation_events.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace bf3::present {

struct PresentedSoldier {
	uint8_t slot = 0;
	uint8_t generation = 0;
	sim::Vec3 position{};
	sim::Vec3 velocity{};
	float yaw = 0.0f;
	float pitch = 0.0f;
	sim::Pose pose = sim::Pose::Stand;
	float eye_height = 1.6f;
	sim::MoveMode mode = sim::MoveMode::OnGround;
	uint8_t flags = 0;
	bool extrapolated = false;

	sim::Pose target_pose = sim::Pose::Stand;
	float transition = 0.0f;
};

struct PresentedOtherWeapon {
	bool valid = false;
	uint8_t slot = 0;
	uint8_t generation = 0;
	uint16_t def = sim::weapons::kNoWeapon;
	sim::weapons::WeaponPhase phase = sim::weapons::WeaponPhase::None;
	bool firing = false;
	bool ads = false;
	uint8_t reload_seq = 0;
	uint8_t burst_seq = 0;
	uint8_t team = 0;
	uint32_t sample_tick = 0;
};
static_assert(std::is_trivially_copyable_v<PresentedOtherWeapon>);

struct PresentedOwnSlot {
	uint16_t def = sim::weapons::kNoWeapon;
	int32_t magazine = 0;
};

struct PresentedOwnWeapons {
	bool changed = false;
	uint8_t active = 0;
	uint32_t events = 0;
	uint8_t trigger_slot = 0xFF;
	uint8_t trigger_flags = 0;
	std::array<PresentedOwnSlot, sim::weapons::kMaxWeaponSlots> slots{};
};
static_assert(std::is_trivially_copyable_v<PresentedOwnWeapons>);

struct PresentClock {
	bool predicted_valid = false;
	bool render_valid = false;
	sim::weapons::SimTime predicted = 0;
	sim::weapons::SimTime render = 0;
	uint64_t now_ns = 0;

	sim::weapons::SimTime now(PresentTimeline p_timeline) const {
		return p_timeline == PresentTimeline::Predicted ? predicted : (p_timeline == PresentTimeline::Render ? render : 0);
	}
	bool valid(PresentTimeline p_timeline) const {
		return p_timeline == PresentTimeline::Predicted ? predicted_valid : (p_timeline == PresentTimeline::Render ? render_valid : true);
	}
};
static_assert(std::is_trivially_copyable_v<PresentClock>);

}

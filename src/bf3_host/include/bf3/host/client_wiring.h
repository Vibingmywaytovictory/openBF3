#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/presentation_events.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace bf3::host {

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

PresentedOtherWeapon present_other_weapon(const PresentationFrame &p_frame, uint8_t p_slot, double p_render_ticks);

size_t present_other_weapons(const PresentationFrame &p_frame, double p_render_ticks, std::span<PresentedOtherWeapon> r_out);

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
PresentClock present_clock(const PresentationFrame &p_frame, uint64_t p_now_ns);

core::Result<std::shared_ptr<const ClientGunplayInputs>> make_client_gunplay_inputs(const SharedLevel &p_level);

}

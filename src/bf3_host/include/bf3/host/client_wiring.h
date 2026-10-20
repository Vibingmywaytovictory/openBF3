#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/present/presented.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace bf3::host {

using present::PresentClock;
using present::PresentedOtherWeapon;
using present::PresentedOwnSlot;
using present::PresentedOwnWeapons;

PresentedOtherWeapon present_other_weapon(const PresentationFrame &p_frame, uint8_t p_slot, double p_render_ticks);

size_t present_other_weapons(const PresentationFrame &p_frame, double p_render_ticks, std::span<PresentedOtherWeapon> r_out);

PresentClock present_clock(const PresentationFrame &p_frame, uint64_t p_now_ns);

class OwnWeaponsPresenter {
public:
	const PresentedOwnWeapons *present(const PresentationFrame *p_frame);

private:
	bool seen_ = false;
	net::WeaponsWire last_{};
	PresentedOwnWeapons out_{};
};

core::Result<std::shared_ptr<const ClientGunplayInputs>> make_client_gunplay_inputs(const SharedLevel &p_level);

}

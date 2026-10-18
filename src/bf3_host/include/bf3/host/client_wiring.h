#pragma once
// Client-side pieces wave 2 did not build and the wave-3 tracks cannot add within their ownership (W3.0 contract,
// docs/design/m2a-w3.md G2 and G5; written by the integrator, bodies included). Godot-free, main thread or any thread.
//
// G2: the others' weapon fields at render time. PresentationFrame::others[].samples[].soldier carries the wire's
// weapon, phase, firing, ADS, reload and burst sequences and (G1) the team, but present_others' PresentedSoldier drops
// them. present_other_weapon returns them from the sample present_others draws: the same window (OtherTrack::samples,
// at most kOtherSamples), the same selection (net::select_sample at the same render time and extrapolation limit) and
// the discrete-field rule of net::sample_other (m2a.md §6.1): in an interpolation the fields of the older sample until
// u reaches 1, then the newer one's; otherwise the selected sample's (hold before the oldest, exactly at a sample's
// tick, past the newest). So the weapon, the pose and the generation of a soldier always come from one sample.
//   tp (3p weapon, fire kick, ADS, reload start, team skin) and au3 (3p trigger and burst seed) read it.
//   firing means "fired since the previous snapshot tick" (sv, m2a-sv.md); the 3p trigger down / up and the fire kick
//   are derived from firing plus burst_seq changes, never from firing alone.
// G5: make_client_gunplay_inputs builds what ClientSessionConfig::gunplay takes from a SharedLevel loaded with
// SharedLoadOptions::gunplay. cl's client_gunplay_inputs does the work; this is the name the game wiring uses (gx's
// client_config hook), with an error that says how to fix a level loaded without gunplay. A session whose config has
// gunplay null and gunplay_from_level set builds the same inputs itself on MatchInfo; passing them explicitly lets
// restart_client reuse one table.

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

// One other soldier's weapon fields as drawn (G2).
struct PresentedOtherWeapon {
	bool valid = false; // the frame holds a track for the slot
	uint8_t slot = 0;
	uint8_t generation = 0; // of the sample the fields come from (equals PresentedSoldier::generation)
	uint16_t def = sim::weapons::kNoWeapon; // the match def id (net::OtherSoldier::weapon 255 -> kNoWeapon)
	sim::weapons::WeaponPhase phase = sim::weapons::WeaponPhase::None;
	bool firing = false; // fired since the previous snapshot tick (sv)
	bool ads = false;
	uint8_t reload_seq = 0; // 2 bits: a change = a reload started
	uint8_t burst_seq = 0; // 4 bits: a change = a new burst (au's audio seed, §4.4)
	uint8_t team = 0; // 0 unknown, 1, 2 (G1)
	uint32_t sample_tick = 0; // the server tick of the sample the fields come from
};
static_assert(std::is_trivially_copyable_v<PresentedOtherWeapon>);

// The weapon fields of p_slot at p_render_ticks (present_render_ticks of the frame's draw time). valid false when the
// frame has no samples for the slot.
PresentedOtherWeapon present_other_weapon(const PresentationFrame &p_frame, uint8_t p_slot, double p_render_ticks);
// Every other soldier at once, in present_others' order (so r_out[i] belongs to present_others' r_out[i] for the same
// frame and time): writes at most r_out.size() entries, returns the count.
size_t present_other_weapons(const PresentationFrame &p_frame, double p_render_ticks, std::span<PresentedOtherWeapon> r_out);

// The two presentation clocks of one display frame (m2a.md §8.3), in sim::weapons::SimTime units, so the main thread
// places a PresentEvent's `time` on its timeline: the effect's start age (fx3) and the sound's mixer frame (au3,
// SimMixerClock) are (now - time) and (time - now) on that clock.
//   predicted  the own soldier as the camera shows it at p_now_ns: own_prev is the state after tick - 1 (time
//              tick_end(tick - 1)), own_curr after tick, so (tick - 1 + present_own_fraction) x kTimeUnitsPerTick;
//   render     the others as present_others draws them: a sample of server tick T is the state at tick_end(T), so
//              present_render_ticks x kTimeUnitsPerTick.
// valid false when the frame has no own tick yet (predicted) or no render clock yet (render); Immediate events need
// neither.
struct PresentClock {
	bool predicted_valid = false;
	bool render_valid = false;
	sim::weapons::SimTime predicted = 0;
	sim::weapons::SimTime render = 0;
	uint64_t now_ns = 0;
	// The time on p_timeline (Immediate: 0, the caller does not map it).
	sim::weapons::SimTime now(PresentTimeline p_timeline) const {
		return p_timeline == PresentTimeline::Predicted ? predicted : (p_timeline == PresentTimeline::Render ? render : 0);
	}
	bool valid(PresentTimeline p_timeline) const {
		return p_timeline == PresentTimeline::Predicted ? predicted_valid : (p_timeline == PresentTimeline::Render ? render_valid : true);
	}
};
static_assert(std::is_trivially_copyable_v<PresentClock>);
PresentClock present_clock(const PresentationFrame &p_frame, uint64_t p_now_ns);

// G5: the client's weapon inputs from a level loaded with SharedLoadOptions::gunplay (cl's client_gunplay_inputs).
core::Result<std::shared_ptr<const ClientGunplayInputs>> make_client_gunplay_inputs(const SharedLevel &p_level);

} // namespace bf3::host

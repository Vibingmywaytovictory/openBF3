#pragma once
// One display frame of the game's audio without Godot (owner au3; docs/design/m2a-au3.md "Frame order"): what
// Bf3GameAudio::step does after it measured the output latency, so the offline E7 rehearsal and the 64-shooter run
// (audio_router_tests.cpp) drive exactly the game's order:
//   set_own_slot / set_own_first_person / set_own_trigger (own_weapons_curr's active slot kTriggerDown)
//   -> set_listener (router and impacts) -> set_others_newest (present_other_weapons at newest_tick, the horizon the
//   end of that tick) -> on_events -> on_others -> impacts -> effect sounds -> update.
// The caller set the clock's latency and bias, and set_now(p_in.clock), before.
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/presentation_events.h"

#include <array>
#include <cstdint>
#include <span>

namespace bf3::audio {

struct GameAudioInputs {
	const host::PresentationFrame *frame = nullptr; // null: no own trigger and no newest records this frame
	uint64_t now_ns = 0;
	std::span<const host::PresentEvent> events;
	std::span<const host::PresentedSoldier> others;
	std::span<const host::PresentedOtherWeapon> other_weapons; // index for index with others
	std::span<const MuzzlePoint> muzzles; // index for index with others (shorter: the rest use the stand-in)
	ListenerState listener;
	uint8_t own_slot = 0xFF;
	bool own_first_person = true; // alive and the 1p camera
};

class GameAudioDriver {
public:
	void step(WeaponAudioRouter &r_router, ImpactAudio *r_impacts, SimMixerClock &r_clock, const GameAudioInputs &p_in,
			std::span<const ImpactSoundRequest> p_impacts = {}, std::span<const EffectSoundRequest> p_effects = {});

private:
	std::array<host::PresentedOtherWeapon, net::kMaxSlots> newest_{};
};

} // namespace bf3::audio

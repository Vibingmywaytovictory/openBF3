// One display frame of the game's audio (game_audio_frame.h).

#include "bf3/audio/game_audio_frame.h"

#include "bf3/sim/weapons/weapon_sim.h"

namespace bf3::audio {

namespace sw = sim::weapons;

void GameAudioDriver::step(WeaponAudioRouter &r_router, ImpactAudio *r_impacts, SimMixerClock &r_clock, const GameAudioInputs &p_in,
		std::span<const ImpactSoundRequest> p_impacts, std::span<const EffectSoundRequest> p_effects) {
	r_router.set_own_slot(p_in.own_slot);
	r_router.set_own_first_person(p_in.own_first_person);
	if (p_in.frame != nullptr && p_in.frame->own_weapons_valid) {
		r_router.set_own_trigger((p_in.frame->own_weapons_curr.timers[0].flags & sw::slot_flags::kTriggerDown) != 0);
	}
	r_router.set_listener(p_in.listener);
	if (r_impacts != nullptr) {
		r_impacts->set_listener(p_in.listener);
	}
	if (p_in.frame != nullptr && p_in.frame->valid) {
		const size_t n = host::present_other_weapons(*p_in.frame, double(p_in.frame->newest_tick), newest_);
		r_router.set_others_newest(std::span<const host::PresentedOtherWeapon>(newest_.data(), n),
				sw::SimTime(p_in.frame->newest_tick + 1) * sw::kTimeUnitsPerTick);
	}
	r_router.on_events(p_in.events, r_clock);
	r_router.on_others(p_in.others, p_in.other_weapons, p_in.muzzles, r_clock);
	if (r_impacts != nullptr) {
		for (const ImpactSoundRequest &r : p_impacts) {
			r_impacts->play_impact(r, r_clock);
		}
		for (const EffectSoundRequest &e : p_effects) {
			r_impacts->on_effect_sound(e, r_clock);
		}
	}
	r_router.update(p_in.now_ns);
}

} // namespace bf3::audio

#pragma once
// The weapon seam (M2a audio A2; docs/design/m2a-audio.md §6.7): how gameplay fire events become patch inputs. The
// game (bf3_sim firing, later M2a integration) calls this, not the patch directly, so the event model has one place.
//
// Binding: WeaponDef::sound (gameplay/weapon_def.h, "partition of the fire sound") names the patch [verified: M16A3
// -> sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02, RPG-7 -> sound/weapons/handheld/rpg-7/
// weapon_at_rpg7]. The fire interval is gameplay::shot_interval(FireFunction) (60 / RateOfFire: M16A3 800 rpm = 75 ms).
//
// Event model [inferred, design §6.3; open question 4]: trigger down plays the first round with the start event
// (Fire = DefaultStartEvent); each further round of the same burst posts the Sampler step event (#1968287961 on the
// M16A3, EnableStep = 1 while the trigger is held) on its own frame, which starts the next loop segment on the other
// sampler slot (two slots: 94.7 ms segments overlap at 75 ms); trigger up posts DefaultStopEvent, which plays the
// tail segment. IsFirstPerson is a parameter set per instance.

#include "bf3/audio/mixer_core.h"
#include "bf3/core/audio/sound_program.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace bf3::audio {

// ebx_name_hash (djb2-xor) of the names the seam uses [verified against the M16A3 graph's InputEvents/Parameters].
inline constexpr uint32_t kHashFire = 2088671869u;
inline constexpr uint32_t kHashIsFirstPerson = 824639024u;
inline constexpr uint32_t kHashReload = 3294210644u;
inline constexpr uint32_t kHashAreaType = 3772263626u;

struct WeaponSoundBinding {
	std::shared_ptr<const core::audio::SoundProgram> program;
	uint32_t start_event = 0; // DefaultStartEvent
	uint32_t stop_event = 0; // DefaultStopEvent
	std::optional<uint32_t> step_event; // the Sampler Step event when the patch has one
	std::optional<uint32_t> enable_step_parameter;
	uint64_t shot_interval_frames = 0; // at the mix rate
};

// Finds the start/stop/step inputs in a compiled weapon patch. Fails when the program has no start event.
core::Result<WeaponSoundBinding> bind_weapon_sound(std::shared_ptr<const core::audio::SoundProgram> p_program, double p_shot_interval_seconds,
		uint32_t p_mix_rate);

// Turns trigger state into mixer commands for one weapon instance (game thread).
class WeaponSoundDriver {
public:
	WeaponSoundDriver(const WeaponSoundBinding &p_binding, InstanceHandle p_instance);
	// p_frame: the MixerClock frame of the event (the game converts its fire time to mix frames).
	void trigger_down(MixerCore &r_mixer, uint64_t p_frame, bool p_first_person);
	void round_fired(MixerCore &r_mixer, uint64_t p_frame); // second and later rounds of a burst
	void trigger_up(MixerCore &r_mixer, uint64_t p_frame);

private:
	WeaponSoundBinding binding_;
	InstanceHandle instance_ = kNoInstance;
	bool held_ = false;
};

} // namespace bf3::audio

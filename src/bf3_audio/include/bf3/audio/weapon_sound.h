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
// M16A3) on its own frame, which starts the next loop segment on the other sampler slot (94.7 ms segments, about
// 75-81 ms after the burst's pitch of 1.16-1.26, overlap at 75 ms); trigger up posts DefaultStopEvent, which
// releases the loops into their tails. The unstepped noise layers loop on their own (patch_instance.h).
//
// Fire mode [verified on the M16A3 graph, design §6.3]: the EnableStep parameter (#4198762218) is also the mode
// selector. With 0, the gated Fire is routed to the release register in the same evaluation (a single shot per
// Fire); with 1, Steps are accepted and the stop event releases. The driver sets it once per instance, before Start,
// from the weapon's fire mode, and never toggles it during a burst (dropping it lets the filtered DeMux index fall
// below 1 within 3 s and the stop event would no longer release the loops). Single fire posts Fire per round.
// IsFirstPerson is set per instance before Start as well.
//
// Frames: trigger_down takes the burst's first frame (MixerCore::earliest_frame() when the game has no earlier
// prediction); round k is stamped first + round(k x shot interval x rate), computed from k, so spacing is exact.

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

enum class WeaponFireMode : uint8_t { Single, Burst, Automatic };

struct WeaponSoundBinding {
	std::shared_ptr<const core::audio::SoundProgram> program;
	uint32_t start_event = 0; // DefaultStartEvent
	uint32_t stop_event = 0; // DefaultStopEvent
	std::optional<uint32_t> step_event; // the Sampler Step event when the patch has one
	std::optional<uint32_t> enable_step_parameter;
	double shot_interval_seconds = 0.0; // gameplay::shot_interval(FireFunction)
	uint32_t mix_rate = 0;
};

// Finds the start/stop/step inputs in a compiled weapon patch. Fails when the program has no start event.
core::Result<WeaponSoundBinding> bind_weapon_sound(std::shared_ptr<const core::audio::SoundProgram> p_program, double p_shot_interval_seconds,
		uint32_t p_mix_rate);

// Turns trigger state into mixer commands for one weapon instance (game thread). Pushes that the ring refuses are
// reported by the false return; the caller retries on its next frame (the frames stay those of the shots).
class WeaponSoundDriver {
public:
	WeaponSoundDriver(const WeaponSoundBinding &p_binding, WeaponFireMode p_mode, InstanceHandle p_instance);
	// Before the Start command: IsFirstPerson and EnableStep (0 for Single, 1 otherwise).
	void prepare(PatchInstance &r_instance, bool p_first_person) const;
	// p_first_frame: the MixerClock frame of round 0.
	bool trigger_down(MixerCore &r_mixer, uint64_t p_first_frame);
	// Round p_round >= 1 of the burst, at first + round(p_round x interval x rate): Step (Single: Fire).
	bool round_fired(MixerCore &r_mixer, uint32_t p_round);
	bool trigger_up(MixerCore &r_mixer, uint64_t p_frame);
	uint64_t round_frame(uint32_t p_round) const;

private:
	WeaponSoundBinding binding_;
	WeaponFireMode mode_ = WeaponFireMode::Automatic;
	InstanceHandle instance_ = kNoInstance;
	uint64_t first_frame_ = 0;
	bool held_ = false;
};

} // namespace bf3::audio

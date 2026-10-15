#pragma once
// The M16A3 check (M2a audio; docs/design/m2a-audio.md §10.4, gates G7 and G8): one script, one driver and one
// analysis, shared by the offline render (G7, tests/audio_install_tests.cpp) and the Godot run (G8, the demo node of
// res://scenes/audio_m16.tscn). The Godot recording is therefore measured with exactly the rules of the offline one,
// and the two are compared sample by sample.
//
// Why these distances and taps [verified on the M16A3 graph, design §4.5; under crossfader reading R]:
//   - third person (IsFirstPerson 0): the LFE sampler (voices 24-25 -> voice 4 -> LFE bus) and the rattle loop are
//     triggered only in first person (Switcher on IsFirstPerson, case 1 -> v78), so no LFE path is active;
//   - 0.5 m: Fire starts corebassclose (voices 28-29), hifi_lmg (18-19) and noise_close (15-16), all stepped;
//   - 60 m and 150 m: Fire starts corebassdistant (stepped, voices 26-27 -> voice 6, HPF v49) and noise_far
//     (UNstepped LtRelease loop, voices 20-21 -> voice 14, HPF v46); voices 3 and 5 are at gain 0 there (v59 = 0.8 x
//     IsFirstPerson, v103 = ScaleClamp(1..3 m -> 0.5..0)); the far reflections (voice 8 -> voice 2, no HPF) start
//     only at the stop event;
//   - so onsets are measured on the core tap (voices 26-29: the two stepped corebass samplers, one of which is
//     audible at each distance, neither with a Delay port), not on the full mix, where the unstepped noise loop adds
//     its own segment onsets about 75-81 ms apart and the hifi/noise layers carry random 0-45 ms delays;
//   - the HPF rule pins both distance high-passes (v46 on voice 14 plugin 2, v49 on voice 6 plugin 1) to 0 Hz in the
//     far_pinned segment, which replays the far segment's first burst with the same seed: inside the burst window
//     every audible path then differs only by those two filters.

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::audio {

// M16A3 graph facts the check relies on; CheckDriver::create verifies each against the compiled program (a link to
// that plugin driven by the expected ScaleClamp register) and fails if the data says otherwise.
inline constexpr std::string_view kM16Patch = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";
inline constexpr core::audio::SoundPluginRef kM16NoiseHpf{ 14, 2 }; // HighPassIir2, Frequency = v46
inline constexpr core::audio::SoundPluginRef kM16CoreHpf{ 6, 1 }; // HighPassIir2, Frequency = v49
inline constexpr std::array<uint16_t, 4> kM16CoreTapVoices{ 26, 27, 28, 29 };
inline constexpr std::array<uint16_t, 1> kM16NoiseTapVoices{ 14 };

struct CheckSegment {
	std::string name; // near, mid, far, far_pinned
	float distance_m = 0.0f; // emitter on the listener's forward axis, facing it (on the Cone axis)
	bool first_person = false;
	uint32_t bursts = 1;
	uint64_t seed = 0; // burst b of the segment uses seed + b
	bool pin_distance_hpfs = false; // kM16NoiseHpf and kM16CoreHpf pinned to 0 Hz
};

struct CheckScript {
	std::vector<CheckSegment> segments;
	uint32_t rounds = 10;
	double shot_interval_s = 0.075; // gameplay::shot_interval of the M16A3 (800 rpm) = 3600 frames
	double stop_after_last_round_s = 0.0375; // trigger up half an interval after the last round
	double burst_period_s = 4.0; // > the longest tail (reflections 3.1-3.7 s at pitch >= 1.16)
	double start_s = 0.25;
	WeaponFireMode fire_mode = WeaponFireMode::Automatic;
	double expected_spacing_frames = 3600.0;
	double spacing_tolerance_frames = 48.0; // 1 ms
};

// near 0.5 m x 2, mid 60 m x 2, far 150 m x 2, far_pinned 150 m x 1 (seed of far's first burst); third person;
// 7 bursts, 28.25 s. G8 runs it inside its 30 s window.
CheckScript m16_check_script();

struct ScheduledBurst {
	uint32_t segment = 0;
	uint32_t index = 0; // within the segment
	uint64_t seed = 0;
	uint64_t first_round_frame = 0; // relative to the script start
	uint64_t stop_frame = 0; // the burst window is [first_round_frame, stop_frame)
};
std::vector<ScheduledBurst> schedule_check(const CheckScript &p_script, uint32_t p_rate);

// Mono recorder for a VoiceTap, preallocated for the script length; adds at absolute positions (overlapping
// instances sum), counts frames outside the buffer.
class TapRecorder final : public VoiceTap {
public:
	TapRecorder(uint64_t p_start_frame, uint64_t p_frames);
	void on_voices(const float *p_mono, uint32_t p_frames, uint64_t p_first_frame) override;
	std::span<const float> samples() const;
	uint64_t dropped_frames() const;

private:
	uint64_t start_ = 0;
	std::vector<float> buffer_;
	uint64_t dropped_ = 0;
};

// Game-thread driver: per burst, creates one PatchInstance (seed, IsFirstPerson and EnableStep through
// WeaponSoundDriver::prepare, emitter/listener, pins, the core and noise taps, a sampler trace) a little before its
// frame, and pushes Start, the rounds, the stop and finally Stop through the mixer's command ring. pump() is called
// once per game frame in Godot and before every mix() call offline; it never pushes a command later than
// MixerCore::earliest_frame() allows, so late_commands stays 0 unless the game thread stalls.
class CheckDriver {
public:
	static core::Result<std::unique_ptr<CheckDriver>> create(const CheckScript &p_script, std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, uint64_t p_start_frame, uint32_t p_rate);
	core::Status pump(MixerCore &r_mixer);
	bool done(const MixerCore &p_mixer) const; // every burst stopped and its tail past
	uint64_t start_frame() const;
	uint64_t total_frames() const;
	std::span<const ScheduledBurst> bursts() const;
	const TapRecorder &core_tap() const;
	const TapRecorder &noise_tap() const;
	std::span<const SamplerTrace> traces() const; // one per burst
	// plugin_attribute(kM16NoiseHpf, 0) as evaluated in the far segment (data: 225.3 Hz at 150 m), for the prediction.
	float far_noise_hpf_hz() const;

private:
	CheckDriver() = default;
	struct State;
	std::unique_ptr<State> state_;
};

struct CheckRow {
	std::string name;
	double value = 0.0;
	std::string rule;
	bool pass = false;
};
struct CheckReport {
	std::vector<CheckRow> rows;
	bool pass() const;
	std::string table() const; // "PASS|FAIL name value rule" lines, the format the ctest scripts parse
};

// The rules G7 and G8 share (design §10.4):
//   onsets: per burst, on the core tap: exactly `rounds` onsets in [first_round_frame - 10 ms, stop_frame), each gap
//     expected_spacing_frames +- spacing_tolerance_frames;
//   levels: full-mix RMS over the burst windows, mean per segment: near > mid > far, each step >= 3 dB;
//   HPF (full mix): 20-100 Hz band energy of far's first burst window >= 10 dB below far_pinned's;
//   HPF (noise tap, voice 14): the same comparison on the tap >= 10 dB, and the far tap's measured band energy within
//     1.5 dB of predicted_highpass_band_db(far_pinned tap, cutoff = p_far_noise_hpf_hz, iir2_q).
// p_mix_stereo, p_core_tap and p_noise_tap start at the driver's start frame.
CheckReport analyse_check(const CheckScript &p_script, std::span<const ScheduledBurst> p_bursts, std::span<const float> p_mix_stereo,
		std::span<const float> p_core_tap, std::span<const float> p_noise_tap, float p_far_noise_hpf_hz, uint32_t p_rate, const AudioTuning &p_tuning);

} // namespace bf3::audio

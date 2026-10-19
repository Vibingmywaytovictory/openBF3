#pragma once
// The HDR loudness window (docs/design/m2b.md §6.1, §2.6 #15; declared in W1.0, owner au). Runs on the audio thread
// inside MixerCore, once per mix block; allocation-free after configure().
//
// Per block: top target = max(WindowMinTop, the loudest active voice's loudness at the listener); the top moves
// towards it with the attack (top_attack_s) or the release (top_min_release_s .. top_max_release_s on the setting's
// curve); bottom = max(WindowMinBottom, top - WindowSize); each voice's gain = loudness - top - Headroom dB (the headroom
// applies to the amplitudes, as patch_instance.cpp's fixed window does today, R5); voices below the bottom are
// suspended (silent, still advancing). E10 (audio.hdr.unit): a 71 dB footstep alone -> top 80, bottom 35; a 110 dB
// shot -> top 110 within 0.05 s (+- one block), bottom 65, a 63 dB source suspended.
//
// As built (au, docs/design/m2b-au.md "HDR window"; every rule below that the data does not state is [inferred],
// measured = false):
//   - attack: a linear ramp in dB that reaches the target top_attack_s after it started (a higher target during the
//     ramp restarts it from the current top); 0 s = at once. 80 -> 110 dB takes 5 blocks (53.3 ms);
//   - release: a fall from the top where it started (release_from) to the target over a duration
//     T = lerp(top_min_release_s, top_max_release_s, clamp(drop / WindowSize, 0, 1)) (a small dip releases in about
//     0.035 s, a full window in 0.85 s), shaped by WindowTopReleaseTimeCurve read as the fraction of the drop still
//     left at normalised time x = elapsed / T, rescaled so it runs from exactly 1 at x = 0 to 0 at x = 1 (the retail
//     curve: 0.976 .. 0.008); no curve = linear. A new target during the fall restarts it from the current top;
//   - bottom: towards max(WindowMinBottom, top - WindowSize), rising at WindowSize / WindowBottomAttackTime dB/s (0 s:
//     at once) and falling at WindowSize / WindowBottomReleaseTime dB/s (0.025 s: 1,800 dB/s);
//   - AllowedOvershoot: a voice louder than the top (during the attack) sounds at most AllowedOvershoot above it:
//     gain = min(loudness, top + AllowedOvershoot) - top - Headroom;
//   - pre-cull edge (voice_cull.h step 1): the bottom of the last step() - 3 dB, read before this block's step().
// CompressFactor, DischargeFactor and MaxAllowedEnergy are not applied (Q9).

#include "bf3/core/audio/hdr_setting.h"

#include <cstdint>

namespace bf3::audio {

class HdrWindow {
public:
	HdrWindow() = default;
	void configure(const core::audio::HdrSetting &p_setting, uint32_t p_mix_rate); // game thread, before the first block
	void reset(); // top = WindowMinTop, bottom = WindowMinBottom
	// One block: p_loudest_db = the loudest active HDR voice's loudness at the listener (below WindowMinTop when none);
	// p_frames = the block's frames.
	void step(float p_loudest_db, uint32_t p_frames);
	float top_db() const { return top_db_; }
	float bottom_db() const { return bottom_db_; }
	float gain_db(float p_loudness_db) const; // min(loudness, top + overshoot) - top - headroom
	bool suspended(float p_loudness_db) const; // below the bottom
	float precull_db() const; // the bottom of the last step() - 3 dB (voice_cull.h step 1)
	const core::audio::HdrSetting &setting() const { return setting_; }
	// The release duration for a fall of p_drop_db (as built, see above).
	float release_seconds(float p_drop_db) const;
	// The release shape: the fraction of the drop left at normalised time p_x in [0, 1] (1 at 0, 0 at 1).
	float release_shape(float p_x) const;

private:
	enum class Phase : uint8_t { Hold, Attack, Release };
	core::audio::HdrSetting setting_;
	uint32_t mix_rate_ = 48000;
	float top_db_ = 80.0f;
	float bottom_db_ = 35.0f;
	Phase phase_ = Phase::Hold;
	float ramp_from_db_ = 80.0f; // where the current attack or release started (the curve's origin)
	float ramp_to_db_ = 80.0f; // its target
	float ramp_seconds_ = 0.0f; // its duration
	float ramp_elapsed_ = 0.0f;
	float curve_y0_ = 1.0f, curve_y1_ = 0.0f; // the curve at x = 0 and x = 1 (the rescale)
};

} // namespace bf3::audio

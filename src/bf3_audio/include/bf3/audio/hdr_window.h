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
	float gain_db(float p_loudness_db) const; // loudness - top - headroom
	bool suspended(float p_loudness_db) const; // below the bottom
	float precull_db() const; // the previous block's bottom - 3 dB (voice_cull.h step 1)

private:
	core::audio::HdrSetting setting_;
	uint32_t mix_rate_ = 48000;
	float top_db_ = 80.0f;
	float bottom_db_ = 35.0f;
	float previous_bottom_db_ = 35.0f;
	float release_from_db_ = 80.0f; // where the current release started (the curve's x)
};

} // namespace bf3::audio
